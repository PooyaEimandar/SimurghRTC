// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativePeerService.h"
#include "Engine/World.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace SimurghRTC
{
namespace
{
FNativePeerSnapshot ReadSnapshot(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State)
{
    FScopeLock Lock(&State->Mutex);
    return State->Snapshot;
}

void PublishClosed(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State)
{
    FScopeLock Lock(&State->Mutex);
    State->Snapshot.State = ENativePeerState::Closed;
    State->Snapshot.Diagnostics = {};
}
}

FNativePeer::FNativePeer(TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe> InState,
    TWeakPtr<FNativePeerService, ESPMode::ThreadSafe> InService)
    : SharedState(MoveTemp(InState)), Service(MoveTemp(InService)) {}

FNativePeer::~FNativePeer() { Close(); }

FNativePeerSnapshot FNativePeer::GetSnapshot() const
{
    check(IsInGameThread());
    return ReadSnapshot(SharedState);
}

void FNativePeer::Close()
{
    check(IsInGameThread());
    if (auto Pinned = Service.Pin()) Pinned->Close(SharedState);
}

FNativePeerService::FNativePeerService()
{
    check(IsInGameThread());
    Wake = FPlatformProcess::GetSynchEventFromPool(false);
}

FNativePeerService::~FNativePeerService()
{
    Shutdown();
    FPlatformProcess::ReturnSynchEventToPool(Wake);
}

bool FNativePeerService::StartWorker(FString& OutError)
{
    check(IsInGameThread());
    if (bShuttingDown) { OutError = TEXT("Native peer service is shutting down."); return false; }
    if (Thread) return true;
    if (!FPlatformProcess::SupportsMultithreading())
    { OutError = TEXT("Native peers require multithreading."); return false; }
    Thread.Reset(FRunnableThread::Create(this, TEXT("SimurghRTC.NativeOwner")));
    if (!Thread) { OutError = TEXT("Could not start the native peer worker."); return false; }
    return true;
}

FNativePeer::FHandle FNativePeerService::Open(UObject* Owner, ENativeFactoryMode Mode,
    FNativePeerOpened OnOpened, FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    if (bShuttingDown) { OutError = TEXT("Native peer service is shutting down."); return {}; }
    if (!IsValid(Owner)) { OutError = TEXT("A live native peer owner is required."); return {}; }
    if (Mode != ENativeFactoryMode::DataOnly && Mode != ENativeFactoryMode::BuiltinMedia)
    { OutError = TEXT("Invalid native factory mode."); return {}; }
    if (!FNativeRuntime::IsSupported())
    { OutError = TEXT("The isolated native backend is not available for this target."); return {}; }
    if (Entries.Num() >= MaxPeers)
    { OutError = TEXT("Native peer capacity reached; wait for pending peers to close."); return {}; }
    if (!StartWorker(OutError)) return {};

    auto State = MakeShared<FNativePeerSharedState, ESPMode::ThreadSafe>();
    State->Id = NextId++;
    FNativePeer::FHandle Peer(new FNativePeer(State, AsShared()));
    auto Entry = MakeShared<FEntry>();
    Entry->State = State;
    Entry->Peer = Peer;
    Entry->Owner = Owner;
    Entry->World = Owner->GetWorld();
    Entry->OnOpened = MoveTemp(OnOpened);
    Entries.Add(Entry);
    Enqueue({ECommand::Open, State, Mode});
    return Peer;
}

void FNativePeerService::Close(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State)
{
    check(IsInGameThread());
    {
        FScopeLock Lock(&State->Mutex);
        if (State->Snapshot.State == ENativePeerState::Closing || State->Snapshot.State == ENativePeerState::Closed) return;
        State->Snapshot.State = ENativePeerState::Closing;
        State->Snapshot.Diagnostics = {};
    }
    Enqueue({ECommand::Close, State});
}

void FNativePeerService::Tick()
{
    check(IsInGameThread());
    if (bShuttingDown) return;
    // Callbacks may open/close peers, collect their owners, or shut down this service.
    const auto Pending = Entries;
    for (const auto& Entry : Pending)
    {
        if (bShuttingDown) break;
        auto Peer = Entry->Peer.Pin();
        if (!Entry->Owner.IsValid() || !Peer) Close(Entry->State.ToSharedRef());
        const auto Snapshot = ReadSnapshot(Entry->State.ToSharedRef());
        if (Snapshot.State == ENativePeerState::Closing || Snapshot.State == ENativePeerState::Closed)
        {
            Entry->OnOpened = nullptr;
        }
        else if (Snapshot.State == ENativePeerState::Ready || Snapshot.State == ENativePeerState::Failed)
        {
            auto Callback = MoveTemp(Entry->OnOpened);
            if (Callback) Callback(Snapshot);
        }
    }
    Entries.RemoveAll([](const TSharedRef<FEntry>& Entry)
    { return ReadSnapshot(Entry->State.ToSharedRef()).State == ENativePeerState::Closed; });
}

void FNativePeerService::CloseWorld(UWorld* World)
{
    check(IsInGameThread());
    if (!World || bShuttingDown) return;
    for (const auto& Entry : Entries)
        if (Entry->World.Get() == World) Close(Entry->State.ToSharedRef());
}

void FNativePeerService::Shutdown()
{
    check(IsInGameThread());
    if (bShuttingDown) return;
    bShuttingDown = true;
    for (const auto& Entry : Entries) Close(Entry->State.ToSharedRef());
    if (Thread)
    {
        // FIFO shutdown follows every close. Never abandon a runtime on the game thread.
        Enqueue({ECommand::Shutdown});
        Thread->WaitForCompletion();
        Thread.Reset();
    }
    // Destroy callback captures on the game thread, after all peers have been closed.
    Entries.Reset();
}

int32 FNativePeerService::GetPendingPeerCount() const
{
    check(IsInGameThread());
    return Entries.Num();
}

void FNativePeerService::Enqueue(FCommand Command)
{
    check(IsInGameThread());
    check(Thread);
    Commands.Enqueue(MoveTemp(Command));
    Wake->Trigger();
}

uint32 FNativePeerService::Run()
{
    check(!IsInGameThread());
    // These objects, including their last references, never leave this OS thread.
    TMap<uint64, TUniquePtr<FNativeRuntime>> Runtimes;
    for (;;)
    {
        Wake->Wait();
        FCommand Command;
        while (Commands.Dequeue(Command))
        {
            if (Command.Type == ECommand::Shutdown)
            {
                check(Runtimes.IsEmpty());
                return 0;
            }
#if WITH_DEV_AUTOMATION_TESTS
            if (Command.Type == ECommand::TestGate)
            {
                Command.Entered->Trigger();
                Command.Release->Wait();
                continue;
            }
#endif
            const auto State = Command.State.ToSharedRef();
            if (Command.Type == ECommand::Close)
            {
                Runtimes.Remove(State->Id);
                PublishClosed(State);
                continue;
            }
            if (ReadSnapshot(State).State == ENativePeerState::Closing) continue;

            auto Runtime = MakeUnique<FNativeRuntime>();
            FString Error;
            const bool bReady = Runtime->Start(Command.Mode, Error) && Runtime->CreateProbePeerConnection(Error);
            const auto Diagnostics = bReady ? Runtime->GetDiagnostics() : FNativeFactoryDiagnostics{};
            if (!bReady) Runtime.Reset();
            bool bCancelled;
            {
                FScopeLock Lock(&State->Mutex);
                bCancelled = State->Snapshot.State == ENativePeerState::Closing;
                if (!bCancelled)
                {
                    if (bReady) Runtimes.Add(State->Id, MoveTemp(Runtime));
                    State->Snapshot.State = bReady ? ENativePeerState::Ready : ENativePeerState::Failed;
                    State->Snapshot.Error = MoveTemp(Error);
                    State->Snapshot.Diagnostics = Diagnostics;
                }
            }
            if (bCancelled)
            {
                Runtime.Reset();
                PublishClosed(State);
            }
        }
    }
}

#if WITH_DEV_AUTOMATION_TESTS
bool FNativePeerService::HoldWorkerForTesting(FEvent& Entered, FEvent& Release)
{
    FString Error;
    if (!StartWorker(Error)) return false;
    FCommand Command;
    Command.Type = ECommand::TestGate;
    Command.Entered = &Entered;
    Command.Release = &Release;
    Enqueue(MoveTemp(Command));
    return true;
}
#endif
}
