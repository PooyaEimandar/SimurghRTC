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
    State->Snapshot.Transport.Connection = ENativeConnectionState::Closed;
    State->Snapshot.Transport.bDataTransportOpen = false;
    State->Events.Reset();
}

bool PublishEvent(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State, FNativeTransportEvent Event)
{
    FScopeLock Lock(&State->Mutex);
    if (State->Snapshot.State != ENativePeerState::Ready) return false;
    if (Event.Type != ENativeSignalEvent::None && State->Events.Num() >= 64)
    {
        Event = {}; Event.Type = ENativeSignalEvent::FatalError;
        Event.Result.Error = ENativeSignalError::ResourceExhausted;
        Event.Result.Message = TEXT("Game-thread signaling event queue is full.");
    }
    if (Event.Type == ENativeSignalEvent::FatalError)
    {
        State->Events.Reset(); State->Snapshot.State = ENativePeerState::Failed;
        State->Snapshot.Error = Event.Result.Message; State->Snapshot.Diagnostics = {};
        Event.Transport.Connection = ENativeConnectionState::Failed;
        Event.Transport.bDataTransportOpen = false;
    }
    State->Snapshot.Transport = Event.Transport;
    const bool bKeep = Event.Type != ENativeSignalEvent::FatalError;
    if (Event.Type != ENativeSignalEvent::None) State->Events.Add(MoveTemp(Event));
    return bKeep;
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

bool FNativePeer::Signal(FNativeSignalRequest Request, FNativeSignalCompleted OnComplete, FString& OutError)
{
    check(IsInGameThread());
    if (auto Pinned = Service.Pin()) return Pinned->Signal(SharedState, MoveTemp(Request), MoveTemp(OnComplete), OutError);
    OutError = TEXT("Native peer service is unavailable."); return false;
}
void FNativePeer::SetEventHandler(FNativeTransportChanged OnEvent)
{
    check(IsInGameThread());
    if (auto Pinned = Service.Pin()) Pinned->SetEventHandler(SharedState, MoveTemp(OnEvent));
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
    FNativePeerOpened OnOpened, FString& OutError, bool bLoopbackProbe)
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
    Entry->Data = MakeUnique<FNativeDataGame>(State->Data);
    Entries.Add(Entry);
    Enqueue({ECommand::Open, State, Mode, bLoopbackProbe});
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
    // Provider modules can close during shutdown, before another game tick runs.
    // Release their callback captures synchronously on this thread; native teardown stays asynchronous.
    for (const auto& Entry : Entries) if (Entry->State == State)
    {
        Entry->OnOpened = nullptr; Entry->Operations.Reset(); Entry->PendingSdp = 0; Entry->OnEvent.Reset();
        Entry->Data->Shutdown();
        break;
    }
    Enqueue({ECommand::Close, State});
}

bool FNativePeerService::Signal(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State,
    FNativeSignalRequest Request, FNativeSignalCompleted OnComplete, FString& OutError)
{
    check(IsInGameThread()); OutError.Reset();
    if (bShuttingDown || ReadSnapshot(State).State != ENativePeerState::Ready)
    { OutError = TEXT("Native peer is not ready."); return false; }
    auto Found = Entries.FindByPredicate([&](const TSharedRef<FEntry>& E) { return E->State == State; });
    if (!Found || !(*Found)->Owner.IsValid() || !(*Found)->Peer.IsValid())
    { OutError = TEXT("Native peer owner is no longer live."); return false; }
    auto Entry = *Found;
    auto Bounded = [](const FString& Value, int32 Limit)
    {
        if (Value.Len() > Limit) return false;
        for (int32 I = 0; I < Value.Len(); ++I) if (!Value[I]) return false;
        return FTCHARToUTF8(*Value).Length() <= Limit;
    };
    if (Request.Action < ENativeSignalAction::CreateOffer || Request.Action > ENativeSignalAction::EndIce
        || !Request.TimeoutMs || Request.TimeoutMs > 300000 || !Bounded(Request.Sdp, 131072)
        || !Bounded(Request.Ice.Candidate, 2048) || !Bounded(Request.Ice.Mid, 32)
        || !Bounded(Request.Ice.UsernameFragment, 256))
    { OutError = TEXT("Invalid signaling action, timeout or text size."); return false; }
    const bool bSdp = Request.Action <= ENativeSignalAction::SetRemote;
    if (Entry->Operations.Num() >= 64 || (bSdp && Entry->PendingSdp))
    { OutError = TEXT("Signaling capacity reached or another SDP operation is pending."); return false; }
    FCommand Command; Command.Type = ECommand::Signal; Command.State = State;
    Command.OperationId = Entry->NextOperation++; Command.Deadline = FPlatformTime::Seconds() + Request.TimeoutMs / 1000.0;
    if (bSdp) Entry->PendingSdp = Command.OperationId;
    Entry->Operations.Add(Command.OperationId, MoveTemp(OnComplete));
    Command.Request = MoveTemp(Request); Enqueue(MoveTemp(Command)); return true;
}
void FNativePeerService::SetEventHandler(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State,
    FNativeTransportChanged OnEvent)
{
    check(IsInGameThread());
    for (const auto& Entry : Entries) if (Entry->State == State)
    { if (OnEvent) Entry->OnEvent = MakeShared<FNativeTransportChanged>(MoveTemp(OnEvent)); else Entry->OnEvent.Reset(); return; }
}
void FNativePeerService::Tick()
{
    check(IsInGameThread());
    if (bShuttingDown) return;
    // Recheck before every callback: earlier callbacks may close peers, GC owners or shut down the module.
    const auto Pending = Entries;
    for (const auto& Entry : Pending)
    {
        auto Live = [&]()
        {
            if (bShuttingDown) return false;
            if (!Entry->Owner.IsValid() || !Entry->Peer.IsValid()) Close(Entry->State.ToSharedRef());
            const auto State = ReadSnapshot(Entry->State.ToSharedRef()).State;
            return State != ENativePeerState::Closing && State != ENativePeerState::Closed;
        };
        if (!Live())
        { Entry->OnOpened = nullptr; Entry->Operations.Reset(); Entry->OnEvent.Reset(); continue; }
        const auto Snapshot = ReadSnapshot(Entry->State.ToSharedRef());
        if (Snapshot.State == ENativePeerState::Ready || Snapshot.State == ENativePeerState::Failed)
        { auto Callback = MoveTemp(Entry->OnOpened); if (Callback) Callback(Snapshot); }
        if (Live()) Entry->Data->Tick(Live);
        TArray<FNativeTransportEvent> Events;
        { FScopeLock Lock(&Entry->State->Mutex); Events = MoveTemp(Entry->State->Events); }
        for (const auto& Event : Events)
        {
            if (!Live()) break;
            if (Event.Type == ENativeSignalEvent::OperationComplete)
            {
                FNativeSignalCompleted Callback;
                Entry->Operations.RemoveAndCopyValue(Event.Result.OperationId, Callback);
                if (Entry->PendingSdp == Event.Result.OperationId) Entry->PendingSdp = 0;
                if (Callback) Callback(Event.Result);
            }
            else
            {
                if (Event.Type == ENativeSignalEvent::FatalError)
                {
                    auto Operations = MoveTemp(Entry->Operations); Entry->PendingSdp = 0;
                    for (auto& Operation : Operations)
                    {
                        if (!Live()) break;
                        auto Result = Event.Result; Result.OperationId = Operation.Key;
                        if (Operation.Value) Operation.Value(Result);
                    }
                }
                if (Live()) { auto Callback = Entry->OnEvent; if (Callback) (*Callback)(Event); }
            }
        }
        if (!Live()) { Entry->OnOpened = nullptr; Entry->Operations.Reset(); Entry->OnEvent.Reset(); }
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
    struct FSlot
    {
        TUniquePtr<FNativeRuntime> Runtime;
        TSharedPtr<FNativePeerSharedState, ESPMode::ThreadSafe> State;
        bool bSignaling = false;
        TUniquePtr<FNativeDataWorker> Data;
    };
    TMap<uint64, FSlot> Runtimes;
    bool bPolling = false;
    for (;;)
    {
        Wake->Wait(!Commands.IsEmpty() ? 0 : (bPolling ? 10 : MAX_uint32));
        FCommand Command;
        // Bound command batches so an active peer continues draining native events.
        for (int32 Batch = 0; Batch < 16 && Commands.Dequeue(Command); ++Batch)
        {
            if (Command.Type == ECommand::Shutdown) { check(Runtimes.IsEmpty()); return 0; }
#if WITH_DEV_AUTOMATION_TESTS
            if (Command.Type == ECommand::TestGate)
            { Command.Entered->Trigger(); Command.Release->Wait(); continue; }
#endif
            const auto State = Command.State.ToSharedRef();
            if (Command.Type == ECommand::Close)
            { Runtimes.Remove(State->Id); PublishClosed(State); continue; }
            const auto Status = ReadSnapshot(State).State;
            if (Status == ENativePeerState::Closing || Status == ENativePeerState::Closed) continue;
            if (Command.Type == ECommand::Data)
            {
                if (auto* Slot = Runtimes.Find(State->Id))
                { Slot->bSignaling = true; Slot->Data->Submit(MoveTemp(Command.Data)); }
                continue;
            }
            if (Command.Type == ECommand::Signal)
            {
                auto* Slot = Runtimes.Find(State->Id);
                if (!Slot) continue; // A fatal event already completes pending operations on the game thread.
                Slot->bSignaling = true;
                FNativeTransportEvent Event; Event.Type = ENativeSignalEvent::OperationComplete;
                Event.Result.OperationId = Command.OperationId;
                Event.Transport = ReadSnapshot(State).Transport;
                const double Remaining = Command.Deadline - FPlatformTime::Seconds();
                if (Remaining <= 0)
                { Event.Result.Error = ENativeSignalError::TimedOut; Event.Result.Message = TEXT("Signaling request expired in the worker queue."); }
                else
                {
                    Command.Request.TimeoutMs = FMath::Max(1u, static_cast<uint32>(Remaining * 1000));
                    if (Slot->Runtime->Signal(Command.OperationId, Command.Request, Event.Result)) continue;
                }
                if (!PublishEvent(State, MoveTemp(Event))) Runtimes.Remove(State->Id);
                continue;
            }
            auto Runtime = MakeUnique<FNativeRuntime>();
            FString Error;
            const bool bReady = Runtime->Start(Command.Mode, Error) && Runtime->CreateProbePeerConnection(Error, Command.bLoopbackProbe);
            const auto Diagnostics = bReady ? Runtime->GetDiagnostics() : FNativeFactoryDiagnostics{};
            if (!bReady) Runtime.Reset();
            bool bCancelled;
            {
                FScopeLock Lock(&State->Mutex);
                bCancelled = State->Snapshot.State == ENativePeerState::Closing;
                if (!bCancelled)
                {
                    if (bReady)
                    {
                        FSlot Slot; Slot.Runtime = MoveTemp(Runtime); Slot.State = State;
                        Slot.Data = MakeUnique<FNativeDataWorker>(*Slot.Runtime, State->Data);
                        Runtimes.Add(State->Id, MoveTemp(Slot));
                    }
                    State->Snapshot.State = bReady ? ENativePeerState::Ready : ENativePeerState::Failed;
                    State->Snapshot.Error = MoveTemp(Error); State->Snapshot.Diagnostics = Diagnostics;
                }
            }
            if (bCancelled) { Runtime.Reset(); PublishClosed(State); }
        }
        bPolling = false;
        for (auto It = Runtimes.CreateIterator(); It; ++It)
        {
            auto& Slot = It.Value();
            if (!Slot.bSignaling) continue;
            bPolling = true;
            if (!Slot.Data->Poll())
            {
                FNativeTransportEvent Event; Event.Type = ENativeSignalEvent::FatalError;
                Event.Result.Error = ENativeSignalError::NativeError;
                Event.Result.Message = TEXT("DataChannel send/receive failed or exceeded its queue bounds.");
                PublishEvent(Slot.State.ToSharedRef(), MoveTemp(Event)); It.RemoveCurrent(); continue;
            }
            for (int32 Count = 0; Count < 8; ++Count)
            {
                FNativeTransportEvent Event;
                if (!Slot.Runtime->PollSignal(Event))
                { Event.Type = ENativeSignalEvent::FatalError; Event.Result.Error = ENativeSignalError::NativeError; Event.Result.Message = TEXT("Could not poll native signaling."); }
                const bool bEmpty = Event.Type == ENativeSignalEvent::None;
                if (!PublishEvent(Slot.State.ToSharedRef(), MoveTemp(Event))) { It.RemoveCurrent(); break; }
                if (bEmpty) break;
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
