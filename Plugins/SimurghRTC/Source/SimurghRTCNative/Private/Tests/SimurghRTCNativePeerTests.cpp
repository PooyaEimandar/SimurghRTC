// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativePeerService.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS && SIMURGHRTC_WITH_WEBRTC
namespace
{
using namespace SimurghRTC;
constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

class FNativeOwnershipCommand final : public IAutomationLatentCommand
{
public:
    explicit FNativeOwnershipCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FNativeOwnershipCommand()
    {
        Release->Trigger();
        Service->Shutdown();
        FPlatformProcess::ReturnSynchEventToPool(Entered);
        FPlatformProcess::ReturnSynchEventToPool(Release);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() - Started > 30)
        {
            Test->AddError(TEXT("Timed out waiting for asynchronous native ownership."));
            return true;
        }
        Service->Tick();
        FString Error;
        if (Phase == 0)
        {
            Data = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, [this](const FNativePeerSnapshot& Result)
            {
                ++Completions;
                Test->TestTrue(TEXT("Data completion uses game thread"), IsInGameThread());
                Test->TestEqual(TEXT("Data peer ready"), Result.State, ENativePeerState::Ready);
                Test->TestTrue(TEXT("Data factory and peer exist"), Result.Diagnostics.bFactoryReady && Result.Diagnostics.bPeerConnectionReady);
                Test->TestTrue(TEXT("Data has no codecs"), Result.Diagnostics.AudioCodecs.IsEmpty() && Result.Diagnostics.VideoCodecs.IsEmpty());
            }, Error);
            Media = Service->Open(Owner.Get(), ENativeFactoryMode::BuiltinMedia, [this](const FNativePeerSnapshot& Result)
            {
                ++Completions;
                Test->TestTrue(TEXT("Media completion uses game thread"), IsInGameThread());
                Test->TestEqual(TEXT("Media peer ready"), Result.State, ENativePeerState::Ready);
                Test->TestTrue(TEXT("Media factory has Opus and video"), Result.Diagnostics.AudioCodecs.Contains(TEXT("opus")) && !Result.Diagnostics.VideoCodecs.IsEmpty());
                Media->Close(); // Reentrant close must not invalidate the callback traversal.
            }, Error);
            if (!Data || !Media) { Test->AddError(Error); return true; }
            Test->TestEqual(TEXT("No inline completions"), Completions, 0);
            Phase = 1;
        }
        else if (Phase == 1 && Completions == 2)
        {
            Test->TestEqual(TEXT("Closing media leaves data ready"), Data->GetSnapshot().State, ENativePeerState::Ready);
            if (!Service->HoldWorkerForTesting(*Entered, *Release) || !Entered->Wait(5000))
            { Test->AddError(TEXT("Could not hold the native worker.")); return true; }

            Data->Close();
            Data->Close();
            Test->TestEqual(TEXT("Close returned while native worker is held"), Data->GetSnapshot().State, ENativePeerState::Closing);
            Test->TestFalse(TEXT("Closing does not advertise a usable peer"), Data->GetSnapshot().Diagnostics.bPeerConnectionReady);

            auto Unexpected = [this](const FNativePeerSnapshot&) { ++SuppressedCompletions; };
            Cancelled = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, Unexpected, Error);
            auto Abandoned = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, Unexpected, Error);
            UObject* TemporaryOwner = NewObject<UGameInstance>();
            TWeakObjectPtr<UObject> WeakOwner = TemporaryOwner;
            Orphan = Service->Open(TemporaryOwner, ENativeFactoryMode::DataOnly, Unexpected, Error);
            if (!Cancelled || !Abandoned || !Orphan) { Test->AddError(Error); return true; }
            Cancelled->Close();
            Abandoned.Reset();
            CollectGarbage(RF_NoFlags);
            Test->TestFalse(TEXT("Native service does not root owners"), WeakOwner.IsValid());
            Service->Tick();
            Test->TestEqual(TEXT("Lost owner closes before worker resumes"), Orphan->GetSnapshot().State, ENativePeerState::Closing);
            Test->TestEqual(TEXT("Cancelled callbacks suppressed"), SuppressedCompletions, 0);
            Release->Trigger();
            Phase = 2;
        }
        else if (Phase == 2 && Service->GetPendingPeerCount() == 0)
        {
            for (const auto& Peer : {Data, Media, Cancelled, Orphan})
                Test->TestEqual(TEXT("Native teardown acknowledged"), Peer->GetSnapshot().State, ENativePeerState::Closed);
            Test->TestEqual(TEXT("No late callbacks after drain"), SuppressedCompletions, 0);
            // Finish native creation without ticking delivery, then invalidate/cancel the recipients.
            TStrongObjectPtr<UObject> LateOwner(NewObject<UGameInstance>());
            auto Unexpected = [this](const FNativePeerSnapshot&) { ++SuppressedCompletions; };
            Undelivered = Service->Open(LateOwner.Get(), ENativeFactoryMode::DataOnly, Unexpected, Error);
            ClosedBeforeDelivery = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, Unexpected, Error);
            if (!Undelivered || !ClosedBeforeDelivery || !Service->HoldWorkerForTesting(*Entered, *Release) || !Entered->Wait(5000))
            { Test->AddError(TEXT("Could not prepare undelivered native completions.")); return true; }
            Test->TestEqual(TEXT("Owner-loss result is waiting for delivery"), Undelivered->GetSnapshot().State, ENativePeerState::Ready);
            Test->TestEqual(TEXT("Close result is waiting for delivery"), ClosedBeforeDelivery->GetSnapshot().State, ENativePeerState::Ready);
            LateOwner.Reset();
            CollectGarbage(RF_NoFlags);
            ClosedBeforeDelivery->Close();
            Service->Tick();
            Test->TestEqual(TEXT("Owner loss suppresses an already ready result"), Undelivered->GetSnapshot().State, ENativePeerState::Closing);
            Test->TestEqual(TEXT("Ready results are not delivered after cancellation"), SuppressedCompletions, 0);
            Release->Trigger();
            Phase = 3;
        }
        else if (Phase == 3 && Service->GetPendingPeerCount() == 0)
        {
            Test->TestEqual(TEXT("Undelivered peer closed"), Undelivered->GetSnapshot().State, ENativePeerState::Closed);
            Test->TestEqual(TEXT("Explicitly cancelled ready peer closed"), ClosedBeforeDelivery->GetSnapshot().State, ENativePeerState::Closed);
            // Reopen after teardown, then create another peer from inside its callback.
            Reopened = Service->Open(Owner.Get(), ENativeFactoryMode::BuiltinMedia, [this](const FNativePeerSnapshot& Result)
            {
                ++Completions;
                Test->TestEqual(TEXT("Fresh media peer ready"), Result.State, ENativePeerState::Ready);
                FString ReentrantError;
                Reentrant = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, [this](const FNativePeerSnapshot& Last)
                {
                    ++Completions;
                    Test->TestEqual(TEXT("Reentrant peer ready"), Last.State, ENativePeerState::Ready);
                    Service->Shutdown(); // Drains two live native runtimes during callback dispatch.
                    Service->Shutdown();
                }, ReentrantError);
                if (!Reentrant) Test->AddError(ReentrantError);
            }, Error);
            if (!Reopened) { Test->AddError(Error); return true; }
            Phase = 4;
        }
        else if (Phase == 4 && Completions == 4)
        {
            Test->TestEqual(TEXT("Shutdown closes reopened media"), Reopened->GetSnapshot().State, ENativePeerState::Closed);
            Test->TestEqual(TEXT("Shutdown closes reentrant peer"), Reentrant->GetSnapshot().State, ENativePeerState::Closed);
            Test->TestEqual(TEXT("Shutdown empties registry"), Service->GetPendingPeerCount(), 0);
            Test->TestFalse(TEXT("Shutdown rejects new peers"), Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error).IsValid());
            Test->TestFalse(TEXT("Rejection explains why"), Error.IsEmpty());
            Service->Tick();
            Test->TestEqual(TEXT("Completions are delivered once"), Completions, 4);
            Test->TestEqual(TEXT("Cancelled callbacks stay suppressed"), SuppressedCompletions, 0);
            return true;
        }
        return false;
    }

private:
    FAutomationTestBase* Test;
    TSharedRef<FNativePeerService, ESPMode::ThreadSafe> Service = MakeShared<FNativePeerService, ESPMode::ThreadSafe>();
    TStrongObjectPtr<UObject> Owner{NewObject<UGameInstance>()};
    FNativePeer::FHandle Data, Media, Cancelled, Orphan, Undelivered, ClosedBeforeDelivery, Reopened, Reentrant;
    FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
    FEvent* Release = FPlatformProcess::GetSynchEventFromPool(false);
    const double Started = FPlatformTime::Seconds();
    int32 Phase = 0;
    int32 Completions = 0;
    int32 SuppressedCompletions = 0;
};

class FNativeModuleCommand final : public IAutomationLatentCommand
{
public:
    explicit FNativeModuleCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FNativeModuleCommand()
    {
        if (WorldPeer) WorldPeer->Close();
        if (OtherPeer) OtherPeer->Close();
        if (World.IsValid()) World->DestroyWorld(false);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() - Started > 30)
        { Test->AddError(TEXT("Timed out waiting for module-owned native peers.")); return true; }
        if (Phase == 0)
        {
            FString Error;
            World.Reset(UWorld::CreateWorld(EWorldType::Game, false));
            auto Completion = [this](const FNativePeerSnapshot& Result)
            {
                ++Completions;
                Test->TestTrue(TEXT("Module dispatches on game thread"), IsInGameThread());
                Test->TestEqual(TEXT("Module peer ready"), Result.State, ENativePeerState::Ready);
            };
            WorldPeer = FNativePeer::Open(World.Get(), ENativeFactoryMode::DataOnly, Completion, Error);
            OtherPeer = FNativePeer::Open(Owner.Get(), ENativeFactoryMode::DataOnly, Completion, Error);
            if (!WorldPeer || !OtherPeer) { Test->AddError(Error); return true; }
            Test->TestEqual(TEXT("Module open is asynchronous"), Completions, 0);
            Phase = 1;
        }
        else if (Phase == 1 && Completions == 2)
        {
            World->DestroyWorld(false);
            World.Reset();
            Test->TestTrue(TEXT("World cleanup requests close"), WorldPeer->GetSnapshot().State != ENativePeerState::Ready);
            Test->TestEqual(TEXT("Unrelated owner survives world cleanup"), OtherPeer->GetSnapshot().State, ENativePeerState::Ready);
            // Losing a live owner closes a ready peer too, without a second callback.
            Owner.Reset();
            CollectGarbage(RF_NoFlags);
            Phase = 2;
        }
        else if (Phase == 2 && WorldPeer->GetSnapshot().State == ENativePeerState::Closed
            && OtherPeer->GetSnapshot().State == ENativePeerState::Closed)
        {
            Test->TestEqual(TEXT("Cleanup does not repeat completion"), Completions, 2);
            return true;
        }
        return false;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UWorld> World;
    TStrongObjectPtr<UObject> Owner{NewObject<UGameInstance>()};
    FNativePeer::FHandle WorldPeer, OtherPeer;
    const double Started = FPlatformTime::Seconds();
    int32 Phase = 0;
    int32 Completions = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCNativeAsyncTest, "SimurghRTC.Native.AsyncOwnership", TestFlags)
bool FSimurghRTCNativeAsyncTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FNativeOwnershipCommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCNativeModuleTest, "SimurghRTC.Native.AsyncModuleLifecycle", TestFlags)
bool FSimurghRTCNativeModuleTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FNativeModuleCommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCNativeCapacityTest, "SimurghRTC.Native.AsyncCapacity", TestFlags)
bool FSimurghRTCNativeCapacityTest::RunTest(const FString&)
{
    auto Service = MakeShared<FNativePeerService, ESPMode::ThreadSafe>();
    TStrongObjectPtr<UObject> Owner(NewObject<UGameInstance>());
    FString Error;
    TestFalse(TEXT("Null owner rejected"), Service->Open(nullptr, ENativeFactoryMode::DataOnly, {}, Error).IsValid());
    TestFalse(TEXT("Invalid mode rejected"), Service->Open(Owner.Get(), static_cast<ENativeFactoryMode>(255), {}, Error).IsValid());
    TestEqual(TEXT("Invalid calls consume no slots"), Service->GetPendingPeerCount(), 0);

    FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
    FEvent* Release = FPlatformProcess::GetSynchEventFromPool(false);
    const bool bHeld = Service->HoldWorkerForTesting(*Entered, *Release) && Entered->Wait(5000);
    TestTrue(TEXT("Native worker held"), bHeld);
    TArray<FNativePeer::FHandle> Peers;
    int32 Callbacks = 0;
    if (bHeld)
    {
        for (int32 Index = 0; Index < FNativePeerService::MaxPeers; ++Index)
        {
            auto Peer = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly,
                [&Callbacks](const FNativePeerSnapshot&) { ++Callbacks; }, Error);
            TestTrue(TEXT("Admitted within capacity"), Peer.IsValid());
            if (Peer) Peers.Add(Peer);
        }
        TestFalse(TEXT("Excess peer rejected"), Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error).IsValid());
        TestFalse(TEXT("Capacity error has explanation"), Error.IsEmpty());
        for (const auto& Peer : Peers) { Peer->Close(); Peer->Close(); }
        TestFalse(TEXT("Pending teardown still counts against capacity"), Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error).IsValid());
    }
    Release->Trigger();
    Service->Shutdown();
    for (const auto& Peer : Peers)
        TestEqual(TEXT("Shutdown drains all admitted peers"), Peer->GetSnapshot().State, ENativePeerState::Closed);
    TestEqual(TEXT("Shutdown suppresses completions"), Callbacks, 0);
    TestEqual(TEXT("Shutdown releases capacity"), Service->GetPendingPeerCount(), 0);
    FPlatformProcess::ReturnSynchEventToPool(Entered);
    FPlatformProcess::ReturnSynchEventToPool(Release);
    return true;
}
#endif
