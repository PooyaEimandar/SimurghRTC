// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractLifecycleExample.h"

AContractLifecycleExample::AContractLifecycleExample()
{
    PrimaryActorTick.bCanEverTick = false;
    Config.Provider = SIMURGHRTC_WITH_DIRECT ? ESimurghRTCProvider::DirectUnreal : ESimurghRTCProvider::Janus;
    Config.Endpoint = TEXT("https://rtc.invalid/session");
}

void AContractLifecycleExample::RecordOutcome(FName Step, const FSimurghRTCOperationResult& Result)
{
    const int32 Index = Outcomes.Num();
    const FName Steps[] = {TEXT("Disconnect"), TEXT("Connect"), TEXT("Cancel")};
    const bool Direct = Config.Provider == ESimurghRTCProvider::DirectUnreal;
    const ESimurghRTCErrorCode Codes[] = {ESimurghRTCErrorCode::None,
        Direct ? ESimurghRTCErrorCode::InvalidArgument : ESimurghRTCErrorCode::NotImplemented, ESimurghRTCErrorCode::Cancelled};
    const auto FailedState = Direct ? ESimurghRTCSessionState::Closed : ESimurghRTCSessionState::Failed;
    const ESimurghRTCSessionState States[] = {ESimurghRTCSessionState::Closed, FailedState, FailedState};
    if (Index >= 3 || Step != Steps[Index] || Result.Error.Code != Codes[Index]
        || !Session || Result.Error.SessionId != Session->GetSessionId() || !Result.Error.OperationId.IsValid()
        || OperationIds.Contains(Result.Error.OperationId) || (Index > 0 && Session->GetProvider() != Config.Provider)
        || Session->GetState() != States[Index] || Result.Channel.IsValid() || Result.Source.IsValid() || Result.Track.IsValid())
        Errors.Add(TEXT("Unexpected outcome, state, correlation or fabricated handle."));
    OperationIds.Add(Result.Error.OperationId);
    Outcomes.Add(Step.ToString());
    UE_LOG(LogTemp, Display, TEXT("RTC lifecycle %s: %s (%s)"), *GetClass()->GetName(), *Step.ToString(),
        *StaticEnum<ESimurghRTCErrorCode>()->GetNameStringByValue(int64(Result.Error.Code)));
}

void AContractLifecycleExample::UnexpectedCompletion(const FSimurghRTCOperationResult&)
{
    Errors.Add(TEXT("An unexpected completion branch fired."));
}

void AContractLifecycleExample::VerifyQuietTeardown(USimurghRTCOperation* Operation)
{
    PendingOperation = Operation;
    if (!Operation || !Operation->IsFinished() || Operation->GetResult().Error.Code != ESimurghRTCErrorCode::Cancelled
        || !Session || Operation->GetResult().Error.SessionId != Session->GetSessionId()
        || OperationIds.Contains(Operation->GetOperationId())
        || Session->GetState() != ESimurghRTCSessionState::Closed || Session->IsUsable())
        Errors.Add(TEXT("Shutdown did not silently cancel its pending operation."));
    bComplete = true;
}

bool AContractLifecycleExample::PrepareEndPlayCheck()
{
    if (!Passed()) return false;
    Session = USimurghRTCSession::CreateSession(this);
    PendingOperation = USimurghRTCOperation::Connect(Session, Config);
    PendingOperation->OnCompletedNative.AddUObject(this, &AContractLifecycleExample::UnexpectedCompletion);
    PendingOperation->Activate();
    Destroy(); // The C++ override / Blueprint EndPlay graph must shut down immediately.
    return EndPlayPassed();
}

bool AContractLifecycleExample::EndPlayPassed() const
{
    return Passed() && Session && !Session->IsUsable() && Session->GetState() == ESimurghRTCSessionState::Closed
        && PendingOperation && PendingOperation->IsFinished()
        && PendingOperation->GetResult().Error.Code == ESimurghRTCErrorCode::Cancelled
        && PendingOperation->GetResult().Error.SessionId == Session->GetSessionId();
}

void AContractLifecycleCpp::BeginPlay()
{
    Super::BeginPlay();
    Session = USimurghRTCSession::CreateSession(this);
    PendingOperation = USimurghRTCOperation::Disconnect(Session);
    PendingOperation->OnCompletedNative.AddUObject(this, &AContractLifecycleCpp::AfterDisconnect);
    PendingOperation->Activate();
}

void AContractLifecycleCpp::AfterDisconnect(const FSimurghRTCOperationResult& Result)
{
    RecordOutcome(TEXT("Disconnect"), Result);
    PendingOperation = USimurghRTCOperation::Connect(Session, Config);
    PendingOperation->OnCompletedNative.AddUObject(this, &AContractLifecycleCpp::AfterConnect);
    PendingOperation->Activate();
}

void AContractLifecycleCpp::AfterConnect(const FSimurghRTCOperationResult& Result)
{
    RecordOutcome(TEXT("Connect"), Result);
    PendingOperation = USimurghRTCOperation::Connect(Session, Config);
    PendingOperation->OnCompletedNative.AddUObject(this, &AContractLifecycleCpp::AfterCancel);
    PendingOperation->Activate();
    PendingOperation->Cancel();
}

void AContractLifecycleCpp::AfterCancel(const FSimurghRTCOperationResult& Result)
{
    RecordOutcome(TEXT("Cancel"), Result);
    PendingOperation = USimurghRTCOperation::Connect(Session, Config);
    PendingOperation->OnCompletedNative.AddUObject(this, &AContractLifecycleExample::UnexpectedCompletion);
    PendingOperation->Activate();
    Session->Shutdown();
    VerifyQuietTeardown(PendingOperation);
}

void AContractLifecycleCpp::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Session) Session->Shutdown();
    Super::EndPlay(Reason);
}
