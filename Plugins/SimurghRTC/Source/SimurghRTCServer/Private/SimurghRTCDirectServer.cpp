// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCDirectServer.h"
#include "SimurghRTCDirectProtocol.h"
#include "SimurghRTCNativePeer.h"
#include "Containers/Ticker.h"
#include "HttpPath.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/Guid.h"
#include "UObject/WeakObjectPtr.h"

namespace SimurghRTC
{
namespace
{
struct IServerShutdown { virtual ~IServerShutdown() = default; virtual void Stop() = 0; };
TArray<TWeakPtr<IServerShutdown>> Servers;
bool bServersStopping = false;
struct FReplay { FString Digest, Response; double Until = 0; int32 Bytes = 0; };
struct FDirectSession
{
    FString Id, Principal, Identity, Purpose, CreateId, CreateDigest, CreateResponse;
    int32 LastReportedState = -1; bool bReportedNativeClosed = false;
    FString State = TEXT("open");
    FNativePeer::FHandle Peer;
    TArray<FNativeSignalRequest> Commands;
    TArray<TSharedPtr<FJsonValue>> Events;
    TMap<FString, FReplay> Replay;
    int32 NextSequence = 1, PendingIce = 0, AppliedIce = 0, ReplayBytes = 0;
    bool bIceComplete = false, bConnected = false;
    double LastActivity = 0, RetainUntil = 0;
};
FString Header(const FHttpServerRequest& Request, const TCHAR* Name)
{
    FString Value; int32 Count = 0;
    for (const auto& Field : Request.Headers) if (Field.Key.Equals(Name, ESearchCase::IgnoreCase))
        for (const auto& Item : Field.Value) { Value = Item; ++Count; }
    return Count == 1 ? Value : FString();
}
void Reply(const FHttpResultCallback& Complete, const FString& Body, int32 Code)
{
    auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
    Response->Code = static_cast<EHttpServerResponseCodes>(Code);
    Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
    Complete(MoveTemp(Response));
}
}
void ShutdownDirectServers()
{
    check(IsInGameThread()); bServersStopping = true;
    for (auto& Weak : Servers) if (auto Server = Weak.Pin()) Server->Stop();
    Servers.Reset();
}
class FDirectServer::FImpl : public IServerShutdown, public TSharedFromThis<FImpl>
{
public:
    TWeakObjectPtr<UObject> Owner;
    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle Route;
    FDirectServerConfig Config;
    FDirectServerSnapshot Snapshot;
    TMap<FString, TSharedPtr<FDirectSession>> Sessions;
    FTSTicker::FDelegateHandle Ticker;
    ~FImpl() { Stop(); FTSTicker::GetCoreTicker().RemoveTicker(Ticker); }
    void Stop() override
    {
        check(IsInGameThread()); Snapshot.bRunning = false; Config.OnChannelEvent = {}; Config.OnSessionChanged = {}; Config.CanRoute = {};
        if (Router && Route) Router->UnbindRoute(Route);
        Route.Reset(); Router.Reset();
        for (auto& Entry : Sessions) Close(*Entry.Value);
    }
    void Close(FDirectSession& Session)
    {
        if (Session.State == TEXT("closed")) return;
        Session.State = TEXT("closed"); ++Snapshot.Closed;
        Snapshot.ClosedSessionIds.Add(Session.Id);
        if (Snapshot.ClosedSessionIds.Num() > 64) Snapshot.ClosedSessionIds.RemoveAt(0);
        Session.RetainUntil = FPlatformTime::Seconds() + Config.ReplaySeconds;
        Session.Commands.Reset(); Session.Events.Reset();
        if (Session.Peer) Session.Peer->Close();
    }
    void Error(const Direct::FRequest& Request, const FString& Code, const FHttpResultCallback& Complete)
    {
        ++Snapshot.Rejected;
        auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("code"), Code);
        Reply(Complete, Direct::Write(Direct::Envelope(Request, Request.Session, TEXT("error"), Body)), Direct::HttpStatus(Code));
    }
    void Event(FDirectSession& Session, const TCHAR* Type, TSharedRef<FJsonObject> Body)
    {
        if (Session.State == TEXT("closed") || Session.State == TEXT("failed")) return;
        if (Session.Events.Num() >= Direct::MaxEvents - 1)
        { Fail(Session, TEXT("ResourceLimit")); return; }
        auto Value = MakeShared<FJsonObject>(); Value->SetNumberField(TEXT("event_id"), Session.Events.Num() + 1);
        Value->SetStringField(TEXT("type"), Type); Value->SetObjectField(TEXT("body"), Body);
        Session.Events.Add(MakeShared<FJsonValueObject>(Value));
    }
    void Fail(FDirectSession& Session, const FString& Code)
    {
        if (Session.State == TEXT("closed") || Session.State == TEXT("failed")) return;
        auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("code"), Code);
        auto Value = MakeShared<FJsonObject>(); Value->SetNumberField(TEXT("event_id"), Session.Events.Num() + 1);
        Value->SetStringField(TEXT("type"), TEXT("error")); Value->SetObjectField(TEXT("body"), Body);
        Session.Events.Add(MakeShared<FJsonValueObject>(Value)); Session.State = TEXT("failed");
        Session.Commands.Reset(); if (Session.Peer) Session.Peer->Close();
    }
    FDirectSessionInfo Info(const FDirectSession& S) const
    {
        FDirectSessionInfo I; I.SessionId = S.Id; I.Principal = S.Principal; I.Identity = S.Identity; I.Purpose = S.Purpose;
        I.bNativeClosed = !S.Peer || S.Peer->GetSnapshot().State == ENativePeerState::Closed;
        I.State = S.State == TEXT("closed") ? (I.bNativeClosed ? EDirectServerSessionState::Closed : EDirectServerSessionState::Closing)
            : S.State == TEXT("failed") ? EDirectServerSessionState::Failed
            : S.bConnected ? EDirectServerSessionState::Connected : EDirectServerSessionState::Connecting;
        return I;
    }
    FNativePeer::FHandle DataPeer(const FString& Id) const
    {
        const auto S = Sessions.FindRef(Id);
        if (!Snapshot.bRunning || !Owner.IsValid() || !S || S->State == TEXT("closed") || S->State == TEXT("failed")) return {};
        return S->Peer;
    }
    bool Tick(float)
    {
        check(IsInGameThread());
        if (!Owner.IsValid()) Stop();
        const double Now = FPlatformTime::Seconds();
        TArray<FString> Remove;
        Snapshot.LivePeers = 0; Snapshot.ConnectedSessions = 0;
        TArray<FDirectSessionInfo> Notifications;
        for (const auto& Entry : Sessions)
        {
            auto Session = Entry.Value;
            for (auto It = Session->Replay.CreateIterator(); It; ++It)
                if (It.Value().Until <= Now) { Session->ReplayBytes -= It.Value().Bytes; It.RemoveCurrent(); }
            if (Session->State != TEXT("closed") && Now - Session->LastActivity >= Config.IdleSeconds)
            { ++Snapshot.Expired; Close(*Session); }
            if (Session->Peer)
            {
                const auto State = Session->Peer->GetSnapshot();
                if (State.State == ENativePeerState::Closed) Session->Peer.Reset();
                else
                {
                    ++Snapshot.LivePeers;
                    if (Session->State != TEXT("closed") && Session->State != TEXT("failed"))
                    {
                        if (State.State == ENativePeerState::Failed) Fail(*Session, TEXT("NativeError"));
                        else if (State.State == ENativePeerState::Ready)
                        {
                            if (!Session->bConnected && State.Transport.Connection == ENativeConnectionState::Connected
                                && State.Transport.bDataTransportOpen && State.Transport.DtlsState == 3)
                            {
                                Session->bConnected = true; ++Snapshot.Connected; Snapshot.LastConnectedTransport = State.Transport;
                                Snapshot.ConnectedSessionIds.Add(Session->Id);
                                if (Snapshot.ConnectedSessionIds.Num() > 64) Snapshot.ConnectedSessionIds.RemoveAt(0);
                                auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("state"), TEXT("connected"));
                                Event(*Session, TEXT("connection"), Body);
                            }
                            if (Session->bConnected && State.Transport.Connection == ENativeConnectionState::Connected) ++Snapshot.ConnectedSessions;
                            auto Commands = MoveTemp(Session->Commands); Session->Commands.Reset();
                            for (auto& Command : Commands)
                            {
                                if (Session->State == TEXT("failed")) break;
                                const bool Offer = Command.Action == ENativeSignalAction::SetRemote;
                                FString Failure;
                                TWeakPtr<FImpl> Weak = AsShared(); TWeakPtr<FDirectSession> WeakSession = Session;
                                if (!Session->Peer->Signal(MoveTemp(Command), [Weak, WeakSession, Offer](const FNativeSignalResult& Result)
                                {
                                    const auto Self = Weak.Pin(); const auto S = WeakSession.Pin();
                                    if (!Self || !S || !Self->Snapshot.bRunning || S->State == TEXT("closed") || S->State == TEXT("failed")) return;
                                    if (!Result.IsSuccess()) { Self->Fail(*S, TEXT("NativeError")); return; }
                                    if (!Offer) return;
                                    FNativeSignalRequest Answer; Answer.Action = ENativeSignalAction::CreateAnswer; FString Error;
                                    if (!S->Peer->Signal(MoveTemp(Answer), [Weak, WeakSession](const FNativeSignalResult& R)
                                    {
                                        const auto P = Weak.Pin(); const auto T = WeakSession.Pin();
                                        if (!P || !T || !P->Snapshot.bRunning) return;
                                        if (!R.IsSuccess()) { P->Fail(*T, TEXT("NativeError")); return; }
                                        auto Body = MakeShared<FJsonObject>(); Body->SetNumberField(TEXT("revision"), 1); Body->SetStringField(TEXT("sdp"), R.Sdp);
                                        P->Event(*T, TEXT("answer"), Body);
                                    }, Error)) Self->Fail(*S, TEXT("NativeError"));
                                }, Failure)) Fail(*Session, TEXT("NativeError"));
                            }
                        }
                    }
                }
            }
            const auto Current = Info(*Session);
            if (Session->LastReportedState != int32(Current.State) || Session->bReportedNativeClosed != Current.bNativeClosed)
            {
                Session->LastReportedState = int32(Current.State); Session->bReportedNativeClosed = Current.bNativeClosed;
                Notifications.Add(Current);
            }
            if (Session->State == TEXT("closed") && !Session->Peer && (!Snapshot.bRunning || Now >= Session->RetainUntil)) Remove.Add(Entry.Key);
        }
        for (const auto& Id : Remove) Sessions.Remove(Id);
        Snapshot.Records = Sessions.Num();
        Snapshot.PeakLivePeers = FMath::Max(Snapshot.PeakLivePeers, Snapshot.LivePeers);
        Snapshot.PeakConnectedSessions = FMath::Max(Snapshot.PeakConnectedSessions, Snapshot.ConnectedSessions);
        // Dispatch after iterating the map: callbacks may close any session or stop the host.
        for (const auto& Current : Notifications)
        {
            if (!Snapshot.bRunning || !Owner.IsValid()) break;
            if (const auto S = Sessions.FindRef(Current.SessionId))
            {
                const auto NowInfo = Info(*S);
                if (NowInfo.State != Current.State || NowInfo.bNativeClosed != Current.bNativeClosed) continue;
            }
            const auto Callback = Config.OnSessionChanged;
            if (Callback) Callback(Current);
        }
        return true;
    }
    bool Handle(const FHttpServerRequest& Http, const FHttpResultCallback& Complete)
    {
        check(IsInGameThread()); Direct::FRequest Request; FString Failure;
        if (!Snapshot.bRunning || !Owner.IsValid()) { Error({}, TEXT("Unavailable"), Complete); return true; }
        if (!Http.QueryParams.IsEmpty() || Http.FullPath.GetPath() != Direct::Path
            || !Header(Http, TEXT("Content-Type")).Equals(TEXT("application/json"), ESearchCase::IgnoreCase))
        { Error({}, TEXT("InvalidArgument"), Complete); return true; }
        if (!Direct::ParseRequest(Http.Body, Request, Failure)) { Error({}, Failure, Complete); return true; }
        const FString Authorization = Header(Http, TEXT("Authorization"));
        if (!Authorization.StartsWith(TEXT("Bearer "), ESearchCase::CaseSensitive) || Authorization.Len() <= 7 || Authorization.Len() > 2055)
        { Error(Request, TEXT("Unauthorized"), Complete); return true; }
        FDirectAdmissionRequest Admission; Admission.BearerToken = Authorization.Mid(7); Admission.bCreate = Request.Type == TEXT("create");
        if (Admission.bCreate) { Admission.Identity = Request.Body->GetStringField(TEXT("identity")); Admission.Purpose = Request.Body->GetStringField(TEXT("purpose")); }
        const auto Allowed = Config.Admit(Admission);
        if (!Snapshot.bRunning || !Owner.IsValid()) { Error(Request, TEXT("Unavailable"), Complete); return true; }
        if (Allowed.Principal.IsEmpty() || Direct::Bytes(Allowed.Principal).Num() > 128)
        { Error(Request, TEXT("Unauthorized"), Complete); return true; }
        TSharedPtr<FDirectSession> Session;
        if (Admission.bCreate)
        {
            for (const auto& Entry : Sessions) if (Entry.Value->Principal == Allowed.Principal && Entry.Value->CreateId == Request.Id) { Session = Entry.Value; break; }
            if (Session)
            {
                if (Session->CreateDigest != Request.Digest) Error(Request, TEXT("RequestConflict"), Complete);
                else { ++Snapshot.Replays; Reply(Complete, Session->CreateResponse, 200); }
                return true;
            }
            if (!Allowed.bAllowCreate) { Error(Request, TEXT("AdmissionRejected"), Complete); return true; }
            if (Request.Sequence != 0) { Error(Request, TEXT("OutOfOrder"), Complete); return true; }
            int32 Live = 0, PrincipalLive = 0;
            for (const auto& Entry : Sessions) if (Entry.Value->Peer)
            { ++Live; if (Entry.Value->Principal == Allowed.Principal) ++PrincipalLive; }
            if (Live >= Config.MaxSessions || PrincipalLive >= Config.MaxSessionsPerPrincipal || Sessions.Num() >= 64) { Error(Request, TEXT("ResourceLimit"), Complete); return true; }
            Session = MakeShared<FDirectSession>(); Session->Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            Session->Principal = Allowed.Principal; Session->Identity = Admission.Identity; Session->Purpose = Admission.Purpose; Session->CreateId = Request.Id; Session->CreateDigest = Request.Digest;
            Session->Peer = FNativePeer::Open(Owner.Get(), Admission.Purpose == TEXT("call") ? ENativeFactoryMode::BuiltinMedia : ENativeFactoryMode::DataOnly, {}, Failure, Config.bLoopbackProbe, {Config.IceServers, Config.bRelayOnly}, Admission.Purpose == TEXT("call"));
            if (!Session->Peer) { Error(Request, TEXT("ResourceLimit"), Complete); return true; }
            TWeakPtr<FImpl> Weak = AsShared(); TWeakPtr<FDirectSession> WeakSession = Session;
            Session->Peer->SetChannelHandler([Weak, WeakSession](const FNativeChannelEvent& Event)
            {
                const auto Self = Weak.Pin(); const auto S = WeakSession.Pin();
                if (!Self || !S || !Self->Owner.IsValid() || !Self->Snapshot.bRunning || (S->State == TEXT("closed") || S->State == TEXT("failed"))) return;
                const auto Callback = Self->Config.OnChannelEvent;
                if (Callback) Callback(S->Id, Event);
            });
            Session->Peer->SetEventHandler([Weak, WeakSession](const FNativeTransportEvent& Native)
            {
                const auto Self = Weak.Pin(); const auto S = WeakSession.Pin();
                if (!Self || !S || !Self->Snapshot.bRunning) return;
                if (Native.Type == ENativeSignalEvent::FatalError) Self->Fail(*S, TEXT("NativeError"));
                else if (Native.Type == ENativeSignalEvent::LocalIce) Self->Event(*S, TEXT("ice"), Direct::IceBody(Native.Ice));
                else if (Native.Type == ENativeSignalEvent::LocalIceComplete)
                { auto Body = MakeShared<FJsonObject>(); Body->SetNumberField(TEXT("revision"), 1); Body->SetBoolField(TEXT("completed"), true); Self->Event(*S, TEXT("ice"), Body); }
                else if (Native.Type == ENativeSignalEvent::ConnectionChanged && Native.Transport.Connection == ENativeConnectionState::Disconnected)
                { auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("state"), TEXT("disconnected")); Self->Event(*S, TEXT("connection"), Body); }
            });
            auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("state"), TEXT("open"));
            Session->CreateResponse = Direct::Write(Direct::Envelope(Request, Session->Id, TEXT("accepted"), Body));
            Session->LastActivity = FPlatformTime::Seconds(); Sessions.Add(Session->Id, Session); ++Snapshot.Created;
            Reply(Complete, Session->CreateResponse, 200); return true;
        }
        Session = Sessions.FindRef(Request.Session);
        if (!Session || Session->Principal != Allowed.Principal) { Error(Request, TEXT("SessionNotFound"), Complete); return true; }
        const double Now = FPlatformTime::Seconds();
        if (const auto* Prior = Session->Replay.Find(Request.Id); Prior && Prior->Until > Now)
        {
            if (Prior->Digest != Request.Digest) Error(Request, TEXT("RequestConflict"), Complete);
            else { ++Snapshot.Replays; Reply(Complete, Prior->Response, 200); }
            return true;
        }
        if (Request.Id == Session->CreateId) { Error(Request, TEXT("RequestConflict"), Complete); return true; }
        if (Session->State == TEXT("closed")) { Error(Request, TEXT("SessionClosed"), Complete); return true; }
        const bool Closing = Request.Type == TEXT("close");
        if (!Closing)
        {
            if (Session->State == TEXT("closed")) { Error(Request, TEXT("SessionClosed"), Complete); return true; }
            if (Request.Sequence != Session->NextSequence) { Error(Request, TEXT("OutOfOrder"), Complete); return true; }
            if (Session->Replay.Num() >= Direct::MaxReplay || Session->ReplayBytes + Direct::MaxMessageBytes > Direct::MaxReplayBytes)
            { Error(Request, TEXT("ResourceLimit"), Complete); return true; }
        }
        auto Body = MakeShared<FJsonObject>();
        if (Closing)
        {
            Close(*Session); Body->SetStringField(TEXT("state"), TEXT("closed"));
            // Cleanup is available even when replay capacity/sequence is exhausted.
            Session->Replay.Reset(); Session->ReplayBytes = 0;
        }
        else if (Request.Type == TEXT("poll"))
        {
            int32 After = 0; Direct::Integer(Request.Body, TEXT("after_event"), After);
            if (After > Session->Events.Num()) { Error(Request, TEXT("InvalidCursor"), Complete); return true; }
            TArray<TSharedPtr<FJsonValue>> Events;
            for (int32 I = After; I < Session->Events.Num(); ++I) Events.Add(Session->Events[I]);
            Body->SetArrayField(TEXT("events"), Events); Body->SetNumberField(TEXT("last_event"), Session->Events.Num());
        }
        else
        {
            if (Session->State == TEXT("failed")) { Error(Request, TEXT("InvalidState"), Complete); return true; }
            if (Request.Type == TEXT("offer"))
            {
                if (Session->State != TEXT("open")) { Error(Request, TEXT("InvalidState"), Complete); return true; }
                Session->State = TEXT("offer-set"); Session->AppliedIce += Session->PendingIce; Session->PendingIce = 0;
                FNativeSignalRequest Offer; Offer.Action = ENativeSignalAction::SetRemote; Offer.SdpType = ENativeSdpType::Offer;
                Offer.Sdp = Request.Body->GetStringField(TEXT("sdp")); Session->Commands.Add(MoveTemp(Offer));
                Body->SetStringField(TEXT("state"), Session->State); Body->SetNumberField(TEXT("applied_ice"), Session->AppliedIce);
            }
            else
            {
                if (Session->bIceComplete) { Error(Request, TEXT("IceComplete"), Complete); return true; }
                if (Session->PendingIce + Session->AppliedIce >= Direct::MaxIce) { Error(Request, TEXT("ResourceLimit"), Complete); return true; }
                FNativeSignalRequest Ice; Direct::Ice(Request.Body, Ice); Session->bIceComplete = Ice.Action == ENativeSignalAction::EndIce;
                Session->Commands.Add(MoveTemp(Ice));
                if (Session->State == TEXT("open")) { ++Session->PendingIce; ++Snapshot.EarlyIce; } else ++Session->AppliedIce;
                Body->SetNumberField(TEXT("pending_ice"), Session->PendingIce); Body->SetNumberField(TEXT("applied_ice"), Session->AppliedIce);
            }
        }
        const FString Response = Direct::Write(Direct::Envelope(Request, Session->Id, TEXT("accepted"), Body));
        const int32 Size = Direct::Bytes(Response).Num();
        check(Size <= Direct::MaxMessageBytes);
        Session->Replay.Add(Request.Id, {Request.Digest, Response, Now + Config.ReplaySeconds, Size}); Session->ReplayBytes += Size;
        if (!Closing) ++Session->NextSequence;
        Session->LastActivity = Now; Reply(Complete, Response, 200); return true;
    }
};
FDirectServer::FDirectServer(TSharedRef<FImpl> InImpl) : Impl(MoveTemp(InImpl)) {}
FDirectServer::~FDirectServer() { Stop(); }
void FDirectServer::Stop() { Impl->Stop(); }
FSimurghRTCError FDirectServer::PushAudio(const FString& SessionId, const FSimurghRTCAudioFrame& Frame)
{
    check(IsInGameThread()); const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->PushAudio(Frame) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::MuteAudio(const FString& SessionId, bool bMuted)
{
    check(IsInGameThread()); const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->MuteAudio(bMuted) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
bool FDirectServer::PollAudio(const FString& SessionId, FSimurghRTCAudioFrame& Out)
{
    check(IsInGameThread()); const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->PollAudio(Out) : false;
}
bool FDirectServer::HasRemoteAudio(const FString& SessionId) const
{
    check(IsInGameThread()); const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->HasRemoteAudio() : false;
}
FDirectServerSnapshot FDirectServer::GetSnapshot() const { check(IsInGameThread()); return Impl->Snapshot; }
FSimurghRTCError FDirectServer::OpenChannel(const FString& SessionId, const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out)
{
    check(IsInGameThread());
    Out = {};
    const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->OpenChannel(Options, Out) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::CloseChannel(const FString& SessionId, FSimurghRTCChannelHandle Channel)
{
    check(IsInGameThread());
    const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->CloseChannel(Channel) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::GetChannelInfo(const FString& SessionId, FSimurghRTCChannelHandle Channel, FNativeChannelSnapshot& Out) const
{
    check(IsInGameThread());
    Out = {};
    const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->GetChannelInfo(Channel, Out) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::SendText(const FString& SessionId, FSimurghRTCChannelHandle Channel, const FString& Text)
{
    check(IsInGameThread());
    const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->SendText(Channel, Text) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::SendBytes(const FString& SessionId, FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes)
{
    check(IsInGameThread());
    const auto Peer = Impl->DataPeer(SessionId);
    return Peer ? Peer->SendBytes(Channel, Bytes) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::GetSessionInfo(const FString& Id, FDirectSessionInfo& Out) const
{
    check(IsInGameThread()); Out = {};
    const auto S = Impl->Sessions.FindRef(Id);
    if (!S) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Unknown Server session."));
    Out = Impl->Info(*S); return {};
}
TArray<FDirectSessionInfo> FDirectServer::GetSessions(bool bIncludeClosed) const
{
    check(IsInGameThread()); TArray<FDirectSessionInfo> Out;
    for (const auto& Pair : Impl->Sessions)
    { auto I = Impl->Info(*Pair.Value); if (bIncludeClosed || !I.bNativeClosed) Out.Add(MoveTemp(I)); }
    Out.Sort([](const auto& A, const auto& B) { return A.SessionId < B.SessionId; }); return Out;
}
FSimurghRTCError FDirectServer::CloseSession(const FString& Id)
{
    check(IsInGameThread()); const auto S = Impl->Sessions.FindRef(Id);
    if (!S) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Unknown Server session."));
    Impl->Close(*S); return {};
}
FSimurghRTCError FDirectServer::EchoMessage(const FString& Id, const FSimurghRTCDataMessage& Message)
{
    TArray<FDirectRouteResult> Results;
    const FDirectChannelAddress Address{Id, Message.Channel};
    auto Error = RouteMessage(Address, Message, {Address}, Results);
    return Error.IsError() ? Error : Results[0].Error;
}
FSimurghRTCError FDirectServer::RouteMessage(const FDirectChannelAddress& Source, const FSimurghRTCDataMessage& Message,
    const TArray<FDirectChannelAddress>& Destinations, TArray<FDirectRouteResult>& OutResults)
{
    check(IsInGameThread()); OutResults.Reset();
    auto Invalid = [] { return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid routed message or destination list.")); };
    if (!Source.Channel.IsValid() || Message.Channel.Id != Source.Channel.Id || Destinations.IsEmpty() || Destinations.Num() > 32
        || Message.Text.Len() > FNativeDataLimits::MessageBytes || Message.Bytes.Num() > FNativeDataLimits::MessageBytes) return Invalid();
    if (Message.Kind == ESimurghRTCMessageKind::Text)
    {
        if (!Message.Bytes.IsEmpty()) return Invalid();
        FTCHARToUTF8 Utf8(*Message.Text, Message.Text.Len()); FUTF8ToTCHAR Back(Utf8.Get(), Utf8.Length());
        if (Utf8.Length() > FNativeDataLimits::MessageBytes || Back.Length() != Message.Text.Len()
            || FMemory::Memcmp(Back.Get(), *Message.Text, Message.Text.Len() * sizeof(TCHAR)) != 0) return Invalid();
    }
    else if (Message.Kind != ESimurghRTCMessageKind::Binary || !Message.Text.IsEmpty()) return Invalid();
    TSet<FString> Seen;
    for (const auto& D : Destinations)
    {
        if (!Direct::SessionId(D.SessionId) || !D.Channel.IsValid()) return Invalid();
        const FString Key = D.SessionId + TEXT(":") + D.Channel.Id.ToString();
        if (Seen.Contains(Key)) return Invalid(); Seen.Add(Key);
    }
    const auto State = Impl; // Policy callbacks may release the public handle or stop the host.
    const auto From = State->Sessions.FindRef(Source.SessionId);
    auto SourcePeer = State->DataPeer(Source.SessionId); FNativeChannelSnapshot SourceChannel;
    if (!From || !SourcePeer || !From->bConnected || SourcePeer->GetChannelInfo(Source.Channel, SourceChannel).IsError() || !SourceChannel.Info.bOpen)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Source session/channel is not connected."));
    const auto FromInfo = State->Info(*From);
    const auto Payload = Message; const auto Targets = Destinations; const auto Origin = Source;
    for (const auto& Target : Targets)
    {
        FDirectRouteResult Result; Result.Destination = Target;
        const auto To = State->Sessions.FindRef(Target.SessionId); auto Peer = State->DataPeer(Target.SessionId);
        FNativeChannelSnapshot Channel;
        SourcePeer = State->DataPeer(Origin.SessionId);
        if (!SourcePeer || SourcePeer->GetChannelInfo(Origin.Channel, SourceChannel).IsError() || !SourceChannel.Info.bOpen
            || !To || !To->bConnected || !Peer || Peer->GetChannelInfo(Target.Channel, Channel).IsError() || !Channel.Info.bOpen)
            Result.Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Destination session/channel is not connected."));
        else
        {
            bool Allowed = Origin.SessionId == Target.SessionId;
            if (!Allowed)
            {
                const auto Policy = State->Config.CanRoute;
                Allowed = Policy && Policy(FromInfo, State->Info(*To), Payload.Kind);
            }
            if (!Allowed) Result.Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::Unauthorized, TEXT("Message route is not authorized."));
            else
            {
                SourcePeer = State->DataPeer(Origin.SessionId); Peer = State->DataPeer(Target.SessionId);
                if (!SourcePeer || !Peer || SourcePeer->GetChannelInfo(Origin.Channel, SourceChannel).IsError() || !SourceChannel.Info.bOpen)
                    Result.Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Route closed during authorization."));
                else Result.Error = Payload.Kind == ESimurghRTCMessageKind::Text ? Peer->SendText(Target.Channel, Payload.Text) : Peer->SendBytes(Target.Channel, Payload.Bytes);
            }
        }
        if (Result.Error.IsError()) ++State->Snapshot.RejectedRecipients; else ++State->Snapshot.RoutedRecipients;
        OutResults.Add(MoveTemp(Result));
    }
    return {};
}
TSharedPtr<FDirectServer> FDirectServer::Bind(UObject* Owner, TSharedRef<IHttpRouter> Router, FDirectServerConfig Config, FString& Error)
{
    check(IsInGameThread()); Error.Reset();
    if (bServersStopping || !IsValid(Owner) || !Config.Admit || Config.MaxSessions < 1 || Config.MaxSessions > 32
        || Config.MaxSessionsPerPrincipal < 1 || Config.MaxSessionsPerPrincipal > 32
        || !FMath::IsFinite(Config.IdleSeconds) || Config.IdleSeconds < 1 || Config.IdleSeconds > 300
        || !FMath::IsFinite(Config.ReplaySeconds) || Config.ReplaySeconds < 1 || Config.ReplaySeconds > 300)
    { Error = TEXT("Invalid Direct Server configuration."); return {}; }
    const auto IceError = ValidateIceServers(Config.IceServers, Config.bRelayOnly);
    if (IceError.IsError()) { Error = IceError.Message; return {}; }
    auto Impl = MakeShared<FImpl>(); Impl->Owner = Owner; Impl->Router = Router; Impl->Config = MoveTemp(Config);
    TWeakPtr<FImpl> Weak = Impl;
    Impl->Route = Router->BindRoute(FHttpPath(Direct::Path), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([Weak](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
        {
            const auto State = Weak.Pin(); if (!State) { Complete(FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail)); return true; }
            return State->Handle(Request, Complete);
        }));
    if (!Impl->Route) { Error = TEXT("Direct signaling route is already bound."); return {}; }
    Impl->Snapshot.bRunning = true;
    Servers.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); }); Servers.Add(Impl);
    Impl->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float Delta)
    { const auto State = Weak.Pin(); return State && State->Tick(Delta); }));
    return MakeShareable(new FDirectServer(Impl));
}
}
