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
    FString Id, Principal, CreateId, CreateDigest, CreateResponse;
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
        check(IsInGameThread()); Snapshot.bRunning = false; Config.OnChannelEvent = {};
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
    bool Tick(float)
    {
        check(IsInGameThread());
        if (!Owner.IsValid()) Stop();
        const double Now = FPlatformTime::Seconds();
        TArray<FString> Remove;
        Snapshot.LivePeers = 0;
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
            if (Session->State == TEXT("closed") && !Session->Peer && (!Snapshot.bRunning || Now >= Session->RetainUntil)) Remove.Add(Entry.Key);
        }
        for (const auto& Id : Remove) Sessions.Remove(Id);
        Snapshot.Records = Sessions.Num(); return true;
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
            int32 Live = 0; for (const auto& Entry : Sessions) if (Entry.Value->Peer) ++Live;
            if (Live >= Config.MaxSessions || Sessions.Num() >= 64) { Error(Request, TEXT("ResourceLimit"), Complete); return true; }
            Session = MakeShared<FDirectSession>(); Session->Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            Session->Principal = Allowed.Principal; Session->CreateId = Request.Id; Session->CreateDigest = Request.Digest;
            Session->Peer = FNativePeer::Open(Owner.Get(), Admission.Purpose == TEXT("call") ? ENativeFactoryMode::BuiltinMedia : ENativeFactoryMode::DataOnly, {}, Failure, Config.bLoopbackProbe);
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
FDirectServerSnapshot FDirectServer::GetSnapshot() const { check(IsInGameThread()); return Impl->Snapshot; }
FSimurghRTCError FDirectServer::OpenChannel(const FString& SessionId, const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out)
{
    check(IsInGameThread());
    Out = {};
    const auto Session = Impl->Sessions.FindRef(SessionId);
    const auto Peer = Impl->Snapshot.bRunning && Impl->Owner.IsValid() && Session && Session->State != TEXT("closed") && Session->State != TEXT("failed") ? Session->Peer : nullptr;
    return Peer ? Peer->OpenChannel(Options, Out) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::CloseChannel(const FString& SessionId, FSimurghRTCChannelHandle Channel)
{
    check(IsInGameThread());
    const auto Session = Impl->Sessions.FindRef(SessionId);
    const auto Peer = Impl->Snapshot.bRunning && Impl->Owner.IsValid() && Session && Session->State != TEXT("closed") && Session->State != TEXT("failed") ? Session->Peer : nullptr;
    return Peer ? Peer->CloseChannel(Channel) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::GetChannelInfo(const FString& SessionId, FSimurghRTCChannelHandle Channel, FNativeChannelSnapshot& Out) const
{
    check(IsInGameThread());
    Out = {};
    const auto Session = Impl->Sessions.FindRef(SessionId);
    const auto Peer = Impl->Snapshot.bRunning && Impl->Owner.IsValid() && Session && Session->State != TEXT("closed") && Session->State != TEXT("failed") ? Session->Peer : nullptr;
    return Peer ? Peer->GetChannelInfo(Channel, Out) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::SendText(const FString& SessionId, FSimurghRTCChannelHandle Channel, const FString& Text)
{
    check(IsInGameThread());
    const auto Session = Impl->Sessions.FindRef(SessionId);
    const auto Peer = Impl->Snapshot.bRunning && Impl->Owner.IsValid() && Session && Session->State != TEXT("closed") && Session->State != TEXT("failed") ? Session->Peer : nullptr;
    return Peer ? Peer->SendText(Channel, Text) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectServer::SendBytes(const FString& SessionId, FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes)
{
    check(IsInGameThread());
    const auto Session = Impl->Sessions.FindRef(SessionId);
    const auto Peer = Impl->Snapshot.bRunning && Impl->Owner.IsValid() && Session && Session->State != TEXT("closed") && Session->State != TEXT("failed") ? Session->Peer : nullptr;
    return Peer ? Peer->SendBytes(Channel, Bytes) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
TSharedPtr<FDirectServer> FDirectServer::Bind(UObject* Owner, TSharedRef<IHttpRouter> Router, FDirectServerConfig Config, FString& Error)
{
    check(IsInGameThread()); Error.Reset();
    if (bServersStopping || !IsValid(Owner) || !Config.Admit || Config.MaxSessions < 1 || Config.MaxSessions > 32
        || !FMath::IsFinite(Config.IdleSeconds) || Config.IdleSeconds < 1 || Config.IdleSeconds > 300
        || !FMath::IsFinite(Config.ReplaySeconds) || Config.ReplaySeconds < 1 || Config.ReplaySeconds > 300)
    { Error = TEXT("Invalid Direct Server configuration."); return {}; }
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
