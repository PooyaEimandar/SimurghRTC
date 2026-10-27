#include "SimurghRTCOperation.h"
#include "SimurghRTCSession.h"
#include "SimurghRTCSessionTransport.h"
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
    Operation->ChannelOptions = Options;
    Operation->Initialize(InSession, TEXT("OpenChannel"), ESimurghRTCFeature::DataChannels, Validation);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::CloseChannel(USimurghRTCSession* InSession, FSimurghRTCChannelHandle Channel)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Validation;
    if (!Channel.IsValid()) Validation = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Channel handle is required."));
    Operation->PendingChannel = Channel;
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

USimurghRTCOperation* USimurghRTCOperation::AttachSource(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (!Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("AttachSource"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::ReplaceSource(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, FSimurghRTCSourceHandle Source)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid() || !Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("ReplaceSource"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::SetSourceMuted(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source, bool bMuted)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (!Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("SetSourceMuted"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::StopSource(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (!Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("StopSource"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::DetachTrack(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("DetachTrack"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::CreateGeneratedSource(USimurghRTCSession* InSession, ESimurghRTCTrackKind Kind)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (Kind != ESimurghRTCTrackKind::Audio && Kind != ESimurghRTCTrackKind::Video)
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid generated source kind."));
    Operation->Initialize(InSession, TEXT("CreateGeneratedSource"), Kind == ESimurghRTCTrackKind::Audio ? ESimurghRTCFeature::Audio : ESimurghRTCFeature::Video, Error);
    return Operation;
}

USimurghRTCOperation* USimurghRTCOperation::SetDecodedFrameDelivery(USimurghRTCSession* InSession, FSimurghRTCTrackHandle Track, bool bEnabled)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("A track belonging to this session is required."));
    Operation->Initialize(InSession, TEXT("SetDecodedFrameDelivery"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

void USimurghRTCOperation::Activate()
{
    check(IsInGameThread());
    if (bActivated || bFinished) return;
    bActivated = true;
    Deadline = FPlatformTime::Seconds() + (OperationName == TEXT("Connect") ? double(ConnectConfig.TimeoutMilliseconds)/1000.0
        : Session.IsValid() ? Session->OperationTimeoutSeconds : 30.0);
    if (Session.IsValid() && Session->IsUsable())
    {
        Session->PendingOperations.Add(this);
        if (UWorld* World = Session->GetWorld(); World && World->GetGameInstance())
            RegisterWithGameInstance(Session.Get());
    }
    TStrongObjectPtr<USimurghRTCOperation> Pending(this);
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Pending](float)
    {
        TStrongObjectPtr<USimurghRTCOperation> KeepAlive(Pending.Get());
        if (auto* Self = KeepAlive.Get())
        {
            Self->Execute();
            return !Self->bFinished;
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
    TStrongObjectPtr<USimurghRTCSession> KeepSession(Session.Get());
    if (ValidationError.IsError()) { Finish(ValidationError); return; }
    if (FPlatformTime::Seconds() >= Deadline)
    {
        if (bStarted && OperationName == TEXT("Disconnect"))
        { Session->ReleaseTransport(); Session->SetState(ESimurghRTCSessionState::Closed); }
        Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::Timeout,TEXT("The operation deadline expired."))); return;
    }
    if (OperationName == TEXT("Disconnect"))
    {
        if (!bStarted)
        {
            bStarted = true;
            Session->TransportError = {};
            if (Session->Transport) { Session->Transport->SetHandler({}); Session->Transport->Close(); }
            Session->SetState(ESimurghRTCSessionState::Closing);
            if (!Session->IsUsable()) return;
            TArray<TWeakObjectPtr<USimurghRTCOperation>> Pending;
            for (USimurghRTCOperation* Other : Session->PendingOperations) Pending.Add(Other);
            for (const auto& Other : Pending)
                if (Other.IsValid() && Other.Get() != this) Other->Cancel();
            if (!Session->IsUsable()) return;
            if (!Session->Transport) Session->SetState(ESimurghRTCSessionState::Closed);
        }
        if (!Session->IsUsable() || bFinished) return;
        if (!Session->Transport && Session->GetState() == ESimurghRTCSessionState::Closed) Finish(Session->TransportError);
        return;
    }
    if (OperationName == TEXT("Connect"))
    {
        if (!bStarted)
        {
            const auto State = Session->GetState();
            if (State != ESimurghRTCSessionState::Idle && State != ESimurghRTCSessionState::Closed && State != ESimurghRTCSessionState::Failed)
            { Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Disconnect before connecting or changing providers."))); return; }
            bStarted = true; TransportGeneration = Session->TransportGeneration + 1;
            const auto Error = Session->StartTransport(ConnectConfig); ConnectConfig = {};
            if (bFinished || !Session->IsUsable()) return;
            if (Error.IsError()) { Finish(Error); return; }
        }
        if (!Session->Transport || TransportGeneration != Session->TransportGeneration)
        { Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState,TEXT("The connection attempt was replaced."))); return; }
        const auto Snapshot = Session->Transport->Snapshot();
        if (Snapshot.State == ESimurghRTCSessionState::Connected)
        { Session->SetState(ESimurghRTCSessionState::Connected); if (!bFinished && Session->IsUsable()) Finish({}); }
        else if (Snapshot.State == ESimurghRTCSessionState::Failed)
            Finish(Snapshot.Error.IsError() ? Snapshot.Error : SimurghRTC::MakeError(ESimurghRTCErrorCode::InternalError,TEXT("Connection failed.")));
        return;
    }
    const auto Capability = Session->GetCapability(Session->GetProvider(), RequiredFeature);
    if (Capability.Reason.Code == ESimurghRTCErrorCode::BuildExcluded || Capability.Reason.Code == ESimurghRTCErrorCode::ServiceUnsupported)
    {
        Finish(Capability.Reason);
        return;
    }
    if (OperationName != TEXT("CreateGeneratedSource") && OperationName != TEXT("CreateSource")
        && Session->GetState() != ESimurghRTCSessionState::Connected)
    {
        Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("This operation requires a connected session.")));
        return;
    }
    if ((OperationName == TEXT("OpenChannel") || OperationName == TEXT("CloseChannel")) && Session->Transport)
    {
        const bool Opening = OperationName == TEXT("OpenChannel");
        if (!bStarted)
        {
            bStarted = true; TransportGeneration = Session->TransportGeneration;
            const auto Error = Opening ? Session->Transport->OpenChannel(ChannelOptions,PendingChannel) : Session->Transport->CloseChannel(PendingChannel);
            if (Error.IsError()) { Finish(Error); return; }
        }
        if (TransportGeneration != Session->TransportGeneration)
        { Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState,TEXT("The channel belongs to an earlier connection."))); return; }
        SimurghRTC::FSessionChannelSnapshot Channel;
        const auto Error = Session->Transport->Channel(PendingChannel,Channel);
        if (Error.IsError()) { Finish(Error); return; }
        if (Opening ? Channel.State == SimurghRTC::ESessionChannelState::Open && Channel.Info.MaximumMessageBytes > 0
                    : Channel.State == SimurghRTC::ESessionChannelState::Closed)
        { Result.Channel = PendingChannel; Finish({}); }
        else if (Opening && (Channel.State == SimurghRTC::ESessionChannelState::Closing || Channel.State == SimurghRTC::ESessionChannelState::Closed))
            Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState,TEXT("The channel closed before opening.")));
        return;
    }
    Finish(SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("This operation is not implemented.")));
}

void USimurghRTCOperation::Finish(FSimurghRTCError Error, bool bBroadcast)
{
    if (bFinished) return;
    // Listeners may release the last application reference or collect garbage.
    TStrongObjectPtr<USimurghRTCOperation> KeepAlive(bBroadcast ? this : nullptr);
    TStrongObjectPtr<USimurghRTCSession> KeepSession(bBroadcast ? Session.Get() : nullptr);
    bFinished = true;
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
    Error.SessionId = SessionId;
    Error.OperationId = OperationId;
    Result.Error = MoveTemp(Error);
    // Credentials are needed only during a connect attempt; never retain them in results/logs.
    ConnectConfig = {};
    if (Result.Error.IsError())
    {
        Result.Channel = {};
        if (bStarted && Session.IsValid() && Session->IsUsable() && TransportGeneration == Session->TransportGeneration)
        {
            if (OperationName == TEXT("Connect") && Session->GetState() != ESimurghRTCSessionState::Closing)
            {
                Session->TransportError = Result.Error; Session->ReleaseTransport();
                Session->SetState(Result.Error.Code == ESimurghRTCErrorCode::Cancelled ? ESimurghRTCSessionState::Idle : ESimurghRTCSessionState::Failed);
            }
            else if (OperationName == TEXT("OpenChannel") && PendingChannel.IsValid() && Session->Transport)
                Session->Transport->CloseChannel(PendingChannel);
        }
    }
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
