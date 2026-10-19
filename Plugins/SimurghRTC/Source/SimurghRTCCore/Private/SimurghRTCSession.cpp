#include "SimurghRTCSession.h"
#include "SimurghRTCOperation.h"
#include "Engine/World.h"

USimurghRTCSession* USimurghRTCSession::CreateSession(UObject* Owner)
{
    check(IsInGameThread());
    if (!IsValid(Owner)) return nullptr;
    USimurghRTCSession* Session = NewObject<USimurghRTCSession>(Owner);
    Session->SessionId = FGuid::NewGuid();
    Session->LifetimeOwner = Owner;
    Session->Provider = SIMURGHRTC_WITH_DIRECT ? ESimurghRTCProvider::DirectUnreal : ESimurghRTCProvider::Janus;
    Session->WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(Session, &USimurghRTCSession::HandleWorldCleanup);
    return Session;
}

UWorld* USimurghRTCSession::GetWorld() const
{
    return LifetimeOwner.IsValid() ? LifetimeOwner->GetWorld() : nullptr;
}

bool USimurghRTCSession::IsUsable() const
{
    return !bShuttingDown && LifetimeOwner.IsValid();
}

void USimurghRTCSession::SetState(ESimurghRTCSessionState NewState)
{
    if (State != NewState)
    {
        State = NewState;
        if (IsUsable()) OnStateChanged.Broadcast(State);
    }
}

FSimurghRTCCapability USimurghRTCSession::GetCapability(ESimurghRTCProvider RequestedProvider, ESimurghRTCFeature Feature) const
{
    FSimurghRTCCapability Result;
    if (uint8(Feature) > uint8(ESimurghRTCFeature::DeviceCapture)
        || uint8(RequestedProvider) > uint8(ESimurghRTCProvider::Janus))
    {
        Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid capability query."));
    }
    else
    {
        const bool Media = Feature == ESimurghRTCFeature::Audio || Feature == ESimurghRTCFeature::Video
            || Feature == ESimurghRTCFeature::MediaSources;
        const bool Extension = Feature == ESimurghRTCFeature::JanusRooms;
        const bool Compiled = SimurghRTC::IsProviderCompiled(RequestedProvider)
            && (!Media || SIMURGHRTC_WITH_MEDIA || UE_SERVER)
            && (Feature != ESimurghRTCFeature::DeviceCapture || SIMURGHRTC_WITH_MEDIA)
            && (!Extension || (SIMURGHRTC_WITH_JANUS && SIMURGHRTC_WITH_MEDIA));
        Result.Compiled = Compiled ? ESimurghRTCSupport::Available : ESimurghRTCSupport::Unavailable;
        Result.Service = ESimurghRTCSupport::Unavailable;
        if (!Compiled)
            Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::BuildExcluded, TEXT("The provider or feature is excluded by the build profile."));
        else if (Extension && RequestedProvider != ESimurghRTCProvider::Janus)
            Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::ServiceUnsupported, TEXT("Janus room extensions require the Janus provider."));
        else
            Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Native transport and service support are not implemented in M0."));
    }
    Result.Reason.SessionId = SessionId;
    return Result;
}

FSimurghRTCError USimurghRTCSession::ChannelUnavailable(FSimurghRTCChannelHandle Channel) const
{
    auto Error = SimurghRTC::MakeError(Channel.IsValid() ? ESimurghRTCErrorCode::InvalidState : ESimurghRTCErrorCode::InvalidArgument,
        TEXT("A valid, open channel belonging to this session is required."));
    Error.SessionId = SessionId;
    return Error;
}

FSimurghRTCError USimurghRTCSession::GetChannelInfo(FSimurghRTCChannelHandle Channel, FSimurghRTCChannelInfo& Info) const
{
    check(IsInGameThread());
    Info = {};
    return ChannelUnavailable(Channel);
}

FSimurghRTCError USimurghRTCSession::SendText(FSimurghRTCChannelHandle Channel, const FString& Text)
{
    check(IsInGameThread());
    return ChannelUnavailable(Channel);
}

FSimurghRTCError USimurghRTCSession::SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes)
{
    check(IsInGameThread());
    return ChannelUnavailable(Channel);
}

FSimurghRTCError USimurghRTCSession::PushAudioFrame(FSimurghRTCSourceHandle Source, const FSimurghRTCAudioFrame& Frame)
{
    check(IsInGameThread());
    auto Error = SimurghRTC::ValidateAudioFrame(Frame);
    if (!Source.IsValid()) Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("A source handle is required."));
    if (!Error.IsError() && !SIMURGHRTC_WITH_MEDIA && !UE_SERVER)
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::BuildExcluded, TEXT("Media is excluded by this client profile."));
    if (!Error.IsError()) Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("An active audio source belonging to this session is required."));
    Error.SessionId = SessionId;
    return Error;
}

FSimurghRTCError USimurghRTCSession::PushVideoFrame(FSimurghRTCSourceHandle Source, const FSimurghRTCVideoFrame& Frame)
{
    check(IsInGameThread());
    auto Error = SimurghRTC::ValidateVideoFrame(Frame);
    if (!Source.IsValid()) Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("A source handle is required."));
    if (!Error.IsError() && !SIMURGHRTC_WITH_MEDIA && !UE_SERVER)
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::BuildExcluded, TEXT("Media is excluded by this client profile."));
    if (!Error.IsError()) Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("An active video source belonging to this session is required."));
    Error.SessionId = SessionId;
    return Error;
}

void USimurghRTCSession::HandleWorldCleanup(UWorld* World, bool, bool)
{
    if (GetWorld() == World) Shutdown();
}

void USimurghRTCSession::Shutdown()
{
    check(IsInGameThread());
    if (bShuttingDown) return;
    bShuttingDown = true;
    FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
    WorldCleanupHandle.Reset();
    // Abandon modifies PendingOperations; take a snapshot first.
    const auto Pending = PendingOperations;
    for (USimurghRTCOperation* Operation : Pending)
        if (Operation) Operation->Abandon();
    PendingOperations.Reset();
    State = ESimurghRTCSessionState::Closed;
}

void USimurghRTCSession::BeginDestroy()
{
    Shutdown();
    Super::BeginDestroy();
}
