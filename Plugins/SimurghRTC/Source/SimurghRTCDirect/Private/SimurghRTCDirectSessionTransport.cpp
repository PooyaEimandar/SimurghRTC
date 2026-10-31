// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCSessionTransport.h"
#include "SimurghRTCDirectClient.h"
#include "SimurghRTCNativeRuntime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace SimurghRTC
{
namespace
{
FSimurghRTCError DirectError(const FString& Code)
{
    if (Code.IsEmpty()) return {};
    ESimurghRTCErrorCode Kind = ESimurghRTCErrorCode::InternalError;
    if (Code == TEXT("TimedOut") || Code == TEXT("CloseTimedOut")) Kind = ESimurghRTCErrorCode::Timeout;
    else if (Code == TEXT("Unauthorized") || Code == TEXT("AdmissionRejected")) Kind = ESimurghRTCErrorCode::Unauthorized;
    else if (Code == TEXT("ResourceLimit")) Kind = ESimurghRTCErrorCode::ResourceExhausted;
    else if (Code == TEXT("InvalidState") || Code == TEXT("SessionClosed") || Code == TEXT("SessionNotFound") || Code == TEXT("Disconnected")) Kind = ESimurghRTCErrorCode::InvalidState;
    else if (Code == TEXT("InvalidArgument")) Kind = ESimurghRTCErrorCode::InvalidArgument;
    return SimurghRTC::MakeError(Kind,TEXT("The Direct connection ended or could not complete its request."));
}
class FDirectSessionTransport final : public ISessionTransport
{
    TSharedRef<FDirectClient> Client;
    bool bAudio = false;
public:
    explicit FDirectSessionTransport(TSharedRef<FDirectClient> InClient, bool Audio) : Client(MoveTemp(InClient)), bAudio(Audio) {}
    bool HasAudio() const override { return bAudio && Client->HasRemoteAudio(); }
    bool HasRemoteAudio() const override { return bAudio && Client->HasRemoteAudio(); }
    FSimurghRTCError PushAudio(const FSimurghRTCAudioFrame& F) override { return Client->PushAudio(F); }
    FSimurghRTCError MuteAudio(bool M) override { return Client->MuteAudio(M); }
    bool PollAudio(FSimurghRTCAudioFrame& F) override { return Client->PollAudio(F); }
    FSessionTransportSnapshot Snapshot() const override
    {
        const auto S = Client->GetSnapshot(); FSessionTransportSnapshot Out;
        switch (S.State)
        {
        case EDirectClientState::Connecting: Out.State = ESimurghRTCSessionState::Connecting; break;
        case EDirectClientState::Connected: Out.State = ESimurghRTCSessionState::Connected; break;
        case EDirectClientState::Closing: Out.State = ESimurghRTCSessionState::Closing; break;
        case EDirectClientState::Closed: Out.State = ESimurghRTCSessionState::Closed; break;
        case EDirectClientState::Failed: Out.State = ESimurghRTCSessionState::Failed; break;
        }
        Out.bNativeClosed = S.bNativeClosed; Out.bRemoteClosed = S.bRemoteClosed; Out.Error = DirectError(S.Error); return Out;
    }
    void Close() override { Client->Close(); }
    void SetHandler(FSessionTransportEventHandler Handler) override
    {
        if (!Handler) { Client->SetChannelHandler({}); return; }
        Client->SetChannelHandler([Handler = MoveTemp(Handler)](const FNativeChannelEvent& E)
        {
            if (E.Type != ENativeDataEvent::Message && E.Type != ENativeDataEvent::Changed) return;
            FSessionTransportEvent Out; Out.bMessage = E.Type == ENativeDataEvent::Message;
            Out.Channel = E.Channel.Info; Out.Message = E.Message; Handler(Out);
        });
    }
    FSimurghRTCError OpenChannel(const FSimurghRTCChannelOptions& O, FSimurghRTCChannelHandle& H) override { return Client->OpenChannel(O,H); }
    FSimurghRTCError CloseChannel(FSimurghRTCChannelHandle H) override { return Client->CloseChannel(H); }
    FSimurghRTCError Channel(FSimurghRTCChannelHandle H, FSessionChannelSnapshot& Out) const override
    {
        FNativeChannelSnapshot S; const auto Error = Client->GetChannelInfo(H,S);
        Out.Info = S.Info;
        switch (S.State)
        {
        case ENativeChannelState::Connecting: Out.State = ESessionChannelState::Connecting; break;
        case ENativeChannelState::Open: Out.State = ESessionChannelState::Open; break;
        case ENativeChannelState::Closing: Out.State = ESessionChannelState::Closing; break;
        case ENativeChannelState::Closed: Out.State = ESessionChannelState::Closed; break;
        }
        return Error;
    }
    FSimurghRTCError SendText(FSimurghRTCChannelHandle H, const FString& S) override { return Client->SendText(H,S); }
    FSimurghRTCError SendBytes(FSimurghRTCChannelHandle H, const TArray<uint8>& B) override { return Client->SendBytes(H,B); }
};
}
void RegisterDirectSessionTransport()
{
    SetSessionTransportFactory(ESimurghRTCProvider::DirectUnreal,[](UObject* Owner, const FSimurghRTCSessionConfig& Config, FSimurghRTCError& Error) -> TSharedPtr<ISessionTransport>
    {
        if (Config.Purpose == ESimurghRTCPurpose::Receive)
        { Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented,TEXT("Receive-only sessions are not integrated yet.")); return {}; }
        if (Config.Purpose == ESimurghRTCPurpose::Call && !SIMURGHRTC_WITH_MEDIA && !UE_SERVER)
        { Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::BuildExcluded, TEXT("Audio is excluded by this client profile.")); return {}; }
        FDirectClientConfig Direct; Direct.Endpoint = Config.Endpoint; Direct.BearerToken = Config.AccessToken; Direct.Identity = Config.LocalIdentity;
        Direct.Purpose = Config.Purpose == ESimurghRTCPurpose::Call ? TEXT("call") : Config.Purpose == ESimurghRTCPurpose::TextRoom ? TEXT("text-room") : TEXT("data");
        Direct.bAllowLoopbackHttp = Config.bAllowInsecureDevelopmentSignaling;
        Direct.IceServers = Config.IceServers; Direct.bRelayOnly = Config.bRelayOnly;
        Direct.ConnectTimeoutSeconds = FMath::Max(0.1f, float(Config.TimeoutMilliseconds)/1000.0f);
        Direct.RequestTimeoutSeconds = FMath::Clamp(Direct.ConnectTimeoutSeconds,0.1f,2.0f);
        // Explicit same-host fixture switch; ordinary connections retain normal ICE interfaces.
        Direct.bLoopbackProbe = FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCSessionLoopbackProbe"))
            && !Direct.bRelayOnly && ((Direct.bAllowLoopbackHttp && Direct.Endpoint.StartsWith(TEXT("http://127.0.0.1:")))
                || Direct.Endpoint.StartsWith(TEXT("https://127.0.0.1:")));
        Error = FDirectClient::ValidateConfig(Direct); if (Error.IsError()) return {};
        if (!FNativeRuntime::IsSupported())
        { Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable,TEXT("No native transport is available for this build.")); return {}; }
        FString Why; auto Client = FDirectClient::Connect(Owner,MoveTemp(Direct),{},Why);
        if (!Client)
        {
            const auto Code = Why.StartsWith(TEXT("Invalid Direct")) ? ESimurghRTCErrorCode::InvalidArgument
                : Why.Contains(TEXT("capacity")) ? ESimurghRTCErrorCode::ResourceExhausted : ESimurghRTCErrorCode::InternalError;
            Error = SimurghRTC::MakeError(Code,TEXT("Direct requires a supported endpoint, identity, credential and available native capacity.")); return {};
        }
        return MakeShared<FDirectSessionTransport>(Client.ToSharedRef(), Config.Purpose == ESimurghRTCPurpose::Call);
    });
}
}
