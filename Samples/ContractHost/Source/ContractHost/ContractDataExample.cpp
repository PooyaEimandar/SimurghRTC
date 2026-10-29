// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDataExample.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

AContractDataExample::AContractDataExample()
{
    PrimaryActorTick.bCanEverTick = false;
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
    FString Endpoint, TokenFile;
    if (FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCExampleEndpoint="), Endpoint)) Config.Endpoint = Endpoint;
    if (FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCExampleTokenFile="), TokenFile))
    {
        const int64 Size = IFileManager::Get().FileSize(*TokenFile);
        if (Size <= 0 || Size > 4096 || !FFileHelper::LoadFileToString(Config.AccessToken, *TokenFile))
            Report->Fail(TEXT("The runtime credential file could not be read."));
        Config.AccessToken.TrimStartAndEndInline();
    }
    if (Config.LocalIdentity.IsEmpty()) Config.LocalIdentity = TEXT("unreal-data-example");
    if (FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCExampleLoopback"))) Config.bAllowInsecureDevelopmentSignaling = true;
    if (FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCDataExampleProbe")))
    {
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCExampleScenario="), Report->Scenario);
        if (Report->Scenario == TEXT("timeout")) Config.TimeoutMilliseconds = 250;
    }
    if (Config.Endpoint.IsEmpty() || Config.AccessToken.IsEmpty())
        Report->Fail(TEXT("Set an endpoint and provide a credential at runtime before playing this map."));
    if (!Report->Error.IsEmpty()) UE_LOG(LogTemp, Warning, TEXT("RTC example: %s"), *Report->Error);
    return Report->Error.IsEmpty();
}

void AContractDataExample::TrackOperation(USimurghRTCOperation* Operation)
{
    if (!Report) return;
    PendingOperation = Operation;
    if (!Session || !Operation || Report->StartedIds.Contains(Operation->GetOperationId()))
    { Report->Fail(TEXT("Missing session or duplicate operation.")); return; }
    Report->SessionId = Session->GetSessionId();
    Report->StartedIds.Add(Operation->GetOperationId());
    // Only Connect consumes the credential; the sample actor need not retain it afterward.
    Config.AccessToken.Reset();
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
    Report->Callback(); ++Report->Received;
    if (Message.Channel.Id != Channel.Id) Report->Fail(TEXT("Message arrived on a different channel."));
    if (Message.Kind == ESimurghRTCMessageKind::Text && !Report->bTextReceived && Message.Text == TextToSend && Message.Bytes.IsEmpty())
        Report->bTextReceived = true;
    else if (Message.Kind == ESimurghRTCMessageKind::Binary && !Report->bBytesReceived && Message.Bytes == BytesToSend && Message.Text.IsEmpty())
        Report->bBytesReceived = true;
    else Report->Fail(TEXT("Payload mismatch or duplicate message."));
    return Report->Error.IsEmpty() && Report->bTextReceived && Report->bBytesReceived;
}
void AContractDataExample::RecordChannelEvent(const FSimurghRTCChannelInfo& Info)
{
    if (!Report) return;
    Report->Callback(); ++Report->ChannelEvents;
    if (Info.bOpen) ++Report->OpenEvents;
    if (!Info.Handle.IsValid() || Info.Options.Label != Options.Label) Report->Fail(TEXT("Unexpected channel event."));
}
void AContractDataExample::RecordStateEvent(ESimurghRTCSessionState State)
{ if (Report) { Report->Callback(); Report->States.Add(State); } }
void AContractDataExample::CompleteExample()
{
    if (!Report) return;
    if (!Session || Session->GetState() != ESimurghRTCSessionState::Closed) Report->Fail(TEXT("Disconnect did not close the session."));
    Report->bComplete = true;
}
void AContractDataExample::RecordEndPlay()
{
    if (!Report) return;
    ++Report->EndPlayCalls;
    Report->bEndPlayClosed = Session && !Session->IsUsable() && Session->GetState() == ESimurghRTCSessionState::Closed;
    if (!Report->bEndPlayClosed) Report->Fail(TEXT("EndPlay did not immediately shut down the session."));
    Report->bEnded = true;
}
void AContractDataExample::RequestDisconnect_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its Disconnect flow.")); }

void AContractDataCpp::BeginPlay()
{
    Super::BeginPlay();
    if (!PrepareExample()) return;
    Session = USimurghRTCSession::CreateSession(this);
    Session->OnMessage.AddDynamic(this, &AContractDataCpp::ReceivedMessage);
    Session->OnChannelChanged.AddDynamic(this, &AContractDataCpp::ChannelChanged);
    Session->OnStateChanged.AddDynamic(this, &AContractDataCpp::StateChanged);
    Start(USimurghRTCOperation::Connect(Session, Config), TEXT("Connect"));
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
    if (Step == TEXT("Disconnect")) { CompleteExample(); return; }
    if (!Result.IsSuccess()) { if (Result.Error.Code != ESimurghRTCErrorCode::Cancelled) RequestDisconnect(); return; }
    if (Step == TEXT("Connect")) Start(USimurghRTCOperation::OpenChannel(Session, Options), TEXT("OpenChannel"));
    else if (Step == TEXT("OpenChannel"))
    {
        Channel = Result.Channel;
        FSimurghRTCChannelInfo Info; const auto Error = Session->GetChannelInfo(Channel, Info); RecordChannelInfo(Error, Info);
        RecordSend(Session->SendText(Channel, TextToSend));
        RecordSend(Session->SendBytes(Channel, BytesToSend));
    }
    else if (Step == TEXT("CloseChannel")) RequestDisconnect();
}
void AContractDataCpp::ReceivedMessage(const FSimurghRTCDataMessage& Message)
{ if (RecordMessage(Message)) Start(USimurghRTCOperation::CloseChannel(Session, Channel), TEXT("CloseChannel")); }
void AContractDataCpp::ChannelChanged(const FSimurghRTCChannelInfo& Info) { RecordChannelEvent(Info); }
void AContractDataCpp::StateChanged(ESimurghRTCSessionState State) { RecordStateEvent(State); }
void AContractDataCpp::RequestDisconnect_Implementation() { Start(USimurghRTCOperation::Disconnect(Session), TEXT("Disconnect")); }
void AContractDataCpp::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Session) Session->Shutdown();
    RecordEndPlay();
    Super::EndPlay(Reason);
}
