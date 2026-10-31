// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDataExample.h"
#include "ContractChannelValidation.h"
#include "ContractNetworkValidation.h"
#include "ContractSendDeadlineValidation.h"
#include "ContractIceConfig.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

AContractDataExample::AContractDataExample()
{
    PrimaryActorTick.bCanEverTick = false;
    ApplicationLifecycle = CreateDefaultSubobject<UApplicationLifecycleComponent>(TEXT("ApplicationLifecycle"));
    Config.Provider = ESimurghRTCProvider::DirectUnreal;
    Config.Purpose = ESimurghRTCPurpose::Echo;
    Config.TimeoutMilliseconds = 20000;
    Options.Label = TEXT("rtc-example");
    TextToSend = TEXT("Hello from Unreal — سلام");
    BytesToSend = {0, 1, 2, 0, 127, 128, 255};
}

bool AContractDataExample::PrepareExample()
{
    Report = MakeShared<FContractDataExampleResult>();
    Report->Implementation = GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint) ? TEXT("blueprint") : TEXT("cpp");
    Report->ClassPath = GetClass()->GetPathName();
    LoadConnectionConfig();
    if (FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCDataExampleProbe")))
    {
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCExampleScenario="), Report->Scenario);
        if (Report->Scenario == TEXT("timeout")) Config.TimeoutMilliseconds = 250;
        if (Report->Scenario == TEXT("deadline-expire") || Report->Scenario == TEXT("deadline-release"))
        {
            Options.Label = TEXT("sr107-")+Report->Scenario;
            Report->SendDeadlineValidation = MakeShared<FContractSendDeadlineValidation>(Report->Scenario == TEXT("deadline-expire"));
        }
        if (Report->Scenario.StartsWith(TEXT("network-")))
        {
            const int32 Mode = FCString::Atoi(*Report->Scenario.Mid(8));
            if (Mode < 0 || Mode > 6 || Report->Scenario != FString::Printf(TEXT("network-%d"),Mode))
                Report->Fail(TEXT("Invalid network-validation scenario."));
            else { Options = FContractNetworkValidation::Options(Mode); Report->NetworkValidation = MakeShared<FContractNetworkValidation>(Mode); }
        }
        if (Report->Scenario.StartsWith(TEXT("channels-")))
        {
            const bool Small = Report->Scenario == TEXT("channels-small");
            const int32 Mode = Small ? 0 : FCString::Atoi(*Report->Scenario.Mid(9));
            if (Mode < 0 || Mode > 5 || (!Small && Report->Scenario != FString::Printf(TEXT("channels-%d"),Mode)))
                Report->Fail(TEXT("Invalid channel-validation scenario."));
            else
            {
                Options.Label = FString::Printf(TEXT("sr107-channels-%d"),Mode); Options.bOrdered = Mode % 2 == 0;
                Options.Reliability = static_cast<ESimurghRTCReliability>(Mode/2);
                Options.ReliabilityValue = Mode < 2 ? 0 : Mode < 4 ? (Mode == 2 ? 0 : 2) : 10000;
                Report->ChannelValidation = MakeShared<FContractChannelValidation>(Mode,Small ? 32768 : 65536);
            }
        }
    }
    SetRoundPayload();
    if (Config.Endpoint.IsEmpty() || Config.AccessToken.IsEmpty())
        Report->Fail(TEXT("Set an endpoint and provide a credential at runtime before playing this map."));
    if (!Report->Error.IsEmpty()) UE_LOG(LogTemp, Warning, TEXT("RTC example: %s"), *Report->Error);
    return Report->Error.IsEmpty();
}

bool AContractDataExample::LoadConnectionConfig()
{
    FString Endpoint, TokenFile;
    if (FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCExampleEndpoint="), Endpoint)) Config.Endpoint = Endpoint;
    if (FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCExampleTokenFile="), TokenFile))
    {
        const int64 Size = IFileManager::Get().FileSize(*TokenFile);
        if (Size <= 0 || Size > 4096 || !FFileHelper::LoadFileToString(Config.AccessToken, *TokenFile))
            Report->Fail(TEXT("The runtime credential file could not be read."));
        Config.AccessToken.TrimStartAndEndInline();
    }
    if (!LoadContractIceConfig(Config.IceServers, Config.bRelayOnly)) Report->Fail(TEXT("Invalid runtime ICE configuration."));
    Report->bRelayOnly = Config.bRelayOnly; Report->bHttps = Config.Endpoint.StartsWith(TEXT("https://"));
    if (Config.LocalIdentity.IsEmpty()) Config.LocalIdentity = TEXT("unreal-data-example");
    if (FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCExampleLoopback"))) Config.bAllowInsecureDevelopmentSignaling = true;
    if (Config.Endpoint.IsEmpty() || Config.AccessToken.IsEmpty()) Report->Fail(TEXT("Runtime connection credentials are unavailable."));
    return Report->Error.IsEmpty();
}
void AContractDataExample::SetRoundPayload()
{
    if (Report->IsTwoConnections())
    {
        TextToSend = FString::Printf(TEXT("Unreal recovery round %d — سلام"),Report->Round);
        BytesToSend = {0, 1, 2, 0, 127, 128, 255, uint8(Report->Round)};
    }
}
void AContractDataExample::CheckStaleChannel()
{
    if (!Report || !Session || Report->ChannelIds.IsEmpty()) return;
    FSimurghRTCChannelHandle Old; Old.Id = Report->ChannelIds[0]; FSimurghRTCChannelInfo Info;
    const auto Query = Session->GetChannelInfo(Old,Info);
    const auto Text = Session->SendText(Old,TEXT("must-not-send"));
    const auto Bytes = Session->SendBytes(Old,{42});
    if (Query.Code != ESimurghRTCErrorCode::InvalidState || Info.Handle.IsValid()
        || Text.Code != ESimurghRTCErrorCode::InvalidState || Bytes.Code != ESimurghRTCErrorCode::InvalidState)
        Report->Fail(TEXT("A retired channel remained usable."));
    else Report->StaleHandleChecks += 3;
}
bool AContractDataExample::PrepareNextRound()
{
    if (!Report || !Report->IsRecovery() || Report->Round != 0) return false;
    const auto Expected = Report->Scenario == TEXT("reconnect") ? ESimurghRTCSessionState::Closed : ESimurghRTCSessionState::Failed;
    if (!Session || Session->GetState() != Expected || Report->CompletedRounds != 1 || Report->ChannelIds.Num() != 1
        || (Expected == ESimurghRTCSessionState::Failed && !Report->bFailureObserved))
    { Report->Fail(TEXT("Recovery started before the first round and cleanup state.")); return false; }
    CheckStaleChannel();
    ++Report->Round; Report->bTextReceived = Report->bBytesReceived = Report->bRoundRecorded = Report->bAwaitingFailure = false;
    Channel = {}; SetRoundPayload();
    return LoadConnectionConfig();
}
void AContractDataExample::RequestConnect_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its Connect flow.")); }

void AContractDataExample::TrackOperation(USimurghRTCOperation* Operation)
{
    if (!Report) return;
    PendingOperation = Operation;
    if (!Session || !Operation || Report->StartedIds.Contains(Operation->GetOperationId()))
    { Report->Fail(TEXT("Missing session or duplicate operation.")); return; }
    if (Report->SessionId.IsValid() && Report->SessionId != Session->GetSessionId()) Report->Fail(TEXT("Reconnect replaced the facade identity."));
    Report->SessionId = Session->GetSessionId();
    if (!Report->SessionIds.Contains(Report->SessionId)) Report->SessionIds.Add(Report->SessionId);
    Report->StartedIds.Add(Operation->GetOperationId());
    // Only Connect consumes the credential; the sample actor need not retain it afterward.
    Config.AccessToken.Reset(); Config.IceServers.Reset();
}

void AContractDataExample::RecordOutcome(FName Step, FName Branch, const FSimurghRTCOperationResult& Result)
{
    if (!Report) return;
    Report->Callback();
    const auto Code = Result.Error.Code;
    const FName ExpectedBranch = Code == ESimurghRTCErrorCode::None ? TEXT("Success")
        : Code == ESimurghRTCErrorCode::Cancelled ? TEXT("Cancelled") : TEXT("Failure");
    if (!Session || Result.Error.SessionId != Session->GetSessionId() || !Result.Error.OperationId.IsValid()
        || !Report->StartedIds.Contains(Result.Error.OperationId) || Report->CompletedIds.Contains(Result.Error.OperationId)
        || Branch != ExpectedBranch)
        Report->Fail(TEXT("Invalid completion branch or operation correlation."));
    Report->CompletedIds.Add(Result.Error.OperationId);
    Report->Outcomes.Add(Step.ToString() + TEXT(":") + Branch.ToString());
    if (Code != ESimurghRTCErrorCode::None)
    {
        Report->LastFailure = int32(Code);
        const bool Expected = Step == TEXT("Connect") &&
            ((Report->Scenario == TEXT("timeout") && Code == ESimurghRTCErrorCode::Timeout)
             || (Report->Scenario == TEXT("unauthorized") && Code == ESimurghRTCErrorCode::Unauthorized)
             || (Report->Scenario == TEXT("tls-rejected") && Code == ESimurghRTCErrorCode::InternalError)
             || (Report->Scenario == TEXT("disconnect-pending") && Code == ESimurghRTCErrorCode::Cancelled));
        if (!Expected || Result.Channel.IsValid()) Report->Fail(TEXT("Unexpected operation failure or fabricated channel."));
    }
    else if (Step == TEXT("OpenChannel") && !Result.Channel.IsValid()) Report->Fail(TEXT("OpenChannel returned no handle."));
    UE_LOG(LogTemp, Display, TEXT("RTC %s: %s %s"), *Report->Implementation, *Step.ToString(), *Branch.ToString());
}

void AContractDataExample::RecordChannelInfo(const FSimurghRTCError& Error, const FSimurghRTCChannelInfo& Info)
{
    if (!Report) return;
    Report->bInfoChecked = !Error.IsError() && Session && Error.SessionId == Session->GetSessionId()
        && Info.Handle.Id == Channel.Id && Info.bOpen && Info.MaximumMessageBytes > 0 && Info.Options.Label == Options.Label;
    if (!Report->bInfoChecked) Report->Fail(TEXT("The opened channel has no usable negotiated information."));
    if (Report->ChannelIds.Contains(Channel.Id)) Report->Fail(TEXT("Reconnect reused a channel handle."));
    else Report->ChannelIds.Add(Channel.Id);
    if (Report->Round > 0 && Report->ChannelIds.Num() > 1) CheckStaleChannel();
    if (Session && Session->GetConnectionError().IsError()) Report->Fail(TEXT("A new connection retained the previous failure."));
}
void AContractDataExample::RecordSend(const FSimurghRTCError& Error)
{
    if (!Report) return;
    if (Error.IsError() || !Session || Error.SessionId != Session->GetSessionId()) Report->Fail(TEXT("Sample send was rejected."));
    else ++Report->Sent;
}
bool AContractDataExample::RecordMessage(const FSimurghRTCDataMessage& Message)
{
    if (!Report) return false;
    if (Report->SendDeadlineValidation) { Report->SendDeadlineValidation->Message(*this,Message); return false; }
    if (Report->NetworkValidation) { Report->NetworkValidation->Message(*this,Message); return false; }
    if (Report->ChannelValidation) { Report->ChannelValidation->Message(*this,Message); return false; }
    Report->Callback(); ++Report->Received;
    if (Message.Channel.Id != Channel.Id) Report->Fail(TEXT("Message arrived on a different channel."));
    if (Message.Kind == ESimurghRTCMessageKind::Text && !Report->bTextReceived && Message.Text == TextToSend && Message.Bytes.IsEmpty())
        Report->bTextReceived = true;
    else if (Message.Kind == ESimurghRTCMessageKind::Binary && !Report->bBytesReceived && Message.Bytes == BytesToSend && Message.Text.IsEmpty())
        Report->bBytesReceived = true;
    else Report->Fail(TEXT("Payload mismatch or duplicate message."));
    const bool Both = Report->Error.IsEmpty() && Report->bTextReceived && Report->bBytesReceived;
    if (Both && !Report->bRoundRecorded) { Report->bRoundRecorded = true; ++Report->CompletedRounds; }
    if (Both && Report->Round == 0 && (Report->Scenario == TEXT("signaling-loss") || Report->Scenario == TEXT("relay-loss") || Report->Scenario == TEXT("background-connected") || Report->Scenario == TEXT("terminate-connected")))
    { Report->bAwaitingFailure = true; return false; }
    return Both;
}
void AContractDataExample::RecordChannelEvent(const FSimurghRTCChannelInfo& Info)
{
    if (!Report) return;
    Report->Callback(); ++Report->ChannelEvents;
    if (Report->ChannelValidation) Report->ChannelValidation->Changed(Info);
    if (Info.bOpen) ++Report->OpenEvents;
    if (Report->NetworkValidation) { Report->NetworkValidation->Changed(*this,Info); return; }
    if (!Info.Handle.IsValid() || Info.Options.Label != Options.Label) Report->Fail(TEXT("Unexpected channel event."));
}
void AContractDataExample::RecordStateEvent(ESimurghRTCSessionState State)
{
    if (!Report) return;
    Report->Callback(); Report->States.Add(State);
    if (State == ESimurghRTCSessionState::Failed && Report->bAwaitingFailure)
    {
        const auto Error = Session->GetConnectionError();
        if (!Error.IsError() || Error.SessionId != Report->SessionId || Error.OperationId.IsValid()
            || Report->bFailureObserved) Report->Fail(TEXT("Invalid or repeated connection-loss error."));
        Report->ConnectionErrors.Add(int32(Error.Code)); Report->bFailureObserved = true;
    }
}
void AContractDataExample::CompleteExample()
{
    if (!Report) return;
    if (!Session || Session->GetState() != ESimurghRTCSessionState::Closed) Report->Fail(TEXT("Disconnect did not close the session."));
    Report->bComplete = true;
}
void AContractDataExample::RecordEndPlay()
{
    FTSTicker::RemoveTicker(ForegroundTicker); ForegroundTicker.Reset();
    if (!Report) return;
    ++Report->EndPlayCalls;
    Report->bEndPlayClosed = Session && !Session->IsUsable() && Session->GetState() == ESimurghRTCSessionState::Closed;
    if (!Report->bEndPlayClosed) Report->Fail(TEXT("EndPlay did not immediately shut down the session."));
    Report->bEnded = true;
}
void AContractDataExample::RequestDisconnect_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its Disconnect flow.")); }
bool AContractDataExample::BeginChannelValidation()
{
    if (Report && Report->SendDeadlineValidation) { Report->SendDeadlineValidation->Opened(*this); return true; }
    if (Report && Report->NetworkValidation) { Report->NetworkValidation->Opened(*this); return true; }
    if (!Report || !Report->ChannelValidation) return false;
    Report->ChannelValidation->Opened(*this); return true;
}
void AContractDataExample::RecordValidationSend(const FSimurghRTCError& Error)
{ if (Report && Report->SendDeadlineValidation) Report->SendDeadlineValidation->SendResult(*this,Error);
  if (Report && Report->ChannelValidation) Report->ChannelValidation->SendResult(*this,Error);
  if (Report && Report->NetworkValidation) Report->NetworkValidation->SendResult(*this,Error); }
void AContractDataExample::TickChannelValidation()
{ if (Report && Report->SendDeadlineValidation) Report->SendDeadlineValidation->Tick(*this);
  if (Report && Report->ChannelValidation) Report->ChannelValidation->Tick(*this);
  if (Report && Report->NetworkValidation) Report->NetworkValidation->Tick(*this); }
bool AContractDataExample::FinishChannelClose()
{ return !Report || !Report->NetworkValidation || Report->NetworkValidation->Closed(*this); }
void AContractDataExample::RequestOpenChannel_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its channel open flow.")); }
void AContractDataExample::RequestValidationSend_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its validation send flow.")); }
void AContractDataExample::RequestCloseChannel_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its channel close flow.")); }

void AContractDataCpp::BeginPlay()
{
    Super::BeginPlay();
    if (!PrepareExample()) return;
    ApplicationLifecycle->ApplicationWillDeactivateDelegate.AddDynamic(this,&AContractDataCpp::EnterBackground);
    ApplicationLifecycle->ApplicationWillEnterBackgroundDelegate.AddDynamic(this,&AContractDataExample::RecordOSBackground);
    ApplicationLifecycle->ApplicationHasReactivatedDelegate.AddDynamic(this,&AContractDataCpp::EnterForeground);
    ApplicationLifecycle->ApplicationHasEnteredForegroundDelegate.AddDynamic(this,&AContractDataExample::RecordOSForeground);
    RequestSession();
}
void AContractDataCpp::RequestSession_Implementation()
{
    Session = USimurghRTCSession::CreateSession(this);
    Session->OnMessage.AddDynamic(this, &AContractDataCpp::ReceivedMessage);
    Session->OnChannelChanged.AddDynamic(this, &AContractDataCpp::ChannelChanged);
    Session->OnStateChanged.AddDynamic(this, &AContractDataCpp::StateChanged);
    RequestConnect();
}
void AContractDataCpp::EnterBackground()
{
    if (PrepareBackground()) { Session->Shutdown(); RecordBackgroundShutdown(); }
}
void AContractDataCpp::EnterForeground()
{
    ScheduleForeground();
}
void AContractDataCpp::Start(USimurghRTCOperation* Operation, FName Step)
{
    TrackOperation(Operation);
    Operation->OnCompletedNative.AddWeakLambda(this, [this, Step](const FSimurghRTCOperationResult& Result) { Completed(Step, Result); });
    Operation->Activate();
}
void AContractDataCpp::Completed(FName Step, const FSimurghRTCOperationResult& Result)
{
    RecordOutcome(Step, Result.IsSuccess() ? TEXT("Success") : Result.Error.Code == ESimurghRTCErrorCode::Cancelled ? TEXT("Cancelled") : TEXT("Failure"), Result);
    if (Step == TEXT("Disconnect")) { if (PrepareNextRound()) RequestConnect(); else CompleteExample(); return; }
    if (!Result.IsSuccess()) { if (Result.Error.Code != ESimurghRTCErrorCode::Cancelled) RequestDisconnect(); return; }
    if (Step == TEXT("Connect")) RequestOpenChannel();
    else if (Step == TEXT("OpenChannel"))
    {
        Channel = Result.Channel;
        FSimurghRTCChannelInfo Info; const auto Error = Session->GetChannelInfo(Channel, Info); RecordChannelInfo(Error, Info);
        if (BeginChannelValidation()) return;
        RecordSend(Session->SendText(Channel, TextToSend));
        RecordSend(Session->SendBytes(Channel, BytesToSend));
    }
    else if (Step == TEXT("CloseChannel") && FinishChannelClose()) RequestDisconnect();
}
void AContractDataCpp::ReceivedMessage(const FSimurghRTCDataMessage& Message)
{ if (RecordMessage(Message)) RequestCloseChannel(); }
void AContractDataCpp::RequestOpenChannel_Implementation()
{ Start(USimurghRTCOperation::OpenChannel(Session, Options), TEXT("OpenChannel")); }
void AContractDataCpp::RequestCloseChannel_Implementation()
{ Start(USimurghRTCOperation::CloseChannel(Session, Channel), TEXT("CloseChannel")); }
void AContractDataCpp::RequestValidationSend_Implementation()
{ RecordValidationSend(bValidationText ? Session->SendText(Channel,TextToSend) : Session->SendBytes(Channel,BytesToSend)); }
void AContractDataCpp::ChannelChanged(const FSimurghRTCChannelInfo& Info) { RecordChannelEvent(Info); }
void AContractDataCpp::StateChanged(ESimurghRTCSessionState State) { RecordStateEvent(State); }
void AContractDataCpp::RequestConnect_Implementation() { Start(USimurghRTCOperation::Connect(Session, Config), TEXT("Connect")); }
void AContractDataCpp::RequestDisconnect_Implementation() { Start(USimurghRTCOperation::Disconnect(Session), TEXT("Disconnect")); }
void AContractDataCpp::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Session) Session->Shutdown();
    RecordEndPlay();
    Super::EndPlay(Reason);
}
