// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractAudioProbe.h"
#include "SimurghRTCNativeAudioProbe.h"
#include "SimurghRTCNativeRuntime.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
DEFINE_LOG_CATEGORY_STATIC(LogSimurghRTCAudioProbe, Log, All);

class FContractAudioProbe::FImpl
{
public:
    FDelegateHandle Init;
    FTSTicker::FDelegateHandle Tick;
    TFuture<SimurghRTC::FNativeAudioProbeResult> Work;
    FString RunId;
    int32 Heartbeats = 0;
    double PreviousTick = 0, MaximumGap = 0;
    ~FImpl()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(Init); FTSTicker::GetCoreTicker().RemoveTicker(Tick);
        if (Work.IsValid()) Work.Wait(); // Bounded worker owns and closes every native runtime.
    }
    void Begin()
    {
        PreviousTick = FPlatformTime::Seconds();
        Work = Async(EAsyncExecution::Thread, [] { return SimurghRTC::RunNativeAudioTransportProbe(); });
        Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FImpl::Update));
    }
    bool Update(float)
    {
        const double Now = FPlatformTime::Seconds(); MaximumGap = FMath::Max(MaximumGap, Now - PreviousTick); PreviousTick = Now; ++Heartbeats;
        if (!Work.IsReady()) return true;
        const auto Result = Work.Get();
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("schema"), 1); J->SetStringField(TEXT("run_id"), RunId);
        J->SetBoolField(TEXT("passed"), Result.bPassed && Heartbeats >= 30 && MaximumGap < 2);
        J->SetStringField(TEXT("error"), Result.Error);
        J->SetNumberField(TEXT("pairs"), Result.Pairs); J->SetNumberField(TEXT("peers_closed"), Result.PeersClosed);
        J->SetNumberField(TEXT("decoded_samples"), double(Result.DecodedSamples));
        J->SetNumberField(TEXT("minimum_correlation"), Result.MinimumCorrelation); J->SetNumberField(TEXT("maximum_muted_rms"), Result.MaximumMutedRms);
        J->SetNumberField(TEXT("seconds"), Result.Seconds); J->SetNumberField(TEXT("heartbeats"), Heartbeats); J->SetNumberField(TEXT("max_tick_gap_seconds"), MaximumGap);
        J->SetBoolField(TEXT("server_build"), UE_SERVER != 0); J->SetBoolField(TEXT("editor_build"), WITH_EDITOR != 0);
        J->SetBoolField(TEXT("with_janus"), SIMURGHRTC_WITH_JANUS != 0);
        J->SetBoolField(TEXT("loopback_only"), true); J->SetBoolField(TEXT("audio_hardware_opened"), false);
        J->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        J->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
        J->SetStringField(TEXT("artifact_manifest_sha256"), SimurghRTC::FNativeRuntime::GetMobileArtifactSHA256());
        J->SetStringField(TEXT("native_backend"), SimurghRTC::FNativeRuntime::GetBackendVersion());
        FString Json; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json); FJsonSerializer::Serialize(J, Writer);
        UE_LOG(LogSimurghRTCAudioProbe, Display, TEXT("SIMURGHRTC_AUDIO_REPORT %s"), *Json);
        FPlatformMisc::RequestExitWithStatus(false, J->GetBoolField(TEXT("passed")) ? 0 : 1); return false;
    }
};
FContractAudioProbe::FContractAudioProbe() = default;
FContractAudioProbe::~FContractAudioProbe() = default;
void FContractAudioProbe::Startup()
{
#if !UE_BUILD_SHIPPING
    if (!FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCAudioProbe"))) return;
    Impl = MakeUnique<FImpl>();
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCAudioProbeRunId="), Impl->RunId);
    Impl->Init = FCoreDelegates::GetOnPostEngineInit().AddRaw(Impl.Get(), &FImpl::Begin);
#endif
}
void FContractAudioProbe::Shutdown() { Impl.Reset(); }
