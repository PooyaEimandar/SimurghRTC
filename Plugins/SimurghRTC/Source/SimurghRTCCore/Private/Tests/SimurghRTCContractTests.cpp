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
    Config.TimeoutMilliseconds = 0;
    TestEqual(TEXT("Timeout validated in C++ too"), SimurghRTC::ValidateSessionConfig(Config).Code, ESimurghRTCErrorCode::InvalidArgument);

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
    TestEqual(TEXT("No fabricated transport success"), Operation->GetResult().Error.Code, ESimurghRTCErrorCode::NotImplemented);
    TestEqual(TEXT("Failed connection state"), Fixture.Session->GetState(), ESimurghRTCSessionState::Failed);
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
    TestEqual(TEXT("Janus compiled availability matches selected profile"), Janus.Compiled,
        SIMURGHRTC_WITH_JANUS ? ESimurghRTCSupport::Available : ESimurghRTCSupport::Unavailable);
    TestEqual(TEXT("Build exclusion differs from implementation pending"), Janus.Reason.Code,
        SIMURGHRTC_WITH_JANUS ? ESimurghRTCErrorCode::NotImplemented : ESimurghRTCErrorCode::BuildExcluded);
    const UClass* JanusClass = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCJanusExtensions.SimurghRTCJanusOperation"));
    const UClass* MediaClass = FindObject<UClass>(nullptr, TEXT("/Script/SimurghRTCMedia.SimurghRTCMediaOperation"));
    TestEqual(TEXT("Optional Janus reflection matches profile"), JanusClass != nullptr, bool(SIMURGHRTC_WITH_JANUS && SIMURGHRTC_WITH_MEDIA));
    TestEqual(TEXT("Optional media reflection matches profile"), MediaClass != nullptr, bool(SIMURGHRTC_WITH_MEDIA));
    for (const TCHAR* Name : {TEXT("Connect"), TEXT("Disconnect"), TEXT("OpenChannel"), TEXT("CloseChannel"), TEXT("Call"), TEXT("AcceptCall"), TEXT("RejectCall"), TEXT("HangUp"), TEXT("JoinTextRoom"), TEXT("LeaveTextRoom"), TEXT("SendRoomText"), TEXT("Cancel")})
    {
        const UFunction* Function = USimurghRTCOperation::StaticClass()->FindFunctionByName(Name);
        TestTrue(FString::Printf(TEXT("%s is Blueprint callable"), Name), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
    }
    TestFalse(TEXT("No fabricated statistics"), Fixture.Session->GetStatistics().bAvailable);
    TestEqual(TEXT("Invalid channel is rejected"), Fixture.Session->SendText({}, TEXT("hello")).Code, ESimurghRTCErrorCode::InvalidArgument);
    return true;
}

#endif
