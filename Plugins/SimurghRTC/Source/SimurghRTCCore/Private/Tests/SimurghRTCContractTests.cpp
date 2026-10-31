#include "SimurghRTCOperation.h"
#include "SimurghRTCSession.h"
#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

struct FSessionFixture
{
    TStrongObjectPtr<UGameInstance> Owner{NewObject<UGameInstance>()};
    TStrongObjectPtr<USimurghRTCSession> Session{USimurghRTCSession::CreateSession(Owner.Get())};
    ~FSessionFixture() { Session->Shutdown(); }

    FSimurghRTCSessionConfig Config() const
    {
        FSimurghRTCSessionConfig Value;
        Value.Provider = SIMURGHRTC_WITH_DIRECT ? ESimurghRTCProvider::DirectUnreal : ESimurghRTCProvider::Janus;
        Value.Endpoint = TEXT("https://rtc.invalid/session");
        return Value;
    }
};

void TickOperations() { FTSTicker::GetCoreTicker().Tick(0.0f); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCValidationTest, "SimurghRTC.Contract.Validation", TestFlags)
bool FSimurghRTCValidationTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    auto Config = Fixture.Config();
    TestFalse(TEXT("HTTPS configuration accepted"), SimurghRTC::ValidateSessionConfig(Config).IsError());
    Config.Endpoint = TEXT("http://localhost:8080/session");
    TestEqual(TEXT("Insecure signaling needs opt-in"), SimurghRTC::ValidateSessionConfig(Config).Code, ESimurghRTCErrorCode::InvalidArgument);
    Config.bAllowInsecureDevelopmentSignaling = true;
    TestFalse(TEXT("Explicit development HTTP allowed"), SimurghRTC::ValidateSessionConfig(Config).IsError());
    for (const TCHAR* Endpoint : {TEXT("https:///missing-host"), TEXT("https://user:secret@rtc.invalid"), TEXT("https://rtc.invalid/a b"), TEXT("https://rtc.invalid/#fragment")})
    {
        Config.Endpoint = Endpoint;
        TestEqual(TEXT("Malformed or credential-bearing URL rejected"), SimurghRTC::ValidateSessionConfig(Config).Code, ESimurghRTCErrorCode::InvalidArgument);
    }
    Config.Endpoint = TEXT("https://rtc.invalid/session");
    Config.TimeoutMilliseconds = 0;
    TestEqual(TEXT("Timeout validated in C++ too"), SimurghRTC::ValidateSessionConfig(Config).Code, ESimurghRTCErrorCode::InvalidArgument);
    Config = Fixture.Config();
    Config.Provider = static_cast<ESimurghRTCProvider>(StaticEnum<ESimurghRTCProvider>()->GetMaxEnumValue());
    TestEqual(TEXT("Generated provider MAX is not a provider"), SimurghRTC::ValidateSessionConfig(Config).Code, ESimurghRTCErrorCode::InvalidArgument);
    Config = Fixture.Config();
    Config.Purpose = static_cast<ESimurghRTCPurpose>(StaticEnum<ESimurghRTCPurpose>()->GetMaxEnumValue());
    TestEqual(TEXT("Generated purpose MAX is not a purpose"), SimurghRTC::ValidateSessionConfig(Config).Code, ESimurghRTCErrorCode::InvalidArgument);

    FSimurghRTCChannelOptions Channel;
    Channel.Label = TEXT("chat");
    TestFalse(TEXT("Reliable channel valid"), SimurghRTC::ValidateChannelOptions(Channel).IsError());
    Channel.ReliabilityValue = 1;
    TestEqual(TEXT("Reliable channel rejects partial-reliability bound"), SimurghRTC::ValidateChannelOptions(Channel).Code, ESimurghRTCErrorCode::InvalidArgument);
    Channel.Reliability = ESimurghRTCReliability::MaxRetransmits;
    Channel.ReliabilityValue = 0;
    TestFalse(TEXT("Zero retransmits is valid"), SimurghRTC::ValidateChannelOptions(Channel).IsError());
    Channel.Label = FString::ChrN(32768, static_cast<TCHAR>(0x00E9));
    TestEqual(TEXT("Label bound is UTF-8 bytes, not TCHAR count"), SimurghRTC::ValidateChannelOptions(Channel).Code, ESimurghRTCErrorCode::InvalidArgument);
    const TCHAR EmbeddedNull[] = { 'a', 0, 'b' };
    Channel.Label = FString(3, EmbeddedNull);
    TestEqual(TEXT("Embedded null rejected"), SimurghRTC::ValidateChannelOptions(Channel).Code, ESimurghRTCErrorCode::InvalidArgument);
    Channel.Label = TEXT("chat");
    Channel.Reliability = static_cast<ESimurghRTCReliability>(StaticEnum<ESimurghRTCReliability>()->GetMaxEnumValue());
    TestEqual(TEXT("Generated reliability MAX is not a mode"), SimurghRTC::ValidateChannelOptions(Channel).Code, ESimurghRTCErrorCode::InvalidArgument);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCCompletionTest, "SimurghRTC.Contract.Completion", TestFlags)
bool FSimurghRTCCompletionTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    TStrongObjectPtr<USimurghRTCOperation> Operation(USimurghRTCOperation::Connect(Fixture.Session.Get(), Fixture.Config()));
    int32 Completions = 0;
    Operation->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult&) { ++Completions; });
    TestFalse(TEXT("Factory is dormant"), Operation->IsFinished());
    TestFalse(TEXT("Pending result does not report success"), Operation->GetResult().IsSuccess());
    Operation->Activate();
    TestEqual(TEXT("Activation does not complete inline"), Completions, 0);
    TickOperations();
    TestEqual(TEXT("Exactly one completion"), Completions, 1);
    TestEqual(TEXT("Invalid Direct endpoint or unimplemented Janus fails explicitly"), Operation->GetResult().Error.Code,
        SIMURGHRTC_WITH_DIRECT ? ESimurghRTCErrorCode::InvalidArgument : ESimurghRTCErrorCode::NotImplemented);
    TestEqual(TEXT("Validation leaves state unchanged"), Fixture.Session->GetState(),
        SIMURGHRTC_WITH_DIRECT ? ESimurghRTCSessionState::Idle : ESimurghRTCSessionState::Failed);
    TestEqual(TEXT("Session correlation"), Operation->GetResult().Error.SessionId, Fixture.Session->GetSessionId());
    TestEqual(TEXT("Operation correlation"), Operation->GetResult().Error.OperationId, Operation->GetOperationId());
    Operation->Activate();
    Operation->Cancel();
    TickOperations();
    TestEqual(TEXT("Finished operation cannot complete again"), Completions, 1);

    TStrongObjectPtr<USimurghRTCOperation> Disconnect(USimurghRTCOperation::Disconnect(Fixture.Session.Get()));
    Disconnect->Activate();
    TickOperations();
    TestTrue(TEXT("Local disconnect succeeds"), Disconnect->GetResult().IsSuccess());
    TestEqual(TEXT("Disconnected state"), Fixture.Session->GetState(), ESimurghRTCSessionState::Closed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCCancellationTest, "SimurghRTC.Contract.Cancellation", TestFlags)
bool FSimurghRTCCancellationTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    TStrongObjectPtr<USimurghRTCOperation> Operation(USimurghRTCOperation::Connect(Fixture.Session.Get(), Fixture.Config()));
    int32 Completions = 0;
    Operation->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult&) { ++Completions; });
    Operation->Activate();
    Operation->Cancel();
    Operation->Cancel();
    TickOperations();
    TestEqual(TEXT("Cancel fires once"), Completions, 1);
    TestEqual(TEXT("Cancellation is distinct from failure"), Operation->GetResult().Error.Code, ESimurghRTCErrorCode::Cancelled);
    TestEqual(TEXT("Cancelled attempt never connects"), Fixture.Session->GetState(), ESimurghRTCSessionState::Idle);

    TStrongObjectPtr<USimurghRTCOperation> Abandoned(USimurghRTCOperation::Connect(Fixture.Session.Get(), Fixture.Config()));
    Abandoned->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult&) { ++Completions; });
    Abandoned->Activate();
    Fixture.Session->Shutdown();
    Fixture.Session->Shutdown();
    TickOperations();
    TestTrue(TEXT("Teardown finishes outstanding operations"), Abandoned->IsFinished());
    TestEqual(TEXT("Teardown suppresses callbacks"), Completions, 1);
    TestEqual(TEXT("Teardown records cancelled result"), Abandoned->GetResult().Error.Code, ESimurghRTCErrorCode::Cancelled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCReentrancyTest, "SimurghRTC.Contract.ReentrantTeardown", TestFlags)
bool FSimurghRTCReentrancyTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    TStrongObjectPtr<USimurghRTCOperation> Operation(USimurghRTCOperation::Connect(Fixture.Session.Get(), Fixture.Config()));
    int32 Completions = 0;
    Operation->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult&)
    {
        ++Completions;
        Fixture.Session->Shutdown();
        Operation->Cancel();
        CollectGarbage(RF_NoFlags);
    });
    Operation->Activate();
    TickOperations();
    TestEqual(TEXT("Reentrant shutdown and GC do not duplicate completion"), Completions, 1);
    TestEqual(TEXT("Owner remains shut down"), Fixture.Session->GetState(), ESimurghRTCSessionState::Closed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCProfileTest, "SimurghRTC.Contract.ProfileAndReflection", TestFlags)
bool FSimurghRTCProfileTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    auto Janus = Fixture.Session->GetCapability(ESimurghRTCProvider::Janus, ESimurghRTCFeature::DataChannels);
    TestFalse(TEXT("Compiled skeleton is not a usable capability"), Janus.IsAvailable());
    TestEqual(TEXT("Generated feature MAX is not a capability"), Fixture.Session->GetCapability(Fixture.Session->GetProvider(),
        static_cast<ESimurghRTCFeature>(StaticEnum<ESimurghRTCFeature>()->GetMaxEnumValue())).Reason.Code, ESimurghRTCErrorCode::InvalidArgument);
    TestEqual(TEXT("Generated provider MAX cannot query capabilities"), Fixture.Session->GetCapability(
        static_cast<ESimurghRTCProvider>(StaticEnum<ESimurghRTCProvider>()->GetMaxEnumValue()), ESimurghRTCFeature::Audio).Reason.Code, ESimurghRTCErrorCode::InvalidArgument);
    TestEqual(TEXT("Janus compiled availability matches selected profile"), Janus.Compiled,
        SIMURGHRTC_WITH_JANUS ? ESimurghRTCSupport::Available : ESimurghRTCSupport::Unavailable);
    TestEqual(TEXT("Build exclusion differs from implementation pending"), Janus.Reason.Code,
        SIMURGHRTC_WITH_JANUS ? ESimurghRTCErrorCode::NotImplemented : ESimurghRTCErrorCode::BuildExcluded);
    const UClass* JanusClass = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCJanusExtensions.SimurghRTCJanusOperation"));
    const UClass* MediaClass = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCMedia.SimurghRTCMediaOperation"));
    TestEqual(TEXT("Optional Janus reflection matches profile"), JanusClass != nullptr, bool(SIMURGHRTC_WITH_JANUS_EXTENSIONS));
    TestEqual(TEXT("Optional media reflection matches profile"), MediaClass != nullptr, bool(SIMURGHRTC_WITH_MEDIA));
    const auto Rooms = Fixture.Session->GetCapability(ESimurghRTCProvider::Janus, ESimurghRTCFeature::JanusRooms);
    TestEqual(TEXT("Room capability follows extension module selection"), Rooms.Reason.Code,
        SIMURGHRTC_WITH_JANUS_EXTENSIONS ? ESimurghRTCErrorCode::NotImplemented : ESimurghRTCErrorCode::BuildExcluded);
    const auto Audio = Fixture.Session->GetCapability(Fixture.Session->GetProvider(), ESimurghRTCFeature::Audio);
    TestEqual(TEXT("Audio capability follows target media selection"), Audio.Reason.Code,
        (SIMURGHRTC_WITH_MEDIA || UE_SERVER) ? (SIMURGHRTC_WITH_DIRECT ? ESimurghRTCErrorCode::NotNegotiated : ESimurghRTCErrorCode::NotImplemented) : ESimurghRTCErrorCode::BuildExcluded);
    auto DirectConfig = Fixture.Config(); DirectConfig.Provider = ESimurghRTCProvider::DirectUnreal;
    TStrongObjectPtr<USimurghRTCOperation> DirectConnect(USimurghRTCOperation::Connect(Fixture.Session.Get(), DirectConfig));
    DirectConnect->Activate(); TickOperations();
    TestEqual(TEXT("Direct exclusion is enforced by the async API"), DirectConnect->GetResult().Error.Code,
        SIMURGHRTC_WITH_DIRECT ? ESimurghRTCErrorCode::InvalidArgument : ESimurghRTCErrorCode::BuildExcluded);
    for (const TCHAR* Name : {TEXT("Connect"), TEXT("Disconnect"), TEXT("OpenChannel"), TEXT("CloseChannel"), TEXT("Call"), TEXT("AcceptCall"), TEXT("RejectCall"), TEXT("HangUp"), TEXT("JoinTextRoom"), TEXT("LeaveTextRoom"), TEXT("SendRoomText"), TEXT("CreateGeneratedSource"), TEXT("AttachSource"), TEXT("ReplaceSource"), TEXT("SetSourceMuted"), TEXT("StopSource"), TEXT("DetachTrack"), TEXT("SetDecodedFrameDelivery"), TEXT("Cancel")})
    {
        const UFunction* Function = USimurghRTCOperation::StaticClass()->FindFunctionByName(Name);
        TestTrue(FString::Printf(TEXT("%s is Blueprint callable"), Name), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
    }
    TestEqual(TEXT("Initial provider exists in the profile"), Fixture.Session->GetProvider(),
        SIMURGHRTC_WITH_DIRECT ? ESimurghRTCProvider::DirectUnreal : ESimurghRTCProvider::Janus);
    auto JanusConfig = Fixture.Config(); JanusConfig.Provider = ESimurghRTCProvider::Janus;
    TStrongObjectPtr<USimurghRTCOperation> JanusConnect(USimurghRTCOperation::Connect(Fixture.Session.Get(), JanusConfig));
    JanusConnect->Activate(); TickOperations();
    TestEqual(TEXT("Async provider selection respects build exclusion"), JanusConnect->GetResult().Error.Code,
        SIMURGHRTC_WITH_JANUS ? ESimurghRTCErrorCode::NotImplemented : ESimurghRTCErrorCode::BuildExcluded);
    TestFalse(TEXT("No fabricated statistics"), Fixture.Session->GetStatistics().bAvailable);
    TestEqual(TEXT("Invalid channel is rejected"), Fixture.Session->SendText({}, TEXT("hello")).Code, ESimurghRTCErrorCode::InvalidArgument);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCPendingGCTest, "SimurghRTC.Contract.PendingFailureSurvivesGC", TestFlags)
bool FSimurghRTCPendingGCTest::RunTest(const FString&)
{
    TWeakObjectPtr<USimurghRTCOperation> Operation(USimurghRTCOperation::Disconnect(nullptr));
    int32 Count = 0;
    FSimurghRTCOperationResult Result;
    Operation->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult& Value) { ++Count; Result = Value; });
    Operation->Activate();
    CollectGarbage(RF_NoFlags);
    TestTrue(TEXT("Deferred null-session failure remains alive"), Operation.IsValid());
    TickOperations();
    TestEqual(TEXT("Null-session failure completes once after GC"), Count, 1);
    TestEqual(TEXT("Null-session input is invalid"), Result.Error.Code, ESimurghRTCErrorCode::InvalidArgument);
    TickOperations();
    TestEqual(TEXT("No duplicate failure"), Count, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCDisconnectTest, "SimurghRTC.Contract.DisconnectAndReuse", TestFlags)
bool FSimurghRTCDisconnectTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    TStrongObjectPtr<USimurghRTCOperation> Disconnect(USimurghRTCOperation::Disconnect(Fixture.Session.Get()));
    TStrongObjectPtr<USimurghRTCOperation> Pending(USimurghRTCOperation::Connect(Fixture.Session.Get(), Fixture.Config()));
    int32 Cancelled = 0;
    Pending->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult& Value)
    {
        ++Cancelled;
        TestEqual(TEXT("Disconnect cancels other active operations"), Value.Error.Code, ESimurghRTCErrorCode::Cancelled);
    });
    Disconnect->Activate(); Pending->Activate(); TickOperations();
    TestEqual(TEXT("One cancellation"), Cancelled, 1);
    TestTrue(TEXT("Disconnect succeeds"), Disconnect->GetResult().IsSuccess());
    TStrongObjectPtr<USimurghRTCOperation> Retry(USimurghRTCOperation::Connect(Fixture.Session.Get(), Fixture.Config()));
    Retry->Activate(); TickOperations();
    TestEqual(TEXT("Reuse reaches the provider boundary"), Retry->GetResult().Error.Code,
        SIMURGHRTC_WITH_DIRECT ? ESimurghRTCErrorCode::InvalidArgument : ESimurghRTCErrorCode::NotImplemented);
    TStrongObjectPtr<USimurghRTCOperation> Dormant(USimurghRTCOperation::Disconnect(Fixture.Session.Get()));
    Dormant->OnCompletedNative.AddLambda([&](const FSimurghRTCOperationResult&) { AddError(TEXT("Callback after final shutdown")); });
    Fixture.Session->Shutdown(); Dormant->Activate(); TickOperations();
    TestTrue(TEXT("Activation after shutdown settles silently"), Dormant->IsFinished());
    TestEqual(TEXT("No fake success after shutdown"), Dormant->GetResult().Error.Code, ESimurghRTCErrorCode::Cancelled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCFrameContractTest, "SimurghRTC.Contract.FrameBoundaries", TestFlags)
bool FSimurghRTCFrameContractTest::RunTest(const FString&)
{
    FSessionFixture Fixture;
    TestEqual(TEXT("Frozen contract version"), USimurghRTCSession::GetApiContractVersion(), 1);
    FSimurghRTCAudioFrame Audio;
    Audio.Samples.Init(0.25f, 480);
    TestFalse(TEXT("10 ms PCM accepted structurally"), SimurghRTC::ValidateAudioFrame(Audio).IsError());
    Audio.Channels = 0;
    TestTrue(TEXT("Zero channels rejected without dividing by zero"), SimurghRTC::ValidateAudioFrame(Audio).IsError());
    Audio.Channels = 2; Audio.Samples.Add(0.0f);
    TestTrue(TEXT("Partial interleaved frame rejected"), SimurghRTC::ValidateAudioFrame(Audio).IsError());
    Audio.Channels = 1; Audio.Samples.Init(0.0f, 4801);
    TestTrue(TEXT("More than 100 ms rejected"), SimurghRTC::ValidateAudioFrame(Audio).IsError());
    Audio.Samples.Init(2.0f, 480);
    TestTrue(TEXT("Out-of-range PCM rejected"), SimurghRTC::ValidateAudioFrame(Audio).IsError());
    Audio.Samples.Init(0.0f, 480);
    FSimurghRTCSourceHandle Forged; Forged.Id = FGuid::NewGuid();
    TestEqual(TEXT("Valid frame cannot fabricate a source"), Fixture.Session->PushAudioFrame(Forged, Audio).Code,
        (SIMURGHRTC_WITH_MEDIA || UE_SERVER) ? ESimurghRTCErrorCode::InvalidState : ESimurghRTCErrorCode::BuildExcluded);
    FSimurghRTCVideoFrame Video; Video.Width = 2; Video.Height = 2; Video.Bgra.Init(0, 16);
    TestFalse(TEXT("BGRA accepted structurally"), SimurghRTC::ValidateVideoFrame(Video).IsError());
    Video.Bgra.RemoveAt(0);
    TestTrue(TEXT("Short pixel buffer rejected"), SimurghRTC::ValidateVideoFrame(Video).IsError());
    Video.Width = MAX_int32; Video.Height = MAX_int32;
    TestTrue(TEXT("Oversized dimensions rejected without overflow"), SimurghRTC::ValidateVideoFrame(Video).IsError());
    TestEqual(TEXT("No fabricated call"), Fixture.Session->GetCallInfo().State, ESimurghRTCCallState::None);
    TStrongObjectPtr<USimurghRTCOperation> Source(USimurghRTCOperation::CreateGeneratedSource(Fixture.Session.Get(), ESimurghRTCTrackKind::Audio));
    Source->Activate(); TickOperations();
    TestEqual(TEXT("Generated-source shell has explicit availability"), Source->GetResult().Error.Code,
        (SIMURGHRTC_WITH_MEDIA || UE_SERVER) ? (SIMURGHRTC_WITH_DIRECT ? ESimurghRTCErrorCode::None : ESimurghRTCErrorCode::NotImplemented) : ESimurghRTCErrorCode::BuildExcluded);
    TStrongObjectPtr<USimurghRTCOperation> InvalidSource(USimurghRTCOperation::CreateGeneratedSource(Fixture.Session.Get(),
        static_cast<ESimurghRTCTrackKind>(StaticEnum<ESimurghRTCTrackKind>()->GetMaxEnumValue())));
    InvalidSource->Activate(); TickOperations();
    TestEqual(TEXT("Generated track MAX is not a media kind"), InvalidSource->GetResult().Error.Code, ESimurghRTCErrorCode::InvalidArgument);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCIceValidationTest, "SimurghRTC.Contract.IceConfiguration", TestFlags)
bool FSimurghRTCIceValidationTest::RunTest(const FString&)
{
    using SimurghRTC::ValidateIceServers;
    TestFalse(TEXT("Default host ICE remains valid"), ValidateIceServers({}, false).IsError());
    TestTrue(TEXT("Relay-only requires TURN"), ValidateIceServers({}, true).IsError());
    FSimurghRTCIceServer Server; Server.Username = TEXT("fixture-user"); Server.Credential = TEXT("fixture-secret");
    for (const TCHAR* Url : {TEXT("stun:rtc.invalid"), TEXT("stuns:rtc.invalid:5349"), TEXT("turn:127.0.0.1:3478?transport=udp"),
        TEXT("turn:rtc.invalid?transport=tcp"), TEXT("turns:rtc.invalid:5349?transport=tcp"), TEXT("turn:[::1]:3478")})
    {
        Server.Urls = {Url}; TestFalse(TEXT("Supported ICE URI accepted"), ValidateIceServers({Server}, false).IsError());
    }
    for (const TCHAR* Url : {TEXT("https://rtc.invalid"), TEXT("turn://rtc.invalid"), TEXT("turn:user:secret@rtc.invalid"),
        TEXT("turn:rtc.invalid#secret"), TEXT("turn:rtc.invalid?transport=udp?extra=x"), TEXT("stun:rtc.invalid?transport=udp"),
        TEXT("turns:rtc.invalid?transport=udp"), TEXT("TURN:rtc.invalid"), TEXT("turn:"), TEXT("turn:rtc.invalid:"),
        TEXT("turn:rtc.invalid:65536"), TEXT("turn:rtc.invalid:0"), TEXT("turn:rtc.invalid:abc"), TEXT("turn:[::1]extra"), TEXT("turn:a b")})
    {
        Server.Urls = {Url}; const auto Error = ValidateIceServers({Server}, false);
        TestTrue(TEXT("Malformed ICE URI rejected"), Error.IsError());
        TestFalse(TEXT("Credential stays out of errors"), Error.Message.Contains(Server.Credential));
    }
    Server.Urls = {TEXT("turn:rtc.invalid")};
    TestFalse(TEXT("Authenticated relay-only accepted"), ValidateIceServers({Server}, true).IsError());
    Server.Credential.Reset(); TestTrue(TEXT("TURN needs credential"), ValidateIceServers({Server}, true).IsError());
    Server.Credential = FString::ChrN(513, 'x'); TestTrue(TEXT("Credential bytes bounded"), ValidateIceServers({Server}, true).IsError());
    const TCHAR EmbeddedNull[] = {'s', 0, 'e', 0}; Server.Credential = FString(3, EmbeddedNull);
    TestTrue(TEXT("Embedded null rejected"), ValidateIceServers({Server}, true).IsError());
    Server.Credential = TEXT("secret"); Server.Urls.Init(TEXT("turn:rtc.invalid"), 17);
    TestTrue(TEXT("URL count bounded"), ValidateIceServers({Server}, false).IsError());
    Server.Urls = {TEXT("stun:rtc.invalid")}; TArray<FSimurghRTCIceServer> Many; Many.Init(Server, 9);
    TestTrue(TEXT("Server count bounded"), ValidateIceServers(Many, false).IsError());
    TestTrue(TEXT("STUN alone cannot satisfy relay-only"), ValidateIceServers({Server}, true).IsError());
    FSessionFixture Fixture; auto Config = Fixture.Config(); Config.bRelayOnly = true;
    TestTrue(TEXT("Session applies ICE policy validation"), SimurghRTC::ValidateSessionConfig(Config).IsError());
    return true;
}

#endif
