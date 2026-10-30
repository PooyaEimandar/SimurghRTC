// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCDirectClient.h"
#include "SimurghRTCDirectProtocol.h"
#include "SimurghRTCNativePeer.h"
#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "UObject/WeakObjectPtr.h"

namespace SimurghRTC
{
namespace
{
struct IClientShutdown { virtual ~IClientShutdown() = default; virtual void Abort() = 0; };
TArray<TWeakPtr<IClientShutdown>> Clients;
bool bShuttingDown = false;
struct FReceive
{
    FCriticalSection Mutex;
    TArray<uint8> Data;
    bool bOverflow = false;
};
struct FPending { FString Type; TSharedPtr<FJsonObject> Body; };
bool TokenValid(const FString& Token)
{
    if (Token.IsEmpty() || Token.Len() > 2048) return false;
    for (TCHAR C : Token) if (C < 33 || C > 126) return false;
    return true;
}
bool EndpointValid(const FDirectClientConfig& Config)
{
    const auto& Url = Config.Endpoint;
    if (Url.Len() > 2048 || !Url.EndsWith(Direct::Path, ESearchCase::CaseSensitive)
        || Url.Contains(TEXT("@")) || Url.Contains(TEXT("?")) || Url.Contains(TEXT("#"))) return false;
    for (TCHAR C : Url) if (C <= 32 || C >= 127 || C == '\\') return false;
    if (Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
    {
        const FString Authority = Url.Mid(8, Url.Len() - 8 - FCString::Strlen(Direct::Path));
        return !Authority.IsEmpty() && !Authority.Contains(TEXT("/"));
    }
    const FString Prefix = TEXT("http://127.0.0.1:");
    if (!Config.bAllowLoopbackHttp || !Url.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
    const FString Port = Url.Mid(Prefix.Len(), Url.Len() - Prefix.Len() - FCString::Strlen(Direct::Path));
    if (Port.IsEmpty() || Port.Len() > 5) return false;
    for (TCHAR C : Port) if (C < '0' || C > '9') return false;
    return FCString::Atoi(*Port) > 0 && FCString::Atoi(*Port) <= 65535;
}
}
void ShutdownDirectClients()
{
    check(IsInGameThread());
    bShuttingDown = true;
    for (auto& Weak : Clients) if (auto Client = Weak.Pin()) Client->Abort();
    Clients.Reset();
}
class FDirectClient::FImpl : public IClientShutdown, public TSharedFromThis<FImpl>
{
public:
    TWeakObjectPtr<UObject> Owner;
    FDirectClientConfig Config;
    FDirectClientSnapshot Snapshot;
    FDirectClientChanged Changed;
    FNativePeer::FHandle Peer;
    FTSTicker::FDelegateHandle Ticker;
    TArray<FPending> Queue;
    FHttpRequestPtr Http;
    TSharedPtr<FReceive, ESPMode::ThreadSafe> Receive;
    Direct::FRequest Current;
    FString Wire, Offer;
    int32 Sequence = 0, Cursor = 0, Generation = 0, Attempt = 0, NativePending = 0;
    double Started = 0, RetryAt = 0, RequestStarted = 0, NextPoll = 0, ClosingAt = 0;
    bool bOfferStarted = false, bOfferQueued = false, bLocalEnd = false, bEndAcknowledged = false;
    bool bAnswer = false, bServerConnected = false, bNotify = false, bAborted = false;
    ~FImpl() { Abort(); FTSTicker::GetCoreTicker().RemoveTicker(Ticker); }
    void Notify(EDirectClientState State)
    {
        if (Snapshot.State != State) { Snapshot.State = State; bNotify = true; }
    }
    void CancelHttp()
    {
        ++Generation;
        if (Http) { Http->OnProcessRequestComplete().Unbind(); Http->CancelRequest(); Http.Reset(); }
        Receive.Reset(); Wire.Reset(); RetryAt = 0;
    }
    void BestEffortClose()
    {
        if (bShuttingDown || Snapshot.SessionId.IsEmpty() || Snapshot.bRemoteClosed || !FModuleManager::Get().IsModuleLoaded(TEXT("HTTP"))) return;
        Direct::FRequest Q; Q.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits); Q.Sequence = Sequence;
        Q.Type = TEXT("close"); Q.Session = Snapshot.SessionId; Q.Body = MakeShared<FJsonObject>();
        auto Request = FHttpModule::Get().CreateRequest();
        Request->SetURL(Config.Endpoint); Request->SetVerb(TEXT("POST")); Request->SetTimeout(1);
        Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Config.BearerToken);
        Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
        Request->SetContentAsString(RequestWire(Q));
        // Bound the cleanup response too. It owns only copied data, never a UObject/peer.
        const auto Buffer = MakeShared<FReceive, ESPMode::ThreadSafe>();
        if (!Request->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Buffer](void* Data, int64& Length)
        {
            FScopeLock Lock(&Buffer->Mutex);
            if (Length < 0 || Length > Direct::MaxMessageBytes - Buffer->Data.Num()) { Length = 0; return; }
            Buffer->Data.Append(static_cast<uint8*>(Data), int32(Length));
        }))) return;
        Request->ProcessRequest();
    }
    void Abort() override
    {
        check(IsInGameThread()); if (bAborted) return;
        bAborted = true; Changed = {}; CancelHttp(); BestEffortClose(); Queue.Reset();
        if (Peer) Peer->Close(); Peer.Reset();
        FTSTicker::GetCoreTicker().RemoveTicker(Ticker); Ticker.Reset();
    }
    void Fail(const FString& Error)
    {
        if (Snapshot.State == EDirectClientState::Failed || bAborted) return;
        Snapshot.Error = Error; CancelHttp(); Queue.Reset(); BestEffortClose();
        if (Peer) Peer->Close(); Notify(EDirectClientState::Failed);
    }
    void Close()
    {
        check(IsInGameThread());
        if (bAborted || Snapshot.State == EDirectClientState::Closing || Snapshot.State == EDirectClientState::Closed) return;
        CancelHttp(); Queue.Reset(); if (Peer) Peer->Close();
        ClosingAt = FPlatformTime::Seconds(); Notify(EDirectClientState::Closing);
        if (!Snapshot.SessionId.IsEmpty()) Enqueue(TEXT("close"), MakeShared<FJsonObject>());
    }
    void Enqueue(const TCHAR* Type, TSharedRef<FJsonObject> Body)
    {
        if (Queue.Num() >= 40) { Fail(TEXT("ResourceLimit")); return; }
        Queue.Add({Type, Body});
    }
    FString RequestWire(const Direct::FRequest& Request)
    {
        auto Json = MakeShared<FJsonObject>(); Json->SetStringField(TEXT("protocol"), TEXT("simurghrtc.direct"));
        Json->SetNumberField(TEXT("version"), 1); Json->SetStringField(TEXT("type"), Request.Type);
        Json->SetStringField(TEXT("request_id"), Request.Id); Json->SetNumberField(TEXT("sequence"), Request.Sequence);
        Json->SetObjectField(TEXT("body"), Request.Body);
        if (Request.Type != TEXT("create")) Json->SetStringField(TEXT("session_id"), Request.Session);
        return Direct::Write(Json);
    }
    void SendNative(FNativeSignalRequest Request, TFunction<void()> Done = {})
    {
        ++NativePending; FString Error; TWeakPtr<FImpl> Weak = AsShared();
        if (!Peer->Signal(MoveTemp(Request), [Weak, Done = MoveTemp(Done)](const FNativeSignalResult& Result)
        {
            const auto Self = Weak.Pin(); if (!Self || Self->bAborted) return;
            --Self->NativePending;
            if (!Result.IsSuccess()) { Self->Fail(TEXT("NativeError")); return; }
            if (Done) Done();
        }, Error)) { --NativePending; Fail(TEXT("NativeError")); }
    }
    bool Events(const TSharedPtr<FJsonObject>& Body)
    {
        const TArray<TSharedPtr<FJsonValue>>* Events; int32 Last;
        if (!Direct::Fields(Body, {TEXT("events"), TEXT("last_event")}) || !Body->TryGetArrayField(TEXT("events"), Events)
            || Events->Num() > Direct::MaxEvents || !Direct::Integer(Body, TEXT("last_event"), Last) || Last < Cursor || Last > Direct::MaxEvents) return false;
        for (const auto& Event : *Events)
        {
            if (!Event || Event->Type != EJson::Object) return false;
            const auto E = Event->AsObject(); const TSharedPtr<FJsonObject>* B; FString Type; int32 Id;
            if (!Direct::Fields(E, {TEXT("event_id"), TEXT("type"), TEXT("body")}) || !Direct::Integer(E, TEXT("event_id"), Id)
                || Id != Cursor + 1 || !Direct::Text(E, TEXT("type"), Type, 16) || !E->TryGetObjectField(TEXT("body"), B)) return false;
            if (Type == TEXT("answer"))
            {
                int32 Revision; FString Sdp;
                if (bAnswer || !Direct::Fields(*B, {TEXT("revision"), TEXT("sdp")}) || !Direct::Integer(*B, TEXT("revision"), Revision, 1)
                    || Revision != 1 || !Direct::Text(*B, TEXT("sdp"), Sdp, Direct::MaxSdpBytes) || !Direct::Sdp(Sdp)) return false;
                bAnswer = true; FNativeSignalRequest Q; Q.Action = ENativeSignalAction::SetRemote; Q.SdpType = ENativeSdpType::Answer; Q.Sdp = Sdp;
                SendNative(MoveTemp(Q));
            }
            else if (Type == TEXT("ice"))
            { FNativeSignalRequest Q; if (!Direct::Ice(*B, Q)) return false; SendNative(MoveTemp(Q)); }
            else if (Type == TEXT("connection"))
            {
                FString State; if (!Direct::Fields(*B, {TEXT("state")}) || !Direct::Text(*B, TEXT("state"), State, 16)) return false;
                if (State == TEXT("connected")) bServerConnected = true;
                else if (State == TEXT("disconnected")) { Fail(TEXT("Disconnected")); return true; }
                else return false;
            }
            else if (Type == TEXT("error"))
            {
                FString Code; if (!Direct::Fields(*B, {TEXT("code")}) || !Direct::Text(*B, TEXT("code"), Code, 64)) return false;
                Fail(Code); return true;
            }
            else return false;
            ++Cursor; ++Snapshot.Events;
            if (Snapshot.State == EDirectClientState::Failed) return true;
        }
        return Cursor == Last;
    }
    void Completed(FHttpRequestPtr Request, FHttpResponsePtr Response, bool Success, int32 ExpectedGeneration)
    {
        check(IsInGameThread());
        if (bAborted || Generation != ExpectedGeneration || Http != Request) return;
        TArray<uint8> Data; bool Overflow;
        { FScopeLock Lock(&Receive->Mutex); Data = Receive->Data; Overflow = Receive->bOverflow; }
        Http->OnProcessRequestComplete().Unbind(); Http.Reset(); Receive.Reset();
        if (!Owner.IsValid() && Snapshot.State != EDirectClientState::Closing) { Changed = {}; Close(); return; }
        if (Overflow) { Fail(TEXT("PayloadTooLarge")); return; }
        if (!Success || !Response)
        {
            if (Attempt < 3 && FPlatformTime::Seconds() - RequestStarted < Config.RequestTimeoutSeconds * 3 + 1)
            { ++Snapshot.Retries; RetryAt = FPlatformTime::Seconds() + 0.1; return; }
            // Certificate/connection failures must not masquerade as a deadline expiry.
            Fail(Request.IsValid() && Request->GetFailureReason() == EHttpFailureReason::TimedOut
                ? TEXT("TimedOut") : TEXT("Unavailable")); return;
        }
        if (Response->GetURL() != Config.Endpoint) { Fail(TEXT("UnexpectedEndpoint")); return; }
        TSharedPtr<FJsonObject> Json; FString Error, Protocol, Status, Id, Session, Type; int32 V, Seq;
        const TSharedPtr<FJsonObject>* Body;
        if (!Direct::ParseJson(Data, Json, Error)
            || !Direct::Fields(Json, {TEXT("protocol"), TEXT("version"), TEXT("type"), TEXT("request_id"), TEXT("sequence"), TEXT("session_id"), TEXT("status"), TEXT("body")})
            || !Direct::Text(Json, TEXT("protocol"), Protocol, 32) || Protocol != TEXT("simurghrtc.direct")
            || !Direct::Integer(Json, TEXT("version"), V, 1) || V != 1 || !Direct::Text(Json, TEXT("type"), Type, 16) || Type != TEXT("result")
            || !Direct::Text(Json, TEXT("request_id"), Id, 64) || Id != Current.Id || !Direct::Integer(Json, TEXT("sequence"), Seq) || Seq != Current.Sequence
            || !Direct::Text(Json, TEXT("session_id"), Session, 36, true) || (Current.Type != TEXT("create") && Session != Snapshot.SessionId)
            || !Direct::Text(Json, TEXT("status"), Status, 16) || !Json->TryGetObjectField(TEXT("body"), Body))
        { Fail(TEXT("InvalidResponse")); return; }
        if (Status == TEXT("error"))
        {
            FString Code;
            if (!Direct::Fields(*Body, {TEXT("code")}) || !Direct::Text(*Body, TEXT("code"), Code, 64) || Response->GetResponseCode() != Direct::HttpStatus(Code))
            { Fail(TEXT("InvalidResponse")); return; }
            // A correlated, authenticated reply can confirm that cleanup won
            // the race. Keep waiting for local native closure in Tick().
            if (Snapshot.State == EDirectClientState::Closing && Current.Type == TEXT("close") && Code == TEXT("SessionClosed"))
            {
                Snapshot.bRemoteClosed = true; Wire.Reset(); RetryAt = 0; return;
            }
            Fail(Code); return;
        }
        if (Status != TEXT("accepted") || Response->GetResponseCode() != 200 || !Direct::SessionId(Session))
        { Fail(TEXT("InvalidResponse")); return; }
        const FString Kind = Current.Type;
        Wire.Reset(); RetryAt = 0; ++Sequence; ++Snapshot.AcceptedRequests;
        if (Kind == TEXT("create"))
        {
            FString State;
            if (!Direct::Fields(*Body, {TEXT("state")}) || !Direct::Text(*Body, TEXT("state"), State, 16) || State != TEXT("open")) { Fail(TEXT("InvalidResponse")); return; }
            Snapshot.SessionId = Session;
        }
        else if (Kind == TEXT("poll")) { if (!Events(*Body)) Fail(TEXT("InvalidResponse")); }
        else if (Kind == TEXT("close"))
        {
            FString State;
            if (!Direct::Fields(*Body, {TEXT("state")}) || !Direct::Text(*Body, TEXT("state"), State, 16) || State != TEXT("closed")) { Fail(TEXT("InvalidResponse")); return; }
            Snapshot.bRemoteClosed = true;
        }
        else if (Kind == TEXT("offer"))
        {
            FString State; int32 Count;
            if (!Direct::Fields(*Body, {TEXT("state"), TEXT("applied_ice")}) || !Direct::Text(*Body, TEXT("state"), State, 16) || State != TEXT("offer-set")
                || !Direct::Integer(*Body, TEXT("applied_ice"), Count, Direct::MaxIce)) Fail(TEXT("InvalidResponse"));
        }
        else if (Kind == TEXT("ice"))
        {
            int32 Pending, Applied;
            if (!Direct::Fields(*Body, {TEXT("pending_ice"), TEXT("applied_ice")}) || !Direct::Integer(*Body, TEXT("pending_ice"), Pending, Direct::MaxIce)
                || !Direct::Integer(*Body, TEXT("applied_ice"), Applied, Direct::MaxIce) || Pending + Applied > Direct::MaxIce) { Fail(TEXT("InvalidResponse")); return; }
            if (Current.Body->HasField(TEXT("completed"))) bEndAcknowledged = true;
        }
    }
    void Send()
    {
        ++Attempt; const int32 ThisGeneration = ++Generation;
        Http = FHttpModule::Get().CreateRequest(); Http->SetURL(Config.Endpoint); Http->SetVerb(TEXT("POST"));
        Http->SetTimeout(Config.RequestTimeoutSeconds); Http->SetActivityTimeout(Config.RequestTimeoutSeconds);
        Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
        Http->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Config.BearerToken);
        Http->SetHeader(TEXT("Content-Type"), TEXT("application/json")); Http->SetHeader(TEXT("Accept"), TEXT("application/json"));
        Http->SetContentAsString(Wire); Receive = MakeShared<FReceive, ESPMode::ThreadSafe>(); const auto Buffer = Receive.ToSharedRef();
        if (!Http->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Buffer](void* Data, int64& Length)
        {
            FScopeLock Lock(&Buffer->Mutex);
            if (Length < 0 || Length > Direct::MaxMessageBytes - Buffer->Data.Num()) { Buffer->bOverflow = true; Length = 0; return; }
            Buffer->Data.Append(static_cast<uint8*>(Data), int32(Length));
        }))) { Fail(TEXT("Unavailable")); return; }
        TWeakPtr<FImpl> Weak = AsShared();
        Http->OnProcessRequestComplete().BindLambda([Weak, ThisGeneration](FHttpRequestPtr Request, FHttpResponsePtr Response, bool Success)
        { if (auto Self = Weak.Pin()) Self->Completed(Request, Response, Success, ThisGeneration); });
        if (!Http->ProcessRequest() && Http) Completed(Http, {}, false, ThisGeneration);
    }
    bool Tick(float)
    {
        check(IsInGameThread()); if (bAborted) return false;
        const double Now = FPlatformTime::Seconds();
        if (!Owner.IsValid())
        {
            Changed = {};
            if (Snapshot.State == EDirectClientState::Connecting || Snapshot.State == EDirectClientState::Connected) Close();
        }
        if (Peer)
        {
            const auto S = Peer->GetSnapshot(); Snapshot.Transport = S.Transport; Snapshot.bNativeClosed = S.State == ENativePeerState::Closed;
            if ((Snapshot.State == EDirectClientState::Connecting || Snapshot.State == EDirectClientState::Connected) && S.State == ENativePeerState::Failed) Fail(TEXT("NativeError"));
        }
        else Snapshot.bNativeClosed = true;
        if (Snapshot.State == EDirectClientState::Connecting)
        {
            if (Now - Started > Config.ConnectTimeoutSeconds) Fail(TEXT("TimedOut"));
            else if (Peer && Peer->GetSnapshot().State == ENativePeerState::Ready && !bOfferStarted)
            {
                bOfferStarted = true; FString Error; TWeakPtr<FImpl> Weak = AsShared();
                if (!Peer->Signal({}, [Weak](const FNativeSignalResult& R)
                { if (auto Self = Weak.Pin()) { if (!R.IsSuccess()) Self->Fail(TEXT("NativeError")); else Self->Offer = R.Sdp; } }, Error)) Fail(TEXT("NativeError"));
            }
            if (Snapshot.State == EDirectClientState::Connecting && !Offer.IsEmpty() && !bOfferQueued && (!Config.bEarlyIceProbe || (bLocalEnd && bEndAcknowledged)))
            {
                auto Body = MakeShared<FJsonObject>(); Body->SetNumberField(TEXT("revision"), 1); Body->SetStringField(TEXT("sdp"), Offer);
                Enqueue(TEXT("offer"), Body); bOfferQueued = true;
            }
            const auto& T = Snapshot.Transport;
            if (Snapshot.State == EDirectClientState::Connecting && bAnswer && bEndAcknowledged && bServerConnected && NativePending == 0 && Queue.IsEmpty() && !Http && Wire.IsEmpty()
                && T.Connection == ENativeConnectionState::Connected && T.bDataTransportOpen && T.bRemoteIceComplete && T.bLocalIceComplete && T.RemoteIceQueued == 0
                && T.DtlsState == 3 && T.StatsSamples > 0) Notify(EDirectClientState::Connected);
        }
        if (Snapshot.State == EDirectClientState::Connecting || Snapshot.State == EDirectClientState::Connected || Snapshot.State == EDirectClientState::Closing)
        {
            if (!Http && !Wire.IsEmpty() && Now >= RetryAt) Send();
            if (!Http && Wire.IsEmpty())
            {
                if (Queue.IsEmpty() && !Snapshot.SessionId.IsEmpty() && Snapshot.State != EDirectClientState::Closing && Now >= NextPoll)
                {
                    auto Body = MakeShared<FJsonObject>(); Body->SetNumberField(TEXT("after_event"), Cursor); Enqueue(TEXT("poll"), Body);
                    NextPoll = Now + (Snapshot.State == EDirectClientState::Connected ? 1 : 0.5);
                }
                if (!Queue.IsEmpty())
                {
                    auto Pending = Queue[0]; Queue.RemoveAt(0);
                    Current = {}; Current.Type = Pending.Type; Current.Body = Pending.Body; Current.Sequence = Sequence;
                    Current.Session = Snapshot.SessionId; Current.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
                    Wire = RequestWire(Current); Attempt = 0; RequestStarted = Now; Send();
                }
            }
        }
        if (Snapshot.State == EDirectClientState::Closing && Snapshot.bNativeClosed && !Http && Wire.IsEmpty() && Queue.IsEmpty()) Notify(EDirectClientState::Closed);
        if (Snapshot.State == EDirectClientState::Closing && Now - ClosingAt > Config.RequestTimeoutSeconds * 3 + 2) Fail(TEXT("CloseTimedOut"));
        if (bNotify && Owner.IsValid() && Changed)
        {
            bNotify = false;
            const auto Callback = Changed; // The callback may release the client or close it reentrantly.
            Callback(Snapshot);
        }
        return !bAborted;
    }
};
FDirectClient::FDirectClient(TSharedRef<FImpl> InImpl) : Impl(MoveTemp(InImpl)) {}
FDirectClient::~FDirectClient() { Impl->Abort(); }
void FDirectClient::Close() { Impl->Close(); }
FDirectClientSnapshot FDirectClient::GetSnapshot() const { check(IsInGameThread()); return Impl->Snapshot; }
void FDirectClient::SetChannelHandler(FNativeChannelChanged Handler)
{ check(IsInGameThread()); if (Impl->Peer) Impl->Peer->SetChannelHandler(MoveTemp(Handler)); }
FSimurghRTCError FDirectClient::OpenChannel(const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out)
{
    check(IsInGameThread());
    Out = {};
    const auto Peer = Impl->Owner.IsValid() && Impl->Snapshot.State == EDirectClientState::Connected ? Impl->Peer : nullptr;
    return Peer ? Peer->OpenChannel(Options, Out) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectClient::CloseChannel(FSimurghRTCChannelHandle Channel)
{
    check(IsInGameThread());
    const auto Peer = Impl->Owner.IsValid() && Impl->Snapshot.State == EDirectClientState::Connected ? Impl->Peer : nullptr;
    return Peer ? Peer->CloseChannel(Channel) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectClient::GetChannelInfo(FSimurghRTCChannelHandle Channel, FNativeChannelSnapshot& Out) const
{
    check(IsInGameThread());
    Out = {};
    const auto Peer = Impl->Owner.IsValid() && Impl->Snapshot.State == EDirectClientState::Connected ? Impl->Peer : nullptr;
    return Peer ? Peer->GetChannelInfo(Channel, Out) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectClient::SendText(FSimurghRTCChannelHandle Channel, const FString& Text)
{
    check(IsInGameThread());
    const auto Peer = Impl->Owner.IsValid() && Impl->Snapshot.State == EDirectClientState::Connected ? Impl->Peer : nullptr;
    return Peer ? Peer->SendText(Channel, Text) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectClient::SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes)
{
    check(IsInGameThread());
    const auto Peer = Impl->Owner.IsValid() && Impl->Snapshot.State == EDirectClientState::Connected ? Impl->Peer : nullptr;
    return Peer ? Peer->SendBytes(Channel, Bytes) : SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Direct session is not connected."));
}
FSimurghRTCError FDirectClient::ValidateConfig(const FDirectClientConfig& Config)
{
    for (TCHAR C : Config.Identity) if (C == 0) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument,TEXT("Invalid Direct client identity."));
    if (!EndpointValid(Config) || !TokenValid(Config.BearerToken) || Config.Identity.IsEmpty()
        || Direct::Bytes(Config.Identity).Num() > 128 || (Config.Purpose != TEXT("data") && Config.Purpose != TEXT("call") && Config.Purpose != TEXT("text-room"))
        || !FMath::IsFinite(Config.RequestTimeoutSeconds) || Config.RequestTimeoutSeconds < 0.1 || Config.RequestTimeoutSeconds > 10
        || !FMath::IsFinite(Config.ConnectTimeoutSeconds) || Config.ConnectTimeoutSeconds < 0.1 || Config.ConnectTimeoutSeconds > 300)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument,TEXT("Direct requires a supported endpoint, identity, credential and timeout."));
    return ValidateIceServers(Config.IceServers, Config.bRelayOnly);
}
TSharedPtr<FDirectClient> FDirectClient::Connect(UObject* Owner, FDirectClientConfig Config, FDirectClientChanged Changed, FString& Error)
{
    check(IsInGameThread()); Error.Reset();
    if (bShuttingDown || !IsValid(Owner) || ValidateConfig(Config).IsError())
    { Error = TEXT("Invalid Direct client configuration."); return {}; }
    Clients.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
    if (Clients.Num() >= 32) { Error = TEXT("Direct client capacity reached."); return {}; }
    auto State = MakeShared<FImpl>(); State->Owner = Owner; State->Config = MoveTemp(Config); State->Changed = MoveTemp(Changed);
    State->Started = FPlatformTime::Seconds();
    State->Peer = FNativePeer::Open(Owner, State->Config.Purpose == TEXT("call") ? ENativeFactoryMode::BuiltinMedia : ENativeFactoryMode::DataOnly, {}, Error, State->Config.bLoopbackProbe, {State->Config.IceServers, State->Config.bRelayOnly});
    if (!State->Peer) return {};
    TWeakPtr<FImpl> Weak = State;
    State->Peer->SetEventHandler([Weak](const FNativeTransportEvent& E)
    {
        const auto Self = Weak.Pin();
        if (!Self || Self->bAborted || (Self->Snapshot.State != EDirectClientState::Connecting && Self->Snapshot.State != EDirectClientState::Connected)) return;
        if (E.Type == ENativeSignalEvent::FatalError) Self->Fail(TEXT("NativeError"));
        else if (E.Type == ENativeSignalEvent::LocalIce) Self->Enqueue(TEXT("ice"), Direct::IceBody(E.Ice));
        else if (E.Type == ENativeSignalEvent::LocalIceComplete)
        { Self->bLocalEnd = true; auto B = MakeShared<FJsonObject>(); B->SetNumberField(TEXT("revision"), 1); B->SetBoolField(TEXT("completed"), true); Self->Enqueue(TEXT("ice"), B); }
        else if (E.Type == ENativeSignalEvent::ConnectionChanged && E.Transport.Connection == ENativeConnectionState::Disconnected) Self->Fail(TEXT("Disconnected"));
    });
    auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("purpose"), State->Config.Purpose); Body->SetStringField(TEXT("identity"), State->Config.Identity);
    State->Enqueue(TEXT("create"), Body);
    State->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float Delta)
    { const auto Self = Weak.Pin(); return Self && Self->Tick(Delta); }));
    Clients.Add(State); return MakeShareable(new FDirectClient(State));
}
}
