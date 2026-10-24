// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativePeerService.h"
#include "Engine/GameInstance.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS && SIMURGHRTC_WITH_WEBRTC
namespace
{
using namespace SimurghRTC;
constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;
class FLocalConnections final : public IAutomationLatentCommand
{
public:
    explicit FLocalConnections(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FLocalConnections() { Service->Shutdown(); }
    bool Update() override
    {
        Service->Tick();
        if (bFailed) return true;
        if (FPlatformTime::Seconds() - Started > 90) { Test->AddError(TEXT("Local signaling timed out.")); return true; }
        if (Phase == 0)
        {
            FString Error;
            const auto Mode = Round < 2 ? ENativeFactoryMode::DataOnly : ENativeFactoryMode::BuiltinMedia;
            auto Opened = [this](const FNativePeerSnapshot& S) { if (S.State != ENativePeerState::Ready) Fail(S.Error); else ++Ready; };
            A = Service->Open(Owner.Get(), Mode, Opened, Error); B = Service->Open(Owner.Get(), Mode, Opened, Error);
            if (!A || !B) { Fail(Error); return true; }
            A->SetEventHandler([this](const FNativeTransportEvent& Event) { Forward(Event, B, 0); });
            B->SetEventHandler([this](const FNativeTransportEvent& Event) { Forward(Event, A, 1); });
            Phase = 1;
        }
        else if (Phase == 1 && Ready == 2)
        {
            FNativeSignalRequest Invalid; Invalid.Action = ENativeSignalAction::SetRemote;
            Invalid.SdpType = ENativeSdpType::Offer; Invalid.Sdp = TEXT("v=0\n");
            Send(B, MoveTemp(Invalid), [this](const FNativeSignalResult& R)
            { Test->TestEqual(TEXT("Malformed SDP rejected"), R.Error, ENativeSignalError::InvalidArgument); bInvalidDone = true; }, false);
            Phase = 2;
        }
        else if (Phase == 2 && bInvalidDone)
        {
            FNativeSignalRequest Request;
            Send(A, MoveTemp(Request), [this](const FNativeSignalResult& R) { Offer = R.Sdp; }); Phase = 3;
        }
        else if (Phase == 3 && !Offer.IsEmpty() && EndAcknowledged[0] && Early[0] > 0)
        {
            Test->TestFalse(TEXT("Offer ICE arrived before remote SDP"), B->GetSnapshot().Transport.bRemoteDescriptionSet);
            Test->TestEqual(TEXT("Offer candidates retained"), B->GetSnapshot().Transport.RemoteIceQueued, Early[0]);
            FNativeSignalRequest Request; Request.Action = ENativeSignalAction::SetRemote;
            Request.SdpType = ENativeSdpType::Offer; Request.Sdp = Offer;
            Send(B, MoveTemp(Request), [this](const FNativeSignalResult&) { bRemoteOffer = true; }); Phase = 4;
        }
        else if (Phase == 4 && bRemoteOffer)
        {
            FNativeSignalRequest Request; Request.Action = ENativeSignalAction::CreateAnswer;
            Send(B, MoveTemp(Request), [this](const FNativeSignalResult& R) { Answer = R.Sdp; }); Phase = 5;
        }
        else if (Phase == 5 && !Answer.IsEmpty() && EndAcknowledged[1] && Early[1] > 0)
        {
            Test->TestFalse(TEXT("Answer ICE arrived before remote SDP"), A->GetSnapshot().Transport.bRemoteDescriptionSet);
            Test->TestEqual(TEXT("Answer candidates retained"), A->GetSnapshot().Transport.RemoteIceQueued, Early[1]);
            FNativeSignalRequest Request; Request.Action = ENativeSignalAction::SetRemote;
            Request.SdpType = ENativeSdpType::Answer; Request.Sdp = Answer;
            Send(A, MoveTemp(Request), [this](const FNativeSignalResult&) { bRemoteAnswer = true; }); Phase = 6;
        }
        else if (Phase == 6 && bRemoteAnswer)
        {
            const auto SA = A->GetSnapshot().Transport, SB = B->GetSnapshot().Transport;
            if (SA.Connection != ENativeConnectionState::Connected || SB.Connection != ENativeConnectionState::Connected
                || !SA.bDataTransportOpen || !SB.bDataTransportOpen || !SA.bRemoteIceComplete || !SB.bRemoteIceComplete) return false;
            Test->TestEqual(TEXT("A early queue drained"), SA.RemoteIceQueued, 0u);
            Test->TestEqual(TEXT("B early queue drained"), SB.RemoteIceQueued, 0u);
            Test->TestEqual(TEXT("A applied each unique candidate"), SA.RemoteIceApplied, Early[1]);
            Test->TestEqual(TEXT("B applied each unique candidate"), SB.RemoteIceApplied, Early[0]);
            Test->AddInfo(FString::Printf(TEXT("Local pair %d: ICE connected, DTLS/SCTP open; early candidates %u/%u"), Round, Early[0], Early[1]));
            A->Close(); B->Close(); Phase = 7;
        }
        else if (Phase == 7 && Service->GetPendingPeerCount() == 0)
        {
            Test->TestEqual(TEXT("A closed"), A->GetSnapshot().State, ENativePeerState::Closed);
            Test->TestEqual(TEXT("B closed"), B->GetSnapshot().State, ENativePeerState::Closed);
            if (++Round == 4) return true;
            A.Reset(); B.Reset(); Ready = 0; Phase = 0; Offer.Reset(); Answer.Reset();
            Early[0] = Early[1] = 0; EndAcknowledged[0] = EndAcknowledged[1] = false;
            bRemoteOffer = bRemoteAnswer = bInvalidDone = false;
        }
        return false;
    }
private:
    void Fail(const FString& Message)
    {
        Test->AddError(FString::Printf(TEXT("Round %d phase %d: %s"), Round, Phase, *Message));
        for (const auto& Peer : {A, B}) if (Peer)
        {
            const auto S = Peer->GetSnapshot().Transport;
            Test->AddInfo(FString::Printf(TEXT("connection=%d local/remote SDP=%d/%d ICE queued/applied=%u/%u complete=%d/%d SCTP=%d"),
                int32(S.Connection), S.bLocalDescriptionSet, S.bRemoteDescriptionSet, S.RemoteIceQueued, S.RemoteIceApplied,
                S.bLocalIceComplete, S.bRemoteIceComplete, S.bDataTransportOpen));
        }
        bFailed = true;
    }
    void Send(const FNativePeer::FHandle& Peer, FNativeSignalRequest Request, FNativeSignalCompleted Callback = {}, bool bExpectSuccess = true)
    {
        FString Error;
        if (!Peer->Signal(MoveTemp(Request), [this, Callback = MoveTemp(Callback), bExpectSuccess](const FNativeSignalResult& R) mutable
        {
            Test->TestTrue(TEXT("Signaling callback is on game thread"), IsInGameThread());
            if (bExpectSuccess && !R.IsSuccess()) { Fail(R.Message); return; }
            if (Callback) Callback(R);
        }, Error)) Fail(Error);
    }
    void Forward(const FNativeTransportEvent& Event, const FNativePeer::FHandle& Target, int32 Side)
    {
        Test->TestTrue(TEXT("Transport event is on game thread"), IsInGameThread());
        if (Event.Type == ENativeSignalEvent::FatalError) { Fail(Event.Result.Message); return; }
        if (Event.Type == ENativeSignalEvent::LocalIce)
        {
            FNativeSignalRequest Request; Request.Action = ENativeSignalAction::AddIce; Request.Ice = Event.Ice;
            // Duplicate delivery must not use another queue slot or apply the candidate twice.
            Send(Target, Request, [this, Side](const FNativeSignalResult& R)
            { Test->TestTrue(TEXT("Early ICE acknowledged as queued"), R.bQueued); ++Early[Side]; });
            Send(Target, MoveTemp(Request));
        }
        else if (Event.Type == ENativeSignalEvent::LocalIceComplete)
        {
            FNativeSignalRequest Request; Request.Action = ENativeSignalAction::EndIce;
            Send(Target, MoveTemp(Request), [this, Side](const FNativeSignalResult& R)
            { Test->TestTrue(TEXT("Early completion retained"), R.bQueued); EndAcknowledged[Side] = true; });
        }
    }
    FAutomationTestBase* Test;
    TSharedRef<FNativePeerService, ESPMode::ThreadSafe> Service = MakeShared<FNativePeerService, ESPMode::ThreadSafe>();
    TStrongObjectPtr<UObject> Owner{NewObject<UGameInstance>()};
    FNativePeer::FHandle A, B;
    const double Started = FPlatformTime::Seconds();
    int32 Round = 0, Phase = 0, Ready = 0;
    uint32 Early[2]{};
    bool EndAcknowledged[2]{};
    bool bRemoteOffer = false, bRemoteAnswer = false, bInvalidDone = false, bFailed = false;
    FString Offer, Answer;
};
class FSignalingFailure final : public IAutomationLatentCommand
{
public:
    explicit FSignalingFailure(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FSignalingFailure() { Service->Shutdown(); }
    bool Update() override
    {
        Service->Tick();
        if (FPlatformTime::Seconds() - Started > 30) { Test->AddError(TEXT("Queued ICE failure timed out.")); return true; }
        FString Error;
        if (Phase == 0)
        {
            A = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error);
            B = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error);
            if (!A || !B) { Test->AddError(Error); return true; }
            A->SetEventHandler([this](const FNativeTransportEvent& E)
            {
                if (E.Type != ENativeSignalEvent::LocalIce || bSentIce) return;
                bSentIce = true; FNativeSignalRequest Q; Q.Action = ENativeSignalAction::AddIce; Q.Ice = E.Ice; Q.Ice.Mid = TEXT("wrong");
                FString SubmitError;
                if (!B->Signal(MoveTemp(Q), [this](const FNativeSignalResult& R)
                { Test->TestTrue(TEXT("Unmatched early ICE is queued before SDP"), R.IsSuccess() && R.bQueued); bQueued = true; }, SubmitError)) Test->AddError(SubmitError);
            });
            B->SetEventHandler([this](const FNativeTransportEvent& E)
            {
                if (E.Type == ENativeSignalEvent::FatalError)
                {
                    Test->TestTrue(TEXT("Failure event runs on game thread"), IsInGameThread());
                    Test->TestEqual(TEXT("Wrong queued MID fails explicitly"), E.Result.Error, ENativeSignalError::InvalidArgument);
                    ++Failures;
                }
            });
            Phase = 1;
        }
        else if (Phase == 1 && A->GetSnapshot().State == ENativePeerState::Ready && B->GetSnapshot().State == ENativePeerState::Ready)
        {
            if (!A->Signal({}, [this](const FNativeSignalResult& R) { Offer = R.Sdp; }, Error)) { Test->AddError(Error); return true; }
            Phase = 2;
        }
        else if (Phase == 2 && bQueued && !Offer.IsEmpty())
        {
            FNativeSignalRequest Q; Q.Action = ENativeSignalAction::SetRemote; Q.SdpType = ENativeSdpType::Offer; Q.Sdp = Offer;
            if (!B->Signal(MoveTemp(Q), [this](const FNativeSignalResult&) { ++RemoteCompletions; }, Error)) { Test->AddError(Error); return true; }
            Phase = 3;
        }
        else if (Phase == 3 && Failures)
        {
            Test->TestEqual(TEXT("Exactly one fatal event"), Failures, 1);
            Test->TestEqual(TEXT("Pending SDP completed exactly once"), RemoteCompletions, 1);
            Test->TestEqual(TEXT("Peer marked failed"), B->GetSnapshot().State, ENativePeerState::Failed);
            Test->TestFalse(TEXT("Failed runtime released"), B->GetSnapshot().Diagnostics.bPeerConnectionReady);
            Test->TestFalse(TEXT("Failed peer rejects more signaling"), B->Signal({}, {}, Error));
            Test->TestEqual(TEXT("Other peer survives"), A->GetSnapshot().State, ENativePeerState::Ready);
            A->Close(); B->Close(); Phase = 4;
        }
        else if (Phase == 4 && Service->GetPendingPeerCount() == 0) return true;
        return false;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FNativePeerService, ESPMode::ThreadSafe> Service = MakeShared<FNativePeerService, ESPMode::ThreadSafe>();
    TStrongObjectPtr<UObject> Owner{NewObject<UGameInstance>()};
    FNativePeer::FHandle A, B;
    double Started = FPlatformTime::Seconds();
    int32 Phase = 0, Failures = 0, RemoteCompletions = 0;
    bool bSentIce = false, bQueued = false;
    FString Offer;
};
class FSignalingCancellation final : public IAutomationLatentCommand
{
public:
    explicit FSignalingCancellation(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FSignalingCancellation()
    {
        Release->Trigger(); Service->Shutdown();
        FPlatformProcess::ReturnSynchEventToPool(Entered); FPlatformProcess::ReturnSynchEventToPool(Release);
    }
    bool Update() override
    {
        // In phase 5 deliberately withhold callback delivery until the native offer is ready.
        if (Phase != 5) Service->Tick();
        if (FPlatformTime::Seconds() - Started > 30) { Test->AddError(TEXT("Signaling cancellation timed out.")); return true; }
        FString Error;
        if (Phase == 0)
        {
            Peer = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error);
            if (!Peer) { Test->AddError(Error); return true; } Phase = 1;
        }
        else if (Phase == 1 && Peer->GetSnapshot().State == ENativePeerState::Ready)
        {
            if (!Service->HoldWorkerForTesting(*Entered, *Release) || !Entered->Wait(5000))
            { Test->AddError(TEXT("Worker gate failed.")); return true; }
            FNativeSignalRequest Request; Request.TimeoutMs = 1;
            Test->TestTrue(TEXT("Queued operation accepted"), Peer->Signal(Request, [this](const FNativeSignalResult& R)
            { Test->TestEqual(TEXT("Queue time counts toward deadline"), R.Error, ENativeSignalError::TimedOut); bTimedOut = true; }, Error));
            Test->TestFalse(TEXT("Concurrent SDP rejected"), Peer->Signal(Request, {}, Error));
            FNativeSignalRequest IceEnd; IceEnd.Action = ENativeSignalAction::EndIce;
            for (int32 I = 0; I < 63; ++I)
                Test->TestTrue(TEXT("Bounded pending operation accepted"), Peer->Signal(IceEnd, {}, Error));
            Test->TestFalse(TEXT("Operation beyond 64 rejected"), Peer->Signal(IceEnd, {}, Error));
            GatedAt = FPlatformTime::Seconds(); Phase = 2;
        }
        else if (Phase == 2 && FPlatformTime::Seconds() - GatedAt > 0.02) { Release->Trigger(); Phase = 3; }
        else if (Phase == 3 && bTimedOut)
        {
            Test->TestFalse(TEXT("Expired request did not set local SDP"), Peer->GetSnapshot().Transport.bLocalDescriptionSet);
            Test->TestTrue(TEXT("Offer accepted after timeout"), Peer->Signal({}, [this](const FNativeSignalResult&) { ++Unexpected; }, Error));
            Peer->Close(); Phase = 4;
        }
        else if (Phase == 4 && Service->GetPendingPeerCount() == 0)
        {
            Peer = Service->Open(Owner.Get(), ENativeFactoryMode::DataOnly, {}, Error);
            if (!Peer) { Test->AddError(Error); return true; } Phase = 8;
        }
        else if (Phase == 8 && Peer->GetSnapshot().State == ENativePeerState::Ready)
        {
            Peer->SetEventHandler([this](const FNativeTransportEvent&) { ++Unexpected; });
            if (!Peer->Signal({}, [this](const FNativeSignalResult&) { ++Unexpected; }, Error))
            { Test->AddError(Error); return true; }
            Phase = 5;
        }
        else if (Phase == 5 && Peer->GetSnapshot().Transport.bLocalDescriptionSet)
        {
            // Native completion already exists: owner collection must still suppress delivery.
            Owner.Reset(); CollectGarbage(RF_NoFlags); Service->Tick(); Phase = 6;
        }
        else if (Phase == 6 && Service->GetPendingPeerCount() == 0)
        { Test->TestEqual(TEXT("No operation or event after close/owner loss"), Unexpected, 0); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    TSharedRef<FNativePeerService, ESPMode::ThreadSafe> Service = MakeShared<FNativePeerService, ESPMode::ThreadSafe>();
    TStrongObjectPtr<UObject> Owner{NewObject<UGameInstance>()};
    FNativePeer::FHandle Peer;
    FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
    FEvent* Release = FPlatformProcess::GetSynchEventFromPool(false);
    double Started = FPlatformTime::Seconds(), GatedAt = 0;
    int32 Phase = 0, Unexpected = 0;
    bool bTimedOut = false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeLocalConnectionsTest, "SimurghRTC.Native.LocalConnections", Flags)
bool FNativeLocalConnectionsTest::RunTest(const FString&) { ADD_LATENT_AUTOMATION_COMMAND(FLocalConnections(this)); return true; }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeSignalingCancellationTest, "SimurghRTC.Native.SignalingCancellation", Flags)
bool FNativeSignalingCancellationTest::RunTest(const FString&) { ADD_LATENT_AUTOMATION_COMMAND(FSignalingCancellation(this)); return true; }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeSignalingFailureTest, "SimurghRTC.Native.SignalingFailure", Flags)
bool FNativeSignalingFailureTest::RunTest(const FString&) { ADD_LATENT_AUTOMATION_COMMAND(FSignalingFailure(this)); return true; }
#endif
