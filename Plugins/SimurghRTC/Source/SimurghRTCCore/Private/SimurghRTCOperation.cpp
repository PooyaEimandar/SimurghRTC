#include "SimurghRTCOperation.h"
#include "SimurghRTCSession.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

void USimurghRTCOperation::Initialize(USimurghRTCSession* InSession, FName Name, ESimurghRTCFeature Feature, const FSimurghRTCError& Validation)
{
    check(IsInGameThread());
    Session = InSession;
    OperationId = FGuid::NewGuid();
    SessionId = IsValid(InSession) ? InSession->GetSessionId() : FGuid();
    OperationName = Name;
    RequiredFeature = Feature;
    ValidationError = Validation;
    Result.Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Operation has not completed."));
    Result.Error.SessionId = SessionId;
    Result.Error.OperationId = OperationId;
}

USimurghRTCOperation* USimurghRTCOperation::Connect(USimurghRTCSession* InSession, const FSimurghRTCSessionConfig& Config)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    Validation = SimurghRTC::ValidateSessionConfig(Config);
    Operation->ConnectConfig = Config;
    Operation->Initialize(InSession, TEXT("Connect"), ESimurghRTCFeature::DataChannels, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::Disconnect(USimurghRTCSession* InSession)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    Operation->Initialize(InSession, TEXT("Disconnect"), ESimurghRTCFeature::DataChannels, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::OpenChannel(USimurghRTCSession* InSession, const FSimurghRTCChannelOptions& Options)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    Validation = SimurghRTC::ValidateChannelOptions(Options);
    Operation->Initialize(InSession, TEXT("OpenChannel"), ESimurghRTCFeature::DataChannels, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::CloseChannel(USimurghRTCSession* InSession, FSimurghRTCChannelHandle Channel)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (!Channel.IsValid()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Channel handle is required."));
    Operation->Initialize(InSession, TEXT("CloseChannel"), ESimurghRTCFeature::DataChannels, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::Call(USimurghRTCSession* InSession, const FString& PeerId)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (PeerId.IsEmpty()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("An opaque identity is required."));
    Operation->Initialize(InSession, TEXT("Call"), ESimurghRTCFeature::IncomingCalls, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::AcceptCall(USimurghRTCSession* InSession, const FString& CallId)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (CallId.IsEmpty()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("An opaque identity is required."));
    Operation->Initialize(InSession, TEXT("AcceptCall"), ESimurghRTCFeature::IncomingCalls, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::RejectCall(USimurghRTCSession* InSession, const FString& CallId)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (CallId.IsEmpty()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("An opaque identity is required."));
    Operation->Initialize(InSession, TEXT("RejectCall"), ESimurghRTCFeature::IncomingCalls, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::HangUp(USimurghRTCSession* InSession)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    Operation->Initialize(InSession, TEXT("HangUp"), ESimurghRTCFeature::IncomingCalls, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::JoinTextRoom(USimurghRTCSession* InSession, const FString& RoomId)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (RoomId.IsEmpty()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("An opaque identity is required."));
    Operation->Initialize(InSession, TEXT("JoinTextRoom"), ESimurghRTCFeature::TextRooms, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::LeaveTextRoom(USimurghRTCSession* InSession, const FString& RoomId)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (RoomId.IsEmpty()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("An opaque identity is required."));
    Operation->Initialize(InSession, TEXT("LeaveTextRoom"), ESimurghRTCFeature::TextRooms, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::SendRoomText(USimurghRTCSession* InSession, const FString& RoomId, const FString& Text)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (RoomId.IsEmpty()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("An opaque identity is required."));
    Operation->Initialize(InSession, TEXT("SendRoomText"), ESimurghRTCFeature::TextRooms, Validation);
    return Operation;
}

void USimurghRTCOperation::Activate()
{
    check(IsInGameThread());
    if (bActivated || bFinished) return;
    bActivated = true;
    if (Session.IsValid() && Session->IsUsable())
    {
        Session->PendingOperations.Add(this);
        if (UWorld* World = Session->GetWorld(); World && World->GetGameInstance())
            RegisterWithGameInstance(Session.Get());
    }
    TWeakObjectPtr<USimurghRTCOperation> WeakThis(this);
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
    {
        TStrongObjectPtr<USimurghRTCOperation> KeepAlive(WeakThis.Get());
        if (auto* Self = KeepAlive.Get())
        {
            Self->TickHandle.Reset();
            Self->Execute();
        }
        return false;
    }));
}

void USimurghRTCOperation::Execute()
{
    check(IsInGameThread());
    if (bFinished) return;
    if (!Session.IsValid())
    {
        // A null factory input can fail normally; a destroyed session suppresses callbacks.
        Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("A live session is required.")), !SessionId.IsValid());
        return;
    }
    if (!Session->IsUsable()) { Abandon(); return; }
    if (ValidationError.IsError()) { Finish(ValidationError); return; }
    if (OperationName == TEXT("Disconnect"))
    {
        TArray<TWeakObjectPtr<USimurghRTCOperation>> Pending;
        for (USimurghRTCOperation* Other : Session->PendingOperations) Pending.Add(Other);
        for (const auto& Other : Pending)
            if (Other.IsValid() && Other.Get() != this) Other->Cancel();
        if (!Session.IsValid() || !Session->IsUsable() || bFinished) return;
        Session->SetState(ESimurghRTCSessionState::Closed);
        Finish({});
        return;
    }
    if (OperationName == TEXT("Connect"))
    {
        const auto State = Session->GetState();
        if (State != ESimurghRTCSessionState::Idle && State != ESimurghRTCSessionState::Closed && State != ESimurghRTCSessionState::Failed)
        {
            Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Disconnect before connecting or changing providers.")));
            return;
        }
        Session->Provider = ConnectConfig.Provider;
        Session->SetState(ESimurghRTCSessionState::Connecting);
        if (!Session.IsValid() || !Session->IsUsable() || bFinished) return;
        Session->SetState(ESimurghRTCSessionState::Failed);
        Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Native transport is not implemented in M0; no connection was opened.")));
        return;
    }
    const auto Capability = Session->GetCapability(Session->GetProvider(), RequiredFeature);
    if (Capability.Reason.Code == ESimurghRTCErrorCode::BuildExcluded || Capability.Reason.Code == ESimurghRTCErrorCode::ServiceUnsupported)
    {
        Finish(Capability.Reason);
        return;
    }
    if (Session->GetState() != ESimurghRTCSessionState::Connected)
    {
        Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("This operation requires a connected session.")));
        return;
    }
    Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("This operation is not implemented in M0.")));
}

void USimurghRTCOperation::Finish(FSimurghRTCError Error, bool bBroadcast)
{
    if (bFinished) return;
    // Listeners may release the last application reference or collect garbage.
    TStrongObjectPtr<USimurghRTCOperation> KeepAlive(bBroadcast ? this : nullptr);
    bFinished = true;
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
    Error.SessionId = SessionId;
    Error.OperationId = OperationId;
    Result.Error = MoveTemp(Error);
    // Credentials are needed only during a connect attempt; never retain them in results/logs.
    ConnectConfig = {};
    if (bBroadcast && Result.Error.Code == ESimurghRTCErrorCode::Cancelled
        && OperationName == TEXT("Connect") && Session.IsValid() && Session->IsUsable()
        && Session->GetState() == ESimurghRTCSessionState::Connecting)
        Session->SetState(ESimurghRTCSessionState::Idle);
    if (bBroadcast && (!SessionId.IsValid() || (Session.IsValid() && Session->IsUsable())))
    {
        OnCompletedNative.Broadcast(Result);
        // Native listeners may tear down the owner/world; do not enter its Blueprint afterward.
        if (!SessionId.IsValid() || (Session.IsValid() && Session->IsUsable()))
        {
            if (Result.Error.Code == ESimurghRTCErrorCode::Cancelled) OnCancelled.Broadcast(Result);
            else if (Result.IsSuccess()) OnSuccess.Broadcast(Result);
            else OnFailure.Broadcast(Result);
        }
    }
    if (Session.IsValid()) Session->PendingOperations.Remove(this);
    SetReadyToDestroy();
}

void USimurghRTCOperation::Cancel()
{
    check(IsInGameThread());
    Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::Cancelled, TEXT("Operation cancelled.")));
}

void USimurghRTCOperation::Abandon()
{
    Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::Cancelled, TEXT("Session owner or world ended.")), false);
}

void USimurghRTCOperation::BeginDestroy()
{
    Abandon();
    Super::BeginDestroy();
}
