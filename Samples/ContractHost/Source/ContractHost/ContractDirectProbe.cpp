// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDirectProbe.h"
#include "ContractDataScenario.h"
#include "ContractConcurrentScenario.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SimurghRTCNativeRuntime.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#if PLATFORM_IOS
#include <unistd.h>
#endif
#if SIMURGHRTC_WITH_DIRECT
#include "SimurghRTCDirectClient.h"
#include "SimurghRTCDirectProtocol.h"
#endif
#if CONTRACTHOST_WITH_DIRECT_SERVER
#include "SimurghRTCDirectServer.h"
#include "HttpServerModule.h"
#include "HttpPath.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#endif
DEFINE_LOG_CATEGORY_STATIC(LogSimurghRTCDirectProbe, Log, All);
using namespace SimurghRTC;
class FContractDirectProbe::FImpl
{
public:
    FDelegateHandle Init;
    FTSTicker::FDelegateHandle Ticker;
    FString RunId, Token, Endpoint, Error, Scenario = TEXT("connect"), ExpectedError;
    TStrongObjectPtr<UGameInstance> Owner;
    int32 Requested = 4, Index = 0, Port = 18779, CallbackCount = 0, CallbacksAtOwnerLoss = 0;
    double Started = 0, StepStarted = 0, StopAt = 0;
    bool bServer = false, bStopping = false, bGameThread = true, bOwnerLost = false, bClosing = false;
    FString CredentialsFile; int32 ConcurrentRole = 0; bool bReplacement = false;
    TArray<TSharedPtr<FJsonValue>> Rows;
    TSharedPtr<FJsonObject> Row;
#if SIMURGHRTC_WITH_DIRECT
    TSharedPtr<FDirectClient> Client;
    TUniquePtr<FContractDataScenario> Data;
    TUniquePtr<FContractConcurrentClient> ConcurrentClient;
#endif
#if CONTRACTHOST_WITH_DIRECT_SERVER
    TSharedPtr<FDirectServer> Server;
    TUniquePtr<FContractConcurrentServer> ConcurrentServer;
    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle Health, ShutdownRoute, ConcurrentRoute;
    TArray<FString> PreviousOverrides;
    bool bOwnsHttp = false, bOverrides = false;
    struct FEcho { FString Session; FSimurghRTCDataMessage Message; };
    TArray<FEcho> Echoes;
    TSet<FGuid> DataOpened, DataClosed;
    int32 EchoBytes = 0, EchoReceived = 0, EchoSent = 0;

#endif
    ~FImpl()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(Init); FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
#if SIMURGHRTC_WITH_DIRECT
        ConcurrentClient.Reset(); Data.Reset(); if (Client) Client->Close(); Client.Reset();
#endif
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if (Server) Server->Stop(); Server.Reset(); ConcurrentServer.Reset();
        if (Router && Health) Router->UnbindRoute(Health);
        if (Router && ShutdownRoute) Router->UnbindRoute(ShutdownRoute);
        if (Router && ConcurrentRoute) Router->UnbindRoute(ConcurrentRoute);
        Router.Reset(); Health.Reset(); ShutdownRoute.Reset();
        if (bOwnsHttp && FHttpServerModule::IsAvailable()) FHttpServerModule::Get().StopAllListeners();
        if (bOverrides && GConfig) GConfig->SetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), PreviousOverrides, GEngineIni);
#endif
        Owner.Reset();
    }
    void Fail(const FString& Why) { if (Error.IsEmpty()) Error = Why; }
#if SIMURGHRTC_WITH_DIRECT
    TSharedRef<FJsonObject> Transport(const FNativeTransportSnapshot& T)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("connection"), int32(T.Connection)); J->SetNumberField(TEXT("ice"), T.IceState);
        J->SetNumberField(TEXT("dtls"), T.DtlsState); J->SetNumberField(TEXT("protocol"), T.SelectedProtocol);
        J->SetBoolField(TEXT("sctp_open"), T.bDataTransportOpen); J->SetNumberField(TEXT("queued_ice"), T.RemoteIceQueued);
        J->SetNumberField(TEXT("applied_ice"), T.RemoteIceApplied); J->SetBoolField(TEXT("remote_end"), T.bRemoteIceComplete);
        J->SetNumberField(TEXT("stats_samples"), T.StatsSamples); J->SetNumberField(TEXT("stun_responses"), double(T.StunResponsesReceived)); return J;
    }
    TSharedRef<FJsonObject> Metadata()
    {
        auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("schema"), 1); J->SetStringField(TEXT("run_id"), RunId);
#if PLATFORM_IOS
        J->SetNumberField(TEXT("pid"), int32(getpid()));
#else
        J->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
#endif
        J->SetBoolField(TEXT("server_build"), UE_SERVER != 0);
        J->SetBoolField(TEXT("editor_build"), WITH_EDITOR != 0); J->SetBoolField(TEXT("with_janus"), SIMURGHRTC_WITH_JANUS != 0);
        J->SetBoolField(TEXT("arm64_build"), PLATFORM_CPU_ARM_FAMILY && sizeof(void*) == 8);
        J->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString()); J->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
        J->SetStringField(TEXT("artifact_manifest_sha256"), FNativeRuntime::GetMobileArtifactSHA256());
        J->SetStringField(TEXT("native_backend"), FNativeRuntime::GetBackendVersion());
        J->SetBoolField(TEXT("loopback_only"), true); J->SetBoolField(TEXT("network_media_tested"), false); J->SetBoolField(TEXT("application_messages_tested"), (Scenario == TEXT("data") || Scenario == TEXT("concurrent") || Scenario == TEXT("shared")));
        return J;
    }
#if CONTRACTHOST_WITH_DIRECT_SERVER
    TSharedRef<FJsonObject> ServerReport()
    {
        auto J = Metadata(); const auto S = Server ? Server->GetSnapshot() : FDirectServerSnapshot{};
        J->SetNumberField(TEXT("connected_sessions"), S.ConnectedSessions); J->SetNumberField(TEXT("peak_live_peers"), S.PeakLivePeers);
        J->SetNumberField(TEXT("peak_connected_sessions"), S.PeakConnectedSessions); J->SetNumberField(TEXT("records"), S.Records);
        if (ConcurrentServer) J->SetObjectField(TEXT("concurrent"),ConcurrentServer->Report());
        J->SetBoolField(TEXT("running"), S.bRunning); J->SetNumberField(TEXT("live_peers"), S.LivePeers);
        J->SetNumberField(TEXT("created"), S.Created); J->SetNumberField(TEXT("connected"), S.Connected);
        J->SetNumberField(TEXT("closed"), S.Closed); J->SetNumberField(TEXT("expired"), S.Expired);
        J->SetNumberField(TEXT("replays"), S.Replays); J->SetNumberField(TEXT("rejected"), S.Rejected); J->SetNumberField(TEXT("early_ice"), S.EarlyIce);
        auto Strings = [](const TArray<FString>& Source) { TArray<TSharedPtr<FJsonValue>> Out; for (const auto& Value : Source) Out.Add(MakeShared<FJsonValueString>(Value)); return Out; };
        J->SetArrayField(TEXT("connected_session_ids"), Strings(S.ConnectedSessionIds)); J->SetArrayField(TEXT("closed_session_ids"), Strings(S.ClosedSessionIds));
        J->SetObjectField(TEXT("last_transport"), Transport(S.LastConnectedTransport));
        if (Scenario == TEXT("data") || Scenario == TEXT("shared"))
        {
            J->SetNumberField(TEXT("data_channels_opened"), DataOpened.Num()); J->SetNumberField(TEXT("data_channels_closed"), DataClosed.Num());
            J->SetNumberField(TEXT("data_received"), EchoReceived); J->SetNumberField(TEXT("data_sent"), EchoSent);
            J->SetNumberField(TEXT("echo_pending"), Echoes.Num());
        }
        return J;
    }
    bool StartServer()
    {
        if (!IsRunningDedicatedServer() || FHttpServerModule::IsAvailable()) { Fail(TEXT("Direct probe requires a dedicated process with no existing HTTPServer owner.")); return false; }
        GConfig->GetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), PreviousOverrides, GEngineIni);
        auto Overrides = PreviousOverrides;
        Overrides.Insert(FString::Printf(TEXT("(Port=%d,BindAddress=127.0.0.1,ReuseAddressAndPort=false)"), Port), 0);
        GConfig->SetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni); bOverrides = true;
        auto& Http = FHttpServerModule::Get(); bOwnsHttp = true; Http.StartAllListeners(); Router = Http.GetHttpRouter(Port, true);
        if (!Router) { Fail(TEXT("HTTP listener bind failed.")); return false; }
        FDirectServerConfig Config; Config.bLoopbackProbe = true; Config.IdleSeconds = 5; Config.ReplaySeconds = 30;
        if (Scenario == TEXT("shared")) Config.MaxSessionsPerPrincipal = 16;
        if (Scenario == TEXT("data") || Scenario == TEXT("shared")) Config.OnChannelEvent = [this](const FString& Session, const FNativeChannelEvent& Event)
        {
            bGameThread &= IsInGameThread();
            const auto& C = Event.Channel;
            if (Event.Type == ENativeDataEvent::Error) { Fail(TEXT("Server DataChannel error.")); return; }
            if (Event.Type == ENativeDataEvent::Changed)
            {
                if (C.State == ENativeChannelState::Open)
                {
                    const int32 Mode = FCString::Atoi(*C.Info.Options.Label.Mid(6));
                    const auto O = FContractDataScenario::Options(Mode);
                    const bool Valid = Scenario == TEXT("shared") ? C.Info.Options.Label == TEXT("sr105-main") && C.Info.Options.bOrdered
                        && C.Info.Options.Reliability == ESimurghRTCReliability::Reliable && C.Info.Options.ReliabilityValue == 0
                        : Mode >= 0 && Mode <= 5 && C.Info.Options.Label == O.Label && C.Info.Options.bOrdered == O.bOrdered
                        && C.Info.Options.Reliability == O.Reliability && C.Info.Options.ReliabilityValue == O.ReliabilityValue;
                    if (!Valid
                        || !C.bRemote || C.Info.MaximumMessageBytes != 65536) { Fail(TEXT("Server remote channel options differ.")); return; }
                    DataOpened.Add(C.Info.Handle.Id);
                }
                if (C.State == ENativeChannelState::Closed) DataClosed.Add(C.Info.Handle.Id);
            }
            if (Event.Type != ENativeDataEvent::Message) return;
            const int32 Bytes = Event.Message.Bytes.Num() + Event.Message.Text.Len() * sizeof(TCHAR);
            if (Echoes.Num() >= 64 || EchoBytes + Bytes > 1048576) { Fail(TEXT("Probe echo queue exhausted.")); return; }
            Echoes.Add({Session, Event.Message}); EchoBytes += Bytes; ++EchoReceived;
        };
        Config.Admit = [this](const FDirectAdmissionRequest& A)
        {
            FDirectAdmissionResult Result;
            if (A.BearerToken == Token) { Result.Principal = TEXT("probe-user"); Result.bAllowCreate = A.Identity != TEXT("deny"); }
            else if (A.BearerToken == Token + TEXT("-other")) { Result.Principal = TEXT("other-user"); Result.bAllowCreate = true; }
            return Result;
        };
        if (Scenario == TEXT("concurrent"))
        {
            ConcurrentServer = MakeUnique<FContractConcurrentServer>([this](const FString& Why) { Fail(Why); });
            if (!ConcurrentServer->Configure(Config,CredentialsFile)) { Fail(TEXT("Invalid concurrent credentials.")); return false; }
        }
        Server = FDirectServer::Bind(Owner.Get(), Router.ToSharedRef(), MoveTemp(Config), Error);
        if (!Server) return false;
        if (ConcurrentServer)
        {
            ConcurrentServer->Attach(Server);
            ConcurrentRoute = Router->BindRoute(FHttpPath(TEXT("/simurghrtc/probe/concurrent")), EHttpServerRequestVerbs::VERB_POST,
                FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
                {
                    const auto* A = Request.Headers.Find(TEXT("authorization"));
                    if (!A || A->Num()!=1 || (*A)[0] != TEXT("Bearer ") + Token)
                    { Complete(FHttpServerResponse::Error(static_cast<EHttpServerResponseCodes>(401))); return true; }
                    TSharedPtr<FJsonObject> Body; FString Why, Action;
                    if (!Direct::ParseJson(Request.Body,Body,Why) || !Body->TryGetStringField(TEXT("action"),Action) || !ConcurrentServer->Command(Action))
                    { Complete(FHttpServerResponse::Error(static_cast<EHttpServerResponseCodes>(409))); return true; }
                    Complete(FHttpServerResponse::Ok()); return true;
                }));
            if (!ConcurrentRoute) { Fail(TEXT("Concurrent control route failed.")); return false; }
        }
        Health = Router->BindRoute(FHttpPath(TEXT("/simurghrtc/probe/direct")), EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& Complete)
            { Complete(FHttpServerResponse::Create(Direct::Write(ServerReport()), TEXT("application/json"))); return true; }));
        ShutdownRoute = Router->BindRoute(FHttpPath(TEXT("/simurghrtc/probe/stop")), EHttpServerRequestVerbs::VERB_POST,
            FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
            {
                const auto* Authorization = Request.Headers.Find(TEXT("authorization"));
                if (!Authorization || Authorization->Num() != 1 || (*Authorization)[0] != TEXT("Bearer ") + Token)
                { Complete(FHttpServerResponse::Error(static_cast<EHttpServerResponseCodes>(401))); return true; }
                bStopping = true; StopAt = FPlatformTime::Seconds(); Server->Stop(); Complete(FHttpServerResponse::Ok()); return true;
            }));
        if (!Health || !ShutdownRoute) { Fail(TEXT("Probe routes could not bind.")); return false; }
        UE_LOG(LogSimurghRTCDirectProbe, Display, TEXT("SIMURGHRTC_DIRECT_READY run=%s pid=%u port=%d"), *RunId, FPlatformProcess::GetCurrentProcessId(), Port);
        return true;
    }
#endif
    void Report(TSharedRef<FJsonObject> J)
    {
        J->SetBoolField(TEXT("passed"), Error.IsEmpty()); J->SetStringField(TEXT("error"), Error);
        J->SetBoolField(TEXT("callbacks_on_game_thread"), bGameThread); J->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
        const FString Json = Direct::Write(J), Directory = FPaths::ProjectSavedDir() / TEXT("SimurghRTC");
        IFileManager::Get().MakeDirectory(*Directory, true);
        const bool Saved = FFileHelper::SaveStringToFile(Json, *(Directory / (TEXT("direct-probe-") + RunId + TEXT(".json"))));
        UE_LOG(LogSimurghRTCDirectProbe, Display, TEXT("SIMURGHRTC_DIRECT_REPORT %s"), *Json);
        if (!Saved) UE_LOG(LogSimurghRTCDirectProbe, Error, TEXT("Could not save Direct probe report."));
        FPlatformMisc::RequestExitWithStatus(false, Error.IsEmpty() && Saved ? 0 : 5);
    }
    bool SaveConcurrentProgress(TSharedRef<FJsonObject> ProgressData)
    {
        auto J = Metadata(); J->SetObjectField(TEXT("concurrent"),ProgressData); J->SetBoolField(TEXT("passed"),Error.IsEmpty());
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("SimurghRTC"); IFileManager::Get().MakeDirectory(*Directory,true);
        const FString Json = Direct::Write(J);
        const bool Saved = FFileHelper::SaveStringToFile(Json,*(Directory / (TEXT("concurrent-progress-") + RunId + TEXT(".json"))));
        UE_LOG(LogSimurghRTCDirectProbe,Display,TEXT("SIMURGHRTC_CONCURRENT_PROGRESS %s"),*Json); return Saved;
    }
    bool Tick(float)
    {
        check(IsInGameThread());
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if (bServer)
        {
            // The sample echoes through a bounded queue; production Server has no automatic echo.
            for (int32 I = 0; Server && I < 16 && !Echoes.IsEmpty(); ++I)
            {
                const auto& E = Echoes[0]; const auto& M = E.Message;
                const auto R = M.Kind == ESimurghRTCMessageKind::Text ? Server->SendText(E.Session, M.Channel, M.Text) : Server->SendBytes(E.Session, M.Channel, M.Bytes);
                if (R.Code == ESimurghRTCErrorCode::ResourceExhausted) break;
                if (R.IsError()) { Fail(TEXT("Server echo send failed.")); break; }
                EchoBytes -= M.Bytes.Num() + M.Text.Len() * sizeof(TCHAR); Echoes.RemoveAt(0); ++EchoSent;
            }
            if (!bStopping && FPlatformTime::Seconds() - Started > ((Scenario == TEXT("concurrent") || Scenario == TEXT("shared")) ? 600 : 180)) { Fail(TEXT("Server probe deadline expired.")); bStopping = true; StopAt = FPlatformTime::Seconds(); if (Server) Server->Stop(); }
            if (!Error.IsEmpty()) { if (Server) Server->Stop(); Report(ServerReport()); return false; }
            if (bStopping && (Server->GetSnapshot().LivePeers == 0 || FPlatformTime::Seconds() - StopAt > 10))
            {
                if (Server->GetSnapshot().LivePeers) Fail(TEXT("Server native teardown timed out."));
                auto J = ServerReport(); J->SetBoolField(TEXT("teardown_complete"), !Server->GetSnapshot().LivePeers); Report(J); return false;
            }
            return true;
        }
#endif
        if (FPlatformTime::Seconds() - Started > (Scenario == TEXT("concurrent") ? 540 : Requested * 30 + 20)) Fail(TEXT("Client probe deadline expired."));
        if (!Client && Error.IsEmpty())
        {
            StepStarted = FPlatformTime::Seconds(); bClosing = false;
            FDirectClientConfig Config; Config.Endpoint = Endpoint; Config.BearerToken = Token; Config.Identity = Scenario == TEXT("denied") ? TEXT("deny") : TEXT("probe-client");
            Config.Purpose = Index % 2 ? TEXT("call") : TEXT("data"); Config.bAllowLoopbackHttp = true; Config.bLoopbackProbe = true;
            if (Scenario == TEXT("concurrent")) { Config.Identity = TEXT("shared-identity"); Config.Purpose = TEXT("text-room"); }
            Config.bEarlyIceProbe = Index % 3 == 0; Config.RequestTimeoutSeconds = Scenario == TEXT("data") ? 2 : 0.5; Config.ConnectTimeoutSeconds = Scenario == TEXT("data") ? 20 : 10;
            if (Scenario == TEXT("concurrent")) { Config.RequestTimeoutSeconds = 3; Config.ConnectTimeoutSeconds = 60; }
            Client = FDirectClient::Connect(Owner.Get(), MoveTemp(Config), [this](const FDirectClientSnapshot&) { bGameThread &= IsInGameThread(); ++CallbackCount; }, Error);
            if (!Client) Fail(TEXT("Direct client creation failed."));
        }
        if (Scenario == TEXT("concurrent") && Client)
        {
            const auto S = Client->GetSnapshot();
            if (S.State == EDirectClientState::Connected && !ConcurrentClient)
                ConcurrentClient = MakeUnique<FContractConcurrentClient>(Client.ToSharedRef(),ConcurrentRole,bReplacement,
                    [this](TSharedRef<FJsonObject> ProgressData) { return SaveConcurrentProgress(ProgressData); });
            if (ConcurrentClient && S.State == EDirectClientState::Connected) ConcurrentClient->Tick();
            bool Finished = false;
            if (ConcurrentClient) { Finished = ConcurrentClient->Finished(S); if (!ConcurrentClient->Error.IsEmpty()) Fail(ConcurrentClient->Error); }
            else if (S.State == EDirectClientState::Failed) Fail(S.Error);
            if (Finished || !Error.IsEmpty())
            {
                auto J = Metadata(); J->SetStringField(TEXT("scenario"),Scenario);
                if (ConcurrentClient) J->SetObjectField(TEXT("concurrent"),ConcurrentClient->Report());
                J->SetBoolField(TEXT("native_closed"),S.bNativeClosed); J->SetBoolField(TEXT("remote_closed"),S.bRemoteClosed);
                J->SetStringField(TEXT("close_reason"),S.Error); Report(J); return false;
            }
            return true;
        }
        if (Client)
        {
            const auto S = Client->GetSnapshot();
            if (!bClosing && (Scenario == TEXT("cancel") || Scenario == TEXT("owner-loss")) && !S.SessionId.IsEmpty())
            {
                Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("session_id"), S.SessionId); Row->SetBoolField(TEXT("cancelled"), true); bClosing = true;
                if (Scenario == TEXT("owner-loss")) { bOwnerLost = true; CallbacksAtOwnerLoss = CallbackCount; Owner.Reset(); CollectGarbage(RF_NoFlags); }
                else Client->Close();
            }
            else if (!bClosing && S.State == EDirectClientState::Connected)
            {
                if (Scenario == TEXT("data"))
                {
                    if (!Data) Data = MakeUnique<FContractDataScenario>(Client.ToSharedRef());
                    if (!Data->Tick() && Error.IsEmpty()) return true;
                    if (!Data->Error.IsEmpty()) Fail(Data->Error);
                }
                if (!ExpectedError.IsEmpty()) Fail(TEXT("Connected despite an expected failure."));
                Row = MakeShared<FJsonObject>(); Row->SetNumberField(TEXT("index"), Index); Row->SetStringField(TEXT("session_id"), S.SessionId);
                Row->SetStringField(TEXT("mode"), Index % 2 ? TEXT("media") : TEXT("data")); Row->SetBoolField(TEXT("early_ice"), Index % 3 == 0);
                Row->SetObjectField(TEXT("transport"), Transport(S.Transport)); Row->SetNumberField(TEXT("requests"), S.AcceptedRequests); Row->SetNumberField(TEXT("retries"), S.Retries);
                if (Data) { Row->SetObjectField(TEXT("data"), Data->Report()); Data.Reset(); }
                bClosing = true; Client->Close();
            }
            if (S.State == EDirectClientState::Failed)
            {
                if (ExpectedError.IsEmpty() || S.Error != ExpectedError) Fail(S.Error);
                if (S.bNativeClosed)
                {
                    Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("expected_error"), S.Error); Row->SetBoolField(TEXT("native_closed"), true);
                    Row->SetNumberField(TEXT("retries"), S.Retries); Rows.Add(MakeShared<FJsonValueObject>(Row)); ++Index; Client.Reset();
                }
            }
            else if (S.State == EDirectClientState::Closed)
            {
                if (!Row) Fail(TEXT("Client closed before a connection/cancellation was recorded."));
                else
                {
                    Row->SetBoolField(TEXT("native_closed"), S.bNativeClosed); Row->SetBoolField(TEXT("remote_closed"), S.bRemoteClosed);
                    if (!S.bNativeClosed || !S.bRemoteClosed) Fail(TEXT("Client cleanup was not confirmed."));
                    Rows.Add(MakeShared<FJsonValueObject>(Row)); Row.Reset(); ++Index;
                }
                Client.Reset();
            }
        }
        if (bOwnerLost && CallbackCount != CallbacksAtOwnerLoss) Fail(TEXT("Callback reached a destroyed owner."));
        if (!Error.IsEmpty() || Index == Requested)
        {
            if (Client) Client->Close();
            auto J = Metadata(); J->SetStringField(TEXT("scenario"), Scenario); J->SetStringField(TEXT("expected_error"), ExpectedError);
            J->SetNumberField(TEXT("requested_pairs"), Requested); J->SetNumberField(TEXT("completed_pairs"), Index); J->SetArrayField(TEXT("pairs"), Rows);
            J->SetBoolField(TEXT("owner_callback_suppressed"), !bOwnerLost || CallbackCount == CallbacksAtOwnerLoss);
            Report(J); return false;
        }
        return true;
    }
#endif
};
FContractDirectProbe::FContractDirectProbe() = default;
FContractDirectProbe::~FContractDirectProbe() = default;
void FContractDirectProbe::Startup()
{
#if !UE_BUILD_SHIPPING && SIMURGHRTC_WITH_DIRECT
    const bool Server = FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCDirectServerProbe"));
    if (!Server && !FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCDirectClientProbe"))) return;
    Impl = MakeUnique<FImpl>(); Impl->bServer = Server;
    FString TokenFile; FGuid Id;
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectRunId="), Impl->RunId);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectTokenFile="), TokenFile);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectEndpoint="), Impl->Endpoint);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectPort="), Impl->Port);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectPairs="), Impl->Requested);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectScenario="), Impl->Scenario);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCDirectExpectedError="), Impl->ExpectedError);
    FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCConcurrentCredentials="),Impl->CredentialsFile);
    FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCConcurrentRole="),Impl->ConcurrentRole);
    Impl->bReplacement = FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCConcurrentReplacement"));
    if (Impl->ConcurrentRole < 0 || Impl->ConcurrentRole > 3 || !FGuid::ParseExact(Impl->RunId, EGuidFormats::DigitsWithHyphens, Id) || Impl->Port < 1024 || Impl->Port > 65535
        || Impl->Requested < 1 || Impl->Requested > 16 || !FFileHelper::LoadFileToString(Impl->Token, *TokenFile))
    { UE_LOG(LogSimurghRTCDirectProbe, Error, TEXT("Invalid Direct probe arguments or unreadable token file.")); Impl.Reset(); return; }
    Impl->Token.TrimStartAndEndInline();
    Impl->Init = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]
    {
        Impl->Owner.Reset(NewObject<UGameInstance>()); Impl->Started = FPlatformTime::Seconds();
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if (Impl->bServer) Impl->StartServer();
#else
        if (Impl->bServer) Impl->Fail(TEXT("This target has no Server module."));
#endif
        Impl->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(Impl.Get(), &FImpl::Tick));
    });
#endif
}
void FContractDirectProbe::Shutdown() { Impl.Reset(); }
