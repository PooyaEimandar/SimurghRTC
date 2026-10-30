// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCSession.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCSessionTransport.h"
#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using namespace SimurghRTC;
constexpr auto Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;
void Tick() { FTSTicker::GetCoreTicker().Tick(0); }
class FFakeTransport final : public ISessionTransport
{
public:
    FSessionTransportSnapshot Status;
    FSessionTransportEventHandler Handler;
    TMap<FGuid,FSessionChannelSnapshot> Channels;
    bool bDelayClose = false;
    int32 ClosedChannels = 0;
    FSessionTransportSnapshot Snapshot() const override { return Status; }
    void Close() override
    { Status.State = bDelayClose ? ESimurghRTCSessionState::Closing : ESimurghRTCSessionState::Closed; Status.bNativeClosed = !bDelayClose; Status.bRemoteClosed = !bDelayClose; }
    void SetHandler(FSessionTransportEventHandler InHandler) override { Handler = MoveTemp(InHandler); }
    FSimurghRTCError OpenChannel(const FSimurghRTCChannelOptions& O, FSimurghRTCChannelHandle& H) override
    { H.Id = FGuid::NewGuid(); FSessionChannelSnapshot S; S.Info.Handle = H; S.Info.Options = O; Channels.Add(H.Id,S); return {}; }
    FSimurghRTCError CloseChannel(FSimurghRTCChannelHandle H) override
    { if (auto* C = Channels.Find(H.Id)) { C->State = ESessionChannelState::Closed; C->Info.bOpen = false; ++ClosedChannels; return {}; } return Invalid(); }
    FSimurghRTCError Channel(FSimurghRTCChannelHandle H, FSessionChannelSnapshot& Out) const override
    { if (const auto* C = Channels.Find(H.Id)) { Out = *C; return {}; } return Invalid(); }
    FSimurghRTCError SendText(FSimurghRTCChannelHandle H, const FString&) override { return CanSend(H); }
    FSimurghRTCError SendBytes(FSimurghRTCChannelHandle H, const TArray<uint8>&) override { return CanSend(H); }
    static FSimurghRTCError Invalid() { return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState,TEXT("Unknown fake channel.")); }
    FSimurghRTCError CanSend(FSimurghRTCChannelHandle H) const
    { const auto* C = Channels.Find(H.Id); return C && C->State == ESessionChannelState::Open ? FSimurghRTCError{} : Invalid(); }
};
struct FFixture
{
    const ESimurghRTCProvider Provider = SIMURGHRTC_WITH_DIRECT ? ESimurghRTCProvider::DirectUnreal : ESimurghRTCProvider::Janus;
    FSessionTransportFactory Previous;
    TArray<TSharedPtr<FFakeTransport>> Created;
    TStrongObjectPtr<UGameInstance> Owner{NewObject<UGameInstance>()};
    TStrongObjectPtr<USimurghRTCSession> Session{USimurghRTCSession::CreateSession(Owner.Get())};
    FFixture()
    {
        HasSessionTransport(Provider);
        Previous = SetSessionTransportFactory(Provider,[this](UObject*, const FSimurghRTCSessionConfig&, FSimurghRTCError&)
        { auto T = MakeShared<FFakeTransport>(); Created.Add(T); return T; });
    }
    ~FFixture()
    {
        if (Session) Session->Shutdown();
        for (const auto& T : Created) { T->bDelayClose = false; T->Close(); }
        Tick(); SetSessionTransportFactory(Provider,MoveTemp(Previous));
    }
    FSimurghRTCSessionConfig Config() const
    { FSimurghRTCSessionConfig C; C.Provider = Provider; C.Endpoint = TEXT("https://rtc.invalid/session"); return C; }
    TStrongObjectPtr<USimurghRTCOperation> Connect()
    { TStrongObjectPtr<USimurghRTCOperation> O(USimurghRTCOperation::Connect(Session.Get(),Config())); O->Activate(); Tick(); return O; }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionConnectWaitTest,"SimurghRTC.Session.ConnectionOwnership",Flags)
bool FSessionConnectWaitTest::RunTest(const FString&)
{
    FFixture F; auto Op = F.Connect(); int32 Count = 0;
    Op->OnCompletedNative.AddLambda([&](const auto&) { ++Count; });
    TestFalse(TEXT("Connect waits for negotiation"),Op->IsFinished());
    TStrongObjectPtr<USimurghRTCOperation> Dormant(USimurghRTCOperation::Connect(F.Session.Get(),F.Config())); Dormant->Cancel();
    TestEqual(TEXT("Cancelling a dormant request does not cancel the active one"),F.Session->GetState(),ESimurghRTCSessionState::Connecting);
    F.Created.Last()->Status.State = ESimurghRTCSessionState::Connected; Tick();
    TestEqual(TEXT("Connect completes once"),Count,1); TestTrue(TEXT("Connect success"),Op->GetResult().IsSuccess());
    TestTrue(TEXT("Negotiated data capability"),F.Session->GetCapability(F.Provider,ESimurghRTCFeature::DataChannels).IsAvailable());
    const auto Id = F.Session->GetSessionId(); F.Created.Last()->bDelayClose = true;
    TStrongObjectPtr<USimurghRTCOperation> Close(USimurghRTCOperation::Disconnect(F.Session.Get())); Close->Activate(); Tick();
    TestFalse(TEXT("Disconnect waits for native closure"),Close->IsFinished());
    F.Created.Last()->bDelayClose = false; F.Created.Last()->Close(); Tick();
    TestTrue(TEXT("Disconnect finishes after closure"),Close->GetResult().IsSuccess());
    auto Reconnect = F.Connect(); TestEqual(TEXT("Facade identity survives reconnect"),F.Session->GetSessionId(),Id);
    TestEqual(TEXT("Reconnect creates another transport"),F.Created.Num(),2);
    Reconnect->Cancel(); Tick();
    TestEqual(TEXT("Cancel returns to idle"),F.Session->GetState(),ESimurghRTCSessionState::Idle);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionCancelCollectTest,"SimurghRTC.Session.CancellationRetainsDispatchOwner",Flags)
bool FSessionCancelCollectTest::RunTest(const FString&)
{
    FFixture F; auto Op = F.Connect(); TWeakObjectPtr<USimurghRTCSession> Weak(F.Session.Get());
    int32 Completions = 0;
    Op->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult& R)
    {
        ++Completions;
        TestEqual(TEXT("Cancellation result"),R.Error.Code,ESimurghRTCErrorCode::Cancelled);
        F.Session.Reset(); CollectGarbage(RF_NoFlags);
        TestTrue(TEXT("Dispatch pins its session while callbacks execute"),Weak.IsValid());
    });
    Op->Cancel();
    TestEqual(TEXT("One direct cancellation completion"),Completions,1);
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("No session retained after dispatch returns"),Weak.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionChannelWaitTest,"SimurghRTC.Session.ChannelCompletionAndCancel",Flags)
bool FSessionChannelWaitTest::RunTest(const FString&)
{
    FFixture F; auto Connection = F.Connect(); auto T = F.Created.Last(); T->Status.State = ESimurghRTCSessionState::Connected; Tick();
    TStrongObjectPtr<USimurghRTCOperation> Open(USimurghRTCOperation::OpenChannel(F.Session.Get(),{})); Open->Activate(); Tick();
    TestFalse(TEXT("Open waits for native open"),Open->IsFinished()); TestFalse(TEXT("Pending result has no handle"),Open->GetResult().Channel.IsValid());
    Open->Cancel(); TestEqual(TEXT("Cancel closes partially opened channel"),T->ClosedChannels,1);
    TestFalse(TEXT("Cancelled result has no handle"),Open->GetResult().Channel.IsValid());
    TStrongObjectPtr<USimurghRTCOperation> Next(USimurghRTCOperation::OpenChannel(F.Session.Get(),{})); Next->Activate(); Tick();
    for (auto& Pair : T->Channels) if (Pair.Value.State == ESessionChannelState::Connecting)
    { Pair.Value.State = ESessionChannelState::Open; Pair.Value.Info.bOpen = true; }
    Tick(); TestFalse(TEXT("Open waits for the negotiated message limit"),Next->IsFinished());
    for (auto& Pair : T->Channels) Pair.Value.Info.MaximumMessageBytes = 65536;
    Tick(); TestTrue(TEXT("Open publishes usable handle"),Next->GetResult().Channel.IsValid());
    const auto H = Next->GetResult().Channel; TestFalse(TEXT("Usable channel sends"),F.Session->SendBytes(H,{}).IsError());
    FSimurghRTCChannelHandle Foreign; Foreign.Id = FGuid::NewGuid();
    TestEqual(TEXT("Unknown handle rejected"),F.Session->SendText(Foreign,TEXT("x")).Code,ESimurghRTCErrorCode::InvalidState);
    TStrongObjectPtr<USimurghRTCOperation> Close(USimurghRTCOperation::CloseChannel(F.Session.Get(),H)); Close->Activate(); Tick();
    TestTrue(TEXT("Close completes with native Closed"),Close->GetResult().IsSuccess());
    TestEqual(TEXT("Closed handle cannot send"),F.Session->SendBytes(H,{}).Code,ESimurghRTCErrorCode::InvalidState);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionDisconnectPendingTest,"SimurghRTC.Session.DisconnectPendingConnection",Flags)
bool FSessionDisconnectPendingTest::RunTest(const FString&)
{
    FFixture F; auto Connection = F.Connect(); auto T = F.Created.Last(); T->bDelayClose = true;
    TStrongObjectPtr<USimurghRTCOperation> Close(USimurghRTCOperation::Disconnect(F.Session.Get())); Close->Activate(); Tick();
    TestEqual(TEXT("Pending connect cancelled"),Connection->GetResult().Error.Code,ESimurghRTCErrorCode::Cancelled);
    TestFalse(TEXT("Cancellation does not bypass Disconnect cleanup wait"),Close->IsFinished());
    Close->Cancel(); TestEqual(TEXT("Cancel cannot undo a committed close"),F.Session->GetState(),ESimurghRTCSessionState::Closing);
    T->bDelayClose = false; T->Close(); Tick();
    TestEqual(TEXT("Close completes silently after cancellation"),F.Session->GetState(),ESimurghRTCSessionState::Closed);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionDeadlineTest,"SimurghRTC.Session.ActivationDeadline",Flags)
bool FSessionDeadlineTest::RunTest(const FString&)
{
    FFixture F; auto Config = F.Config(); Config.TimeoutMilliseconds = 1;
    TStrongObjectPtr<USimurghRTCOperation> Op(USimurghRTCOperation::Connect(F.Session.Get(),Config));
    Op->Activate(); FPlatformProcess::Sleep(0.003f); Tick();
    TestEqual(TEXT("Deadline starts at activation"),Op->GetResult().Error.Code,ESimurghRTCErrorCode::Timeout);
    TestEqual(TEXT("Expired work does not allocate a transport"),F.Created.Num(),0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionOwnerLossTest,"SimurghRTC.Session.OwnerLossAndWorldCleanup",Flags)
bool FSessionOwnerLossTest::RunTest(const FString&)
{
    FFixture F; auto Connection = F.Connect(); int32 Calls = 0;
    Connection->OnCompletedNative.AddLambda([&](const auto&) { ++Calls; });
    F.Owner->MarkAsGarbage(); Tick();
    TestTrue(TEXT("Owner loss settles operation"),Connection->IsFinished()); TestEqual(TEXT("Owner loss is silent"),Calls,0);
    TestFalse(TEXT("Owner loss releases callback"),bool(F.Created.Last()->Handler));
    TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::PIE,false));
    TStrongObjectPtr<USimurghRTCSession> Session(USimurghRTCSession::CreateSession(World.Get()));
    TStrongObjectPtr<USimurghRTCOperation> Op(USimurghRTCOperation::Connect(Session.Get(),F.Config()));
    Op->OnCompletedNative.AddLambda([&](const auto&) { ++Calls; }); Op->Activate(); Tick();
    FWorldDelegates::OnWorldCleanup.Broadcast(World.Get(),true,true); Tick();
    TestFalse(TEXT("World cleanup shuts facade"),Session->IsUsable()); TestEqual(TEXT("World cleanup is silent"),Calls,0);
    TestTrue(TEXT("World cleanup settles operation"),Op->IsFinished());
    World->DestroyWorld(false); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionConnectionErrorTest,"SimurghRTC.Session.ConnectionErrorRecovery",Flags)
bool FSessionConnectionErrorTest::RunTest(const FString&)
{
    FFixture F; const auto Id = F.Session->GetSessionId();
    TestFalse(TEXT("New session has no connection failure"),F.Session->GetConnectionError().IsError());
    auto Connect = F.Connect(); auto Old = F.Created.Last();
    Old->Status.State = ESimurghRTCSessionState::Connected; Tick();
    TestTrue(TEXT("First connection completed"),Connect->GetResult().IsSuccess());
    Old->Status.State = ESimurghRTCSessionState::Failed;
    Old->Status.Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InternalError,TEXT("Transport unavailable.")); Tick();
    TestEqual(TEXT("Transport loss reaches facade"),F.Session->GetState(),ESimurghRTCSessionState::Failed);
    TestEqual(TEXT("Connection failure retained after retirement"),F.Session->GetConnectionError().Code,ESimurghRTCErrorCode::InternalError);
    TestEqual(TEXT("Failure identifies facade"),F.Session->GetConnectionError().SessionId,Id);
    TestFalse(TEXT("Unsolicited loss has no operation ID"),F.Session->GetConnectionError().OperationId.IsValid());
    auto Reconnect = F.Connect();
    TestEqual(TEXT("Reconnect preserves facade"),F.Session->GetSessionId(),Id);
    TestEqual(TEXT("Reconnect creates fresh transport"),F.Created.Num(),2);
    TestFalse(TEXT("New connection clears the old error"),F.Session->GetConnectionError().IsError());
    F.Created.Last()->Status.State = ESimurghRTCSessionState::Connected; Tick();
    TestTrue(TEXT("Reconnect completes"),Reconnect->GetResult().IsSuccess());
    // Mutating the retired transport cannot alter the new connection.
    Old->Status.Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::Timeout,TEXT("Late old error.")); Tick();
    TestEqual(TEXT("Old failure cannot replace new state"),F.Session->GetState(),ESimurghRTCSessionState::Connected);
    TestFalse(TEXT("Old failure cannot replace new error"),F.Session->GetConnectionError().IsError());
    return true;
}

#endif
