#include "SimurghRTCSession.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCSessionTransport.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

namespace { TArray<TWeakObjectPtr<USimurghRTCSession>> SharedSessions; }
void SimurghRTC::ShutdownSharedSessions()
{
    const auto Sessions = MoveTemp(SharedSessions); SharedSessions.Reset();
    for (const auto& Session : Sessions) if (Session.IsValid()) Session->Shutdown();
}

USimurghRTCSession* USimurghRTCSession::CreateSession(UObject* Owner)
{
    check(IsInGameThread());
    if (!IsValid(Owner)) return nullptr;
    USimurghRTCSession* Session = NewObject<USimurghRTCSession>(Owner);
    Session->SessionId = FGuid::NewGuid();
    Session->LifetimeOwner = Owner;
    Session->Provider = SIMURGHRTC_WITH_DIRECT ? ESimurghRTCProvider::DirectUnreal : ESimurghRTCProvider::Janus;
    Session->WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(Session, &USimurghRTCSession::HandleWorldCleanup);
    SharedSessions.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); }); SharedSessions.Add(Session);
    TWeakObjectPtr<USimurghRTCSession> Weak(Session);
    Session->TransportTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float Delta)
    {
        TStrongObjectPtr<USimurghRTCSession> Self(Weak.Get());
        return Self && Self->TickTransport(Delta);
    }));
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
            && (!Extension || SIMURGHRTC_WITH_JANUS_EXTENSIONS);
        Result.Compiled = Compiled ? ESimurghRTCSupport::Available : ESimurghRTCSupport::Unavailable;
        Result.Service = ESimurghRTCSupport::Unavailable;
        if (!Compiled)
            Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::BuildExcluded, TEXT("The provider or feature is excluded by the build profile."));
        else if (Extension && RequestedProvider != ESimurghRTCProvider::Janus)
            Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::ServiceUnsupported, TEXT("Janus room extensions require the Janus provider."));
        else if (Feature == ESimurghRTCFeature::DataChannels && SimurghRTC::HasSessionTransport(RequestedProvider))
        {
            Result.Service = ESimurghRTCSupport::Available;
            if (IsUsable() && Provider == RequestedProvider && State == ESimurghRTCSessionState::Connected)
            { Result.Platform = Result.Negotiated = ESimurghRTCSupport::Available; }
            else Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("Connect the session before using channels."));
        }
        else Result.Reason = SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("This provider feature is not implemented."));
    }
    Result.Reason.SessionId = SessionId;
    return Result;
}

FSimurghRTCError USimurghRTCSession::GetConnectionError() const
{
    check(IsInGameThread());
    auto Error = TransportError; Error.SessionId = SessionId; return Error;
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
    if (!IsUsable() || !Transport || State != ESimurghRTCSessionState::Connected || !Channel.IsValid()) return ChannelUnavailable(Channel);
    SimurghRTC::FSessionChannelSnapshot Snapshot;
    auto Error = Transport->Channel(Channel,Snapshot); Info = Snapshot.Info; Error.SessionId = SessionId; return Error;
}

FSimurghRTCError USimurghRTCSession::SendText(FSimurghRTCChannelHandle Channel, const FString& Text)
{
    check(IsInGameThread());
    if (!IsUsable() || !Transport || State != ESimurghRTCSessionState::Connected || !Channel.IsValid()) return ChannelUnavailable(Channel);
    auto Error = Transport->SendText(Channel,Text); Error.SessionId = SessionId; return Error;
}

FSimurghRTCError USimurghRTCSession::SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes)
{
    check(IsInGameThread());
    if (!IsUsable() || !Transport || State != ESimurghRTCSessionState::Connected || !Channel.IsValid()) return ChannelUnavailable(Channel);
    auto Error = Transport->SendBytes(Channel,Bytes); Error.SessionId = SessionId; return Error;
}

FSimurghRTCError USimurghRTCSession::StartTransport(const FSimurghRTCSessionConfig& Config)
{
    FSimurghRTCError Error;
    auto Started = SimurghRTC::CreateSessionTransport(this,Config,Error);
    if (!Started)
    {
        if (Error.Code != ESimurghRTCErrorCode::InvalidArgument)
        { Provider = Config.Provider; TransportError = Error; SetState(ESimurghRTCSessionState::Failed); }
        return Error;
    }
    ReleaseTransport(); Transport = MoveTemp(Started); TransportError = {}; Provider = Config.Provider;
    OperationTimeoutSeconds = double(Config.TimeoutMilliseconds) / 1000.0;
    const uint64 Generation = TransportGeneration;
    TWeakObjectPtr<USimurghRTCSession> Weak(this);
    Transport->SetHandler([Weak,Generation](const SimurghRTC::FSessionTransportEvent& Event)
    {
        TStrongObjectPtr<USimurghRTCSession> Self(Weak.Get());
        if (!Self || !Self->IsUsable() || Self->TransportGeneration != Generation || Self->State != ESimurghRTCSessionState::Connected) return;
        if (Event.bMessage) Self->OnMessage.Broadcast(Event.Message);
        else Self->OnChannelChanged.Broadcast(Event.Channel);
    });
    SetState(ESimurghRTCSessionState::Connecting); return {};
}
void USimurghRTCSession::ReleaseTransport()
{
    ++TransportGeneration;
    SimurghRTC::RetireSessionTransport(MoveTemp(Transport)); Transport.Reset();
}
bool USimurghRTCSession::TickTransport(float)
{
    if (!IsUsable()) { Shutdown(); return false; }
    if (Transport)
    {
        const auto Snapshot = Transport->Snapshot();
        if (State == ESimurghRTCSessionState::Connected && Snapshot.State == ESimurghRTCSessionState::Failed)
        { TransportError = Snapshot.Error; ReleaseTransport(); SetState(ESimurghRTCSessionState::Failed); }
        else if (State == ESimurghRTCSessionState::Closing && Snapshot.bNativeClosed
            && (Snapshot.State == ESimurghRTCSessionState::Closed || Snapshot.State == ESimurghRTCSessionState::Failed))
        { TransportError = Snapshot.Error; ReleaseTransport(); SetState(ESimurghRTCSessionState::Closed); }
    }
    return !bShuttingDown;
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
    FTSTicker::GetCoreTicker().RemoveTicker(TransportTicker); TransportTicker.Reset();
    ReleaseTransport();
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
