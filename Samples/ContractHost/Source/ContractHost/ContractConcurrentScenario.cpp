// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractConcurrentScenario.h"
#include "Misc/FileHelper.h"
#if SIMURGHRTC_WITH_DIRECT
#include "SimurghRTCDirectProtocol.h"
using namespace SimurghRTC;
namespace
{
const TCHAR* RoleName(int32 Role) { const TCHAR* Names[] = {TEXT("alpha"), TEXT("beta"), TEXT("gamma"), TEXT("delta")}; return Role >= 0 && Role < 4 ? Names[Role] : TEXT("invalid"); }
int32 RoleOf(const FString& Name) { for (int32 I = 0; I < 4; ++I) if (Name == RoleName(I)) return I; return -1; }
bool SameRoom(int32 A, int32 B) { return (A == 2) == (B == 2); }
int32 TargetOf(int32 Role) { return Role == 0 ? 1 : Role == 1 ? 2 : 0; }
FSimurghRTCDataMessage TextMessage(const FString& Text)
{ FSimurghRTCDataMessage M; M.Kind = ESimurghRTCMessageKind::Text; M.Text = Text; return M; }
FSimurghRTCDataMessage BinaryMessage(int32 Role, int32 Phase, bool Direct)
{
    FSimurghRTCDataMessage M; M.Kind = ESimurghRTCMessageKind::Binary; M.Bytes.SetNumUninitialized(260);
    M.Bytes[0] = Direct ? 'D' : 'E'; M.Bytes[1] = Role; M.Bytes[2] = Phase; M.Bytes[3] = Direct ? TargetOf(Role) : Role;
    for (int32 I = 4; I < M.Bytes.Num(); ++I) M.Bytes[I] = uint8(I * 17 + Role); return M;
}
bool Equal(const FSimurghRTCDataMessage& A, const FSimurghRTCDataMessage& B)
{ return A.Kind == B.Kind && A.Bytes == B.Bytes && A.Text.Len() == B.Text.Len() && FMemory::Memcmp(*A.Text, *B.Text, A.Text.Len()*sizeof(TCHAR)) == 0; }
FString Value(const TCHAR* Prefix, int32 Role, int32 Phase)
{ return FString::Printf(TEXT("%s|%s|%d"), Prefix, RoleName(Role), Phase); }
FString RoomAck(int32 Role, int32 Phase)
{
    const int32 Active = Phase == 1 ? 4 : 3; int32 Accepted = 0;
    for (int32 I = 0; I < Active; ++I) if (I != Role && SameRoom(Role,I)) ++Accepted;
    return FString::Printf(TEXT("room-ack|%d|%d|%d"), Phase, Accepted, Active - 1 - Accepted);
}
FString DirectAck(int32 Role, int32 Phase)
{ return FString::Printf(TEXT("direct-ack|%d|%s"), Phase, SameRoom(Role,TargetOf(Role)) ? TEXT("accepted") : TEXT("denied")); }
}
FContractConcurrentClient::FContractConcurrentClient(TSharedRef<FDirectClient> InClient, int32 InRole, bool bInReplacement,
    TFunction<bool(TSharedRef<FJsonObject>)> InProgress) : Client(InClient), Progress(MoveTemp(InProgress)), Role(InRole), bReplacement(bInReplacement)
{ Client->SetChannelHandler([this](const FNativeChannelEvent& E) { Event(E); }); }
FContractConcurrentClient::~FContractConcurrentClient() { Client->SetChannelHandler({}); }
void FContractConcurrentClient::Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
void FContractConcurrentClient::SendText(const FString& Text)
{ if (Client->SendText(Channel,Text).IsError()) Fail(TEXT("Concurrent fixture send failed.")); }
void FContractConcurrentClient::Tick()
{
    if (!bOpened)
    {
        FSimurghRTCChannelOptions O; O.Label = TEXT("sr104");
        if (Client->OpenChannel(O,Channel).IsError()) Fail(TEXT("Concurrent channel open failed."));
        bOpened = true;
    }
}
void FContractConcurrentClient::Begin(int32 NewPhase)
{
    if (NewPhase != (Phase == 0 ? (bReplacement ? 2 : 1) : Phase + 1) || NewPhase > 2 || (Phase && !bSealed)) { Fail(TEXT("Unexpected phase command.")); return; }
    Phase = NewPhase; Received = 0; bDone = bSealed = false; Expected.Reset();
    Expected.Add(TextMessage(Value(TEXT("echo"),Role,Phase))); Expected.Add(BinaryMessage(Role,Phase,false));
    for (int32 Other = 0; Other < (Phase == 1 ? 4 : 3); ++Other)
    {
        if (Other == Role || !SameRoom(Role,Other)) continue;
        Expected.Add(TextMessage(Value(TEXT("room"),Other,Phase)));
        if (TargetOf(Other) == Role) Expected.Add(BinaryMessage(Other,Phase,true));
    }
    Expected.Add(TextMessage(RoomAck(Role,Phase))); Expected.Add(TextMessage(DirectAck(Role,Phase))); ExpectedCount = Expected.Num();
    SendText(Value(TEXT("echo"),Role,Phase));
    if (Client->SendBytes(Channel,BinaryMessage(Role,Phase,false).Bytes).IsError()) Fail(TEXT("Binary echo submission failed."));
    // Gamma deliberately claims Alpha in the body. Routing must use its authenticated session.
    SendText(Value(TEXT("room"),Role == 2 ? 0 : Role,Phase));
    if (Client->SendBytes(Channel,BinaryMessage(Role,Phase,true).Bytes).IsError()) Fail(TEXT("Addressed binary submission failed."));
}
void FContractConcurrentClient::Event(const FNativeChannelEvent& E)
{
    bGameThread &= IsInGameThread();
    if (E.Channel.Info.Handle.Id != Channel.Id || E.Type == ENativeDataEvent::Error) { Fail(TEXT("Unexpected channel event.")); return; }
    if (E.Type != ENativeDataEvent::Message) return;
    const auto& M = E.Message;
    if (M.Kind == ESimurghRTCMessageKind::Text && M.Text.StartsWith(TEXT("go|"))) { Begin(FCString::Atoi(*M.Text.Mid(3))); return; }
    if (M.Kind == ESimurghRTCMessageKind::Text && M.Text == FString::Printf(TEXT("seal|%d"),Phase))
    {
        if (!bDone || !Expected.IsEmpty() || bSealed) { Fail(TEXT("Premature or duplicate phase barrier.")); return; }
        bSealed = true; auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("phase"),Phase); J->SetNumberField(TEXT("received"),Received);
        J->SetNumberField(TEXT("expected"),ExpectedCount); J->SetBoolField(TEXT("sealed"),true); Phases.Add(MakeShared<FJsonValueObject>(J));
        if (!Progress(Report())) { Fail(TEXT("Could not save phase evidence.")); return; }
        SendText(FString::Printf(TEXT("sealed|%d"),Phase)); return;
    }
    if (M.Kind == ESimurghRTCMessageKind::Text && M.Text == TEXT("finish"))
    {
        if (!bSealed || Phase != 2) { Fail(TEXT("Unexpected finish.")); return; }
        bFinish = true; Client->Close(); return;
    }
    const int32 Match = Expected.IndexOfByPredicate([&](const auto& Wanted) { return Equal(M,Wanted); });
    if (!Phase || bSealed || Match == INDEX_NONE) { Fail(TEXT("Unexpected, duplicate or unauthorized message.")); return; }
    Expected.RemoveAt(Match); ++Received;
    if (Expected.IsEmpty() && !bDone) { bDone = true; SendText(FString::Printf(TEXT("done|%d"),Phase)); }
}
bool FContractConcurrentClient::Finished(const FDirectClientSnapshot& S)
{
    if (!Error.IsEmpty()) return true;
    if (S.State == EDirectClientState::Failed)
    {
        if (Role == 1 && !bReplacement && Phase == 1 && bSealed && S.Error == TEXT("SessionClosed")) return S.bNativeClosed;
        Error = S.Error; return true;
    }
    if (S.State == EDirectClientState::Closed)
    {
        if (!bFinish || !S.bNativeClosed || !S.bRemoteClosed) Fail(TEXT("Incomplete graceful cleanup.")); return true;
    }
    return false;
}
TSharedRef<FJsonObject> FContractConcurrentClient::Report() const
{
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("role"),RoleName(Role)); J->SetBoolField(TEXT("replacement"),bReplacement);
    J->SetArrayField(TEXT("phases"),Phases); J->SetBoolField(TEXT("callbacks_on_game_thread"),bGameThread);
    J->SetBoolField(TEXT("unexpected_messages"),!Error.IsEmpty()); J->SetStringField(TEXT("session_id"),Client->GetSnapshot().SessionId); return J;
}
#endif
#if CONTRACTHOST_WITH_DIRECT_SERVER
bool FContractConcurrentServer::Configure(FDirectServerConfig& Config, const FString& CredentialFile)
{
    FString Text, Error; TSharedPtr<FJsonObject> J;
    if (!FFileHelper::LoadFileToString(Text,*CredentialFile) || !Direct::ParseJson(Direct::Bytes(Text),J,Error)) return false;
    for (int32 I = 0; I < 5; ++I)
    {
        const FString Name = I == 4 ? TEXT("spare") : RoleName(I); FString Token;
        if (!J->TryGetStringField(Name,Token) || Token.Len() < 32 || Token.Len() > 128 || Tokens.Contains(Token)) return false;
        Tokens.Add(Token,Name);
    }
    Config.MaxSessions = 4; Config.MaxSessionsPerPrincipal = 1; Config.IdleSeconds = 10; Config.ReplaySeconds = 2;
    Config.Admit = [this](const FDirectAdmissionRequest& A)
    { FDirectAdmissionResult R; if (const auto* P = Tokens.Find(A.BearerToken)) { R.Principal = *P; R.bAllowCreate = true; } return R; };
    Config.CanRoute = [this](const FDirectSessionInfo& A, const FDirectSessionInfo& B, ESimurghRTCMessageKind)
    {
        if (bCloseDuringPolicy && B.Principal == TEXT("beta"))
        {
            bCloseDuringPolicy = false;
            if (const auto S = Server.Pin())
                if (S->CloseSession(B.SessionId).IsError() || S->CloseSession(B.SessionId).IsError()) Fail(TEXT("Reentrant close failed."));
        }
        const int32 X = RoleOf(A.Principal), Y = RoleOf(B.Principal); return X >= 0 && Y >= 0 && SameRoom(X,Y);
    };
    Config.OnSessionChanged = [this](const FDirectSessionInfo& I) { Session(I); };
    Config.OnChannelEvent = [this](const FString& Id, const FNativeChannelEvent& E) { Event(Id,E); }; return true;
}
void FContractConcurrentServer::Session(const FDirectSessionInfo& I)
{
    bGameThread &= IsInGameThread(); Sessions.Add(I.SessionId,I);
    if (RoleOf(I.Principal) < 0 || I.Identity != TEXT("shared-identity")) { Fail(TEXT("Unexpected principal or metadata.")); return; }
    if (I.State == EDirectServerSessionState::Connected && I.Principal == TEXT("beta") && OriginalBeta.IsEmpty()) OriginalBeta = I.SessionId;
    if (I.State == EDirectServerSessionState::Closing || I.State == EDirectServerSessionState::Closed || I.State == EDirectServerSessionState::Failed)
    {
        if (I.SessionId == OriginalBeta && Addresses.Contains(I.SessionId)) Retired = Addresses[I.SessionId];
        Addresses.Remove(I.SessionId);
    }
    if (I.State == EDirectServerSessionState::Closed && I.bNativeClosed) NativeClosed.Add(I.SessionId);
}
void FContractConcurrentServer::Event(const FString& Id, const FNativeChannelEvent& E)
{
    bGameThread &= IsInGameThread(); const auto S = Server.Pin(); if (!S) return;
    FDirectSessionInfo Info; if (S->GetSessionInfo(Id,Info).IsError()) { Fail(TEXT("Channel has no session metadata.")); return; }
    if (E.Type == ENativeDataEvent::Error) { Fail(TEXT("Server channel error.")); return; }
    if (E.Type == ENativeDataEvent::Changed && E.Channel.State == ENativeChannelState::Open && E.Channel.bWritable)
    {
        if (!E.Channel.bRemote || E.Channel.Info.Options.Label != TEXT("sr104") || !E.Channel.Info.Options.bOrdered
            || E.Channel.Info.Options.Reliability != ESimurghRTCReliability::Reliable) { Fail(TEXT("Unexpected client channel.")); return; }
        if (const auto* Old = Addresses.Find(Id); Old && Old->Channel.Id != E.Channel.Info.Handle.Id) { Fail(TEXT("Duplicate client channel.")); return; }
        Addresses.Add(Id,{Id,E.Channel.Info.Handle});
    }
    if (E.Type == ENativeDataEvent::Message) Message(Id,E.Message);
}
void FContractConcurrentServer::Send(const FDirectChannelAddress& A, const FString& Text)
{ const auto S = Server.Pin(); if (!S || S->SendText(A.SessionId,A.Channel,Text).IsError()) Fail(TEXT("Server control send failed.")); }
void FContractConcurrentServer::Message(const FString& Id, const FSimurghRTCDataMessage& M)
{
    const auto S = Server.Pin(); const auto* Address = Addresses.Find(Id); FDirectSessionInfo Info;
    if (!S || !Address || S->GetSessionInfo(Id,Info).IsError() || !Phase || !ExpectedClients) { Fail(TEXT("Unexpected message source/phase.")); return; }
    const auto Origin = *Address; const int32 Role = RoleOf(Info.Principal);
    if (M.Kind == ESimurghRTCMessageKind::Text && M.Text == FString::Printf(TEXT("done|%d"),Phase))
    {
        if (Inputs.FindOrAdd(Id).Num() != 4 || Done.Contains(Id)) { Fail(TEXT("Incomplete or duplicate sender completion.")); return; }
        Done.Add(Id);
        if (Done.Num() == ExpectedClients)
        { bBarrier = true; for (const auto& Pair : Addresses) Send(Pair.Value,FString::Printf(TEXT("seal|%d"),Phase)); }
        return;
    }
    if (M.Kind == ESimurghRTCMessageKind::Text && M.Text == FString::Printf(TEXT("sealed|%d"),Phase))
    {
        if (!bBarrier || Sealed.Contains(Id)) { Fail(TEXT("Unexpected seal acknowledgment.")); return; }
        Sealed.Add(Id);
        if (Sealed.Num() == ExpectedClients)
        {
            auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("phase"),Phase); J->SetNumberField(TEXT("clients"),ExpectedClients);
            J->SetNumberField(TEXT("echoes"),Echoes); J->SetNumberField(TEXT("routed"),Routed); J->SetNumberField(TEXT("denied"),Denied);
            TArray<TSharedPtr<FJsonValue>> Ids; for (const auto& Value : Sealed) Ids.Add(MakeShared<FJsonValueString>(Value));
            J->SetArrayField(TEXT("sealed_sessions"),Ids); PhaseReports.Add(MakeShared<FJsonValueObject>(J));
        }
        return;
    }
    FString Kind; FSimurghRTCDataMessage Out = M; TArray<FDirectChannelAddress> Targets;
    if (M.Kind == ESimurghRTCMessageKind::Text && M.Text == Value(TEXT("echo"),Role,Phase)) Kind = TEXT("echo-text");
    else if (M.Kind == ESimurghRTCMessageKind::Binary && Equal(M,BinaryMessage(Role,Phase,false))) Kind = TEXT("echo-binary");
    else if (M.Kind == ESimurghRTCMessageKind::Text && M.Text == Value(TEXT("room"),Role == 2 ? 0 : Role,Phase))
    {
        Kind = TEXT("room"); Out.Text = Value(TEXT("room"),Role,Phase);
        for (const auto& Pair : Addresses) if (Pair.Key != Id) Targets.Add(Pair.Value);
    }
    else if (M.Kind == ESimurghRTCMessageKind::Binary && Equal(M,BinaryMessage(Role,Phase,true)))
    {
        Kind = TEXT("direct");
        for (const auto& Pair : Addresses)
        { FDirectSessionInfo Target; S->GetSessionInfo(Pair.Key,Target); if (Target.Principal == RoleName(TargetOf(Role))) Targets.Add(Pair.Value); }
    }
    else { Fail(TEXT("Unexpected or forged fixture payload.")); return; }
    auto& Seen = Inputs.FindOrAdd(Id);
    if (Seen.Contains(Kind) || bBarrier) { Fail(TEXT("Duplicate or late source message.")); return; } Seen.Add(Kind);
    if (Kind.StartsWith(TEXT("echo"))) { if (S->EchoMessage(Id,M).IsError()) Fail(TEXT("Private echo failed.")); else ++Echoes; return; }
    TArray<FDirectRouteResult> Results;
    if (S->RouteMessage(Origin,Out,Targets,Results).IsError() || Results.Num() != Targets.Num()) { Fail(TEXT("Routing failed.")); return; }
    for (const auto& R : Results)
    {
        FDirectSessionInfo Destination; S->GetSessionInfo(R.Destination.SessionId,Destination);
        const bool Allowed = SameRoom(Role,RoleOf(Destination.Principal));
        if (Allowed && !R.Error.IsError()) ++Routed;
        else if (!Allowed && R.Error.Code == ESimurghRTCErrorCode::Unauthorized) ++Denied;
        else { Fail(TEXT("Routing policy or admission result differed.")); return; }
    }
    Send(Origin,Kind == TEXT("room") ? RoomAck(Role,Phase) : DirectAck(Role,Phase));
}
bool FContractConcurrentServer::Command(const FString& Action)
{
    const auto S = Server.Pin(); if (!S) return false;
    if (Action == TEXT("phase1") || Action == TEXT("phase2"))
    {
        const int32 Next = Action == TEXT("phase1") ? 1 : 2;
        if (Next != Phase + 1 || (Phase && Sealed.Num() != ExpectedClients) || Addresses.Num() != (Next == 1 ? 4 : 3)) return false;
        Phase = Next; ExpectedClients = Addresses.Num(); Inputs.Reset(); Done.Reset(); Sealed.Reset(); bBarrier = false; Echoes = Routed = Denied = 0;
        TArray<FDirectChannelAddress> List; Addresses.GenerateValueArray(List);
        // Foreign handles must not become usable by combining another session ID with them.
        if (S->SendText(List[0].SessionId,List[1].Channel,TEXT("must-not-arrive")).Code != ESimurghRTCErrorCode::InvalidState) return false;
        ++IsolationChecks;
        auto M = TextMessage(TEXT("must-not-arrive")); M.Channel = List[0].Channel; TArray<FDirectRouteResult> Results;
        FDirectChannelAddress Wrong{List[1].SessionId,List[0].Channel};
        if (S->RouteMessage(List[0],M,{Wrong},Results).IsError() || Results.Num()!=1 || Results[0].Error.Code!=ESimurghRTCErrorCode::InvalidState) return false;
        ++IsolationChecks;
        if (S->RouteMessage(List[0],M,{List[1],List[1]},Results).Code != ESimurghRTCErrorCode::InvalidArgument) return false;
        ++IsolationChecks;
        if (Next == 2)
        {
            if (S->SendBytes(Retired.SessionId,Retired.Channel,{}).Code != ESimurghRTCErrorCode::InvalidState) return false;
            if (S->RouteMessage(List[0],M,{Retired},Results).IsError() || Results.Num()!=1 || Results[0].Error.Code!=ESimurghRTCErrorCode::InvalidState) return false;
            IsolationChecks += 2;
        }
        for (const auto& Pair : Addresses) Send(Pair.Value,FString::Printf(TEXT("go|%d"),Phase)); return true;
    }
    if (Action == TEXT("close-beta"))
    {
        if (Phase != 1 || Sealed.Num()!=4 || OriginalBeta.IsEmpty()) return false;
        FDirectChannelAddress Alpha;
        for (const auto& Pair : Addresses)
        { FDirectSessionInfo I; S->GetSessionInfo(Pair.Key,I); if (I.Principal == TEXT("alpha")) Alpha = Pair.Value; }
        const auto* Beta = Addresses.Find(OriginalBeta); if (!Beta || !Alpha.Channel.IsValid()) return false;
        Retired = *Beta; bCloseDuringPolicy = true;
        auto M = TextMessage(TEXT("must-not-arrive")); M.Channel = Alpha.Channel; TArray<FDirectRouteResult> Results;
        if (S->RouteMessage(Alpha,M,{*Beta},Results).IsError() || Results.Num()!=1
            || Results[0].Error.Code != ESimurghRTCErrorCode::InvalidState || bCloseDuringPolicy) return false;
        ++IsolationChecks; return true;
    }
    if (Action == TEXT("finish"))
    { if (Phase!=2 || Sealed.Num()!=3) return false; for (const auto& Pair:Addresses) Send(Pair.Value,TEXT("finish")); return true; }
    return false;
}
TSharedRef<FJsonObject> FContractConcurrentServer::Report() const
{
    auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("ready"),Addresses.Num()); J->SetNumberField(TEXT("phase"),Phase);
    J->SetNumberField(TEXT("sealed"),Sealed.Num()); J->SetNumberField(TEXT("native_closed"),NativeClosed.Num());
    J->SetNumberField(TEXT("isolation_checks"),IsolationChecks); J->SetBoolField(TEXT("callbacks_on_game_thread"),bGameThread);
    J->SetArrayField(TEXT("phases"),PhaseReports);
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const auto& Pair : Sessions)
    {
        auto V = MakeShared<FJsonObject>(); V->SetStringField(TEXT("session_id"),Pair.Key); V->SetStringField(TEXT("principal"),Pair.Value.Principal);
        V->SetNumberField(TEXT("state"),int32(Pair.Value.State)); V->SetBoolField(TEXT("native_closed"),Pair.Value.bNativeClosed); Entries.Add(MakeShared<FJsonValueObject>(V));
    }
    J->SetArrayField(TEXT("sessions"),Entries); return J;
}
#endif
