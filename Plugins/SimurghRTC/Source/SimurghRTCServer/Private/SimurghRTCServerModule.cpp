// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "SimurghRTCNativeRuntime.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSimurghRTCProbe, Log, All);

namespace
{
struct FProbeHttpState
{
    FString Body;
    int32 Requests = 0;
    bool bActive = true;
};
}

/** Opt-in, bounded diagnostic process. Not the production Direct signaling service. */
class FSimurghRTCServerModule final : public IModuleInterface
{
public:
    void StartupModule() override
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCProbe"))) return;
        InitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddRaw(this, &FSimurghRTCServerModule::StartProbe);
        ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FSimurghRTCServerModule::StopProbe);
    }

    void ShutdownModule() override
    {
        FCoreDelegates::OnFEngineLoopInitComplete.Remove(InitHandle);
        FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
        StopProbe();
    }

private:
    void Fail(const FString& Error)
    {
        UE_LOG(LogSimurghRTCProbe, Error, TEXT("SIMURGHRTC_PROBE_FAILED: %s"), *Error);
        StopProbe();
        FPlatformMisc::RequestExitWithStatus(false, 2);
    }

    void StartProbe()
    {
        check(IsInGameThread());
        if (!IsRunningDedicatedServer() || FApp::CanEverRender())
        { Fail(TEXT("Run a Server executable, or the editor with -server -NullRHI -nosound.")); return; }
        int32 Port = 18777;
        int32 Seconds = 15;
        FString RunId;
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCProbePort="), Port);
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCProbeSeconds="), Seconds);
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCProbeRunId="), RunId);
        if (Port < 1024 || Port > 65535 || Seconds < 1 || Seconds > 3600 || RunId.Len() > 64)
        { Fail(TEXT("Probe port must be 1024..65535, duration 1..3600 seconds, and run ID at most 64 characters.")); return; }
        // HTTPServer only offers module-wide start/stop. Never take ownership of another service's listeners.
        if (FHttpServerModule::IsAvailable())
        { Fail(TEXT("Probe needs an isolated process with no existing HTTPServer module owner.")); return; }
        const bool bDataOnly = FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCProbeDataOnly"));
        Runtime = MakeUnique<SimurghRTC::FNativeRuntime>();
        FString Error;
        if (!Runtime->Start(bDataOnly ? SimurghRTC::ENativeFactoryMode::DataOnly : SimurghRTC::ENativeFactoryMode::BuiltinMedia, Error)
            || !Runtime->CreateProbePeerConnection(Error))
        { Fail(Error); return; }

        const auto Diagnostics = Runtime->GetDiagnostics();
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("status"), TEXT("ready"));
        Json->SetStringField(TEXT("run_id"), RunId);
        Json->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
        Json->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Json->SetStringField(TEXT("webrtc_adapter"), SimurghRTC::FNativeRuntime::GetBackendVersion());
        Json->SetStringField(TEXT("factory_mode"), bDataOnly ? TEXT("data-only") : TEXT("builtin-media"));
        Json->SetBoolField(TEXT("compiled_server_target"), UE_SERVER != 0);
        Json->SetBoolField(TEXT("dedicated_runtime"), IsRunningDedicatedServer());
        Json->SetBoolField(TEXT("can_ever_render"), FApp::CanEverRender());
        Json->SetBoolField(TEXT("factory_ready"), Diagnostics.bFactoryReady);
        Json->SetBoolField(TEXT("peer_connection_ready"), Diagnostics.bPeerConnectionReady);
        Json->SetBoolField(TEXT("dummy_audio_device"), Diagnostics.bDummyAudioDevice);
        Json->SetBoolField(TEXT("audio_processing"), Diagnostics.bAudioProcessing);
        Json->SetBoolField(TEXT("rtc_session_connected"), false);
        auto Codecs = [](const TArray<FString>& Names)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const auto& Name : Names) Values.Add(MakeShared<FJsonValueString>(Name));
            return Values;
        };
        Json->SetArrayField(TEXT("audio_codecs"), Codecs(Diagnostics.AudioCodecs));
        Json->SetArrayField(TEXT("video_codecs"), Codecs(Diagnostics.VideoCodecs));
        HttpState = MakeShared<FProbeHttpState>();
        FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&HttpState->Body));

        // In-memory, per-port override; never write user's engine/project config to disk.
        GConfig->GetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), PreviousOverrides, GEngineIni);
        auto Overrides = PreviousOverrides;
        Overrides.Insert(FString::Printf(TEXT("(Port=%d,BindAddress=127.0.0.1,ReuseAddressAndPort=false)"), Port), 0);
        GConfig->SetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), Overrides, GEngineIni);
        bConfigOverridden = true;
        auto& Http = FHttpServerModule::Get();
        bOwnsHttpListeners = true;
        // Enable first: GetHttpRouter otherwise does not attempt to bind, even with fail-on-bind.
        Http.StartAllListeners();
        Router = Http.GetHttpRouter(Port, true);
        if (!Router) { Fail(TEXT("HTTPServer could not bind the probe port.")); return; }
        const auto State = HttpState.ToSharedRef();
        Route = Router->BindRoute(FHttpPath(TEXT("/simurghrtc/probe/health")), EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([State](const FHttpServerRequest&, const FHttpResultCallback& Complete)
            {
                if (!State->bActive)
                    Complete(FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail));
                else
                {
                    ++State->Requests;
                    Complete(FHttpServerResponse::Create(State->Body, TEXT("application/json")));
                }
                return true;
            }));
        if (!Route) { Fail(TEXT("Could not bind the HTTP health route.")); return; }
        Deadline = FPlatformTime::Seconds() + Seconds;
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSimurghRTCServerModule::TickProbe));
        UE_LOG(LogSimurghRTCProbe, Display, TEXT("SIMURGHRTC_PROBE_READY http://127.0.0.1:%d/simurghrtc/probe/health compiled_server=%d mode=%s"),
            Port, UE_SERVER, bDataOnly ? TEXT("data-only") : TEXT("builtin-media"));
    }

    bool TickProbe(float)
    {
        if (FPlatformTime::Seconds() < Deadline) return true;
        const bool bObserved = HttpState && HttpState->Requests > 0;
        TickHandle.Reset();
        if (!bObserved) UE_LOG(LogSimurghRTCProbe, Error, TEXT("No HTTP health request reached the probe."));
        StopProbe();
        FPlatformMisc::RequestExitWithStatus(false, bObserved ? 0 : 3);
        return false;
    }

    void StopProbe()
    {
        check(IsInGameThread());
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
        if (HttpState) HttpState->bActive = false;
        if (Router && Route) Router->UnbindRoute(Route);
        Route.Reset();
        Router.Reset();
        if (bOwnsHttpListeners && FHttpServerModule::IsAvailable()) FHttpServerModule::Get().StopAllListeners();
        bOwnsHttpListeners = false;
        if (bConfigOverridden && GConfig)
            GConfig->SetArray(TEXT("HTTPServer.Listeners"), TEXT("ListenerOverrides"), PreviousOverrides, GEngineIni);
        bConfigOverridden = false;
        if (Runtime)
        {
            Runtime.Reset(); // Close peers/factory, join all RTC threads, then release SSL.
            UE_LOG(LogSimurghRTCProbe, Display, TEXT("SIMURGHRTC_PROBE_STOPPED requests=%d"), HttpState ? HttpState->Requests : 0);
        }
        HttpState.Reset();
    }

    TUniquePtr<SimurghRTC::FNativeRuntime> Runtime;
    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle Route;
    TSharedPtr<FProbeHttpState> HttpState;
    TArray<FString> PreviousOverrides;
    FDelegateHandle InitHandle;
    FDelegateHandle ExitHandle;
    FTSTicker::FDelegateHandle TickHandle;
    double Deadline = 0;
    bool bOwnsHttpListeners = false;
    bool bConfigOverridden = false;
};

IMPLEMENT_MODULE(FSimurghRTCServerModule, SimurghRTCServer)
