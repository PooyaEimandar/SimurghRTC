// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractHostClientProbe.h"
#include "SimurghRTCNativeRuntime.h"
#include "SimurghRTCNativeMediaProbe.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_IOS
#include <unistd.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSimurghRTCClientProbe, Log, All);

void FContractHostClientProbe::Startup()
{
#if !UE_BUILD_SHIPPING && !UE_SERVER
    if (!FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCClientProbe"))) return;
    InitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]
    {
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FContractHostClientProbe::Run), 1.0f);
    });
#endif
}

void FContractHostClientProbe::Shutdown()
{
    FCoreDelegates::GetOnPostEngineInit().Remove(InitHandle);
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
}

bool FContractHostClientProbe::Run(float)
{
    using namespace SimurghRTC;
    check(IsInGameThread());
    FString RunId;
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCClientRunId="), RunId);
    FGuid ParsedId;
    if (!FGuid::ParseExact(RunId, EGuidFormats::DigitsWithHyphens, ParsedId))
    {
        UE_LOG(LogSimurghRTCClientProbe, Error, TEXT("Client probe requires a UUID SimurghRTCClientRunId."));
        return false;
    }
    const double Started = FPlatformTime::Seconds();
    int32 Completed = 0;
    FString Error;
    auto ExerciseCycle = [&]() -> bool
    {
        FNativeRuntime Media, Data;
        auto Require = [&](bool Value, const TCHAR* Message)
        {
            if (!Value) Error = Message;
            return Value;
        };
        if (!Require(!Media.CreateProbePeerConnection(Error), TEXT("Peer created without a factory."))) return false;
        if (!Media.Start(ENativeFactoryMode::BuiltinMedia, Error)) return false;
        if (!Require(!Media.Start(ENativeFactoryMode::BuiltinMedia, Error), TEXT("Duplicate start accepted."))) return false;
        const auto MediaReady = Media.GetDiagnostics();
        if (!Require(MediaReady.bFactoryReady && MediaReady.bDummyAudioDevice && MediaReady.bAudioProcessing
            && MediaReady.AudioCodecs.Contains(TEXT("opus")) && MediaReady.VideoCodecs.Contains(TEXT("VP8")),
            TEXT("Media factory lacks expected dependencies or codecs."))) return false;
        if (!Media.CreateProbePeerConnection(Error)) return false;
        if (!Require(!Media.CreateProbePeerConnection(Error), TEXT("Duplicate peer accepted."))) return false;
        if (!Data.Start(ENativeFactoryMode::DataOnly, Error) || !Data.CreateProbePeerConnection(Error)) return false;
        const auto DataReady = Data.GetDiagnostics();
        if (!Require(DataReady.bFactoryReady && DataReady.bPeerConnectionReady && !DataReady.bDummyAudioDevice
            && !DataReady.bAudioProcessing && DataReady.AudioCodecs.IsEmpty() && DataReady.VideoCodecs.IsEmpty(),
            TEXT("Data-only factory unexpectedly constructed media."))) return false;
        Media.Stop();
        Media.Stop();
        if (!Require(!Media.GetDiagnostics().bFactoryReady && !Media.GetDiagnostics().bPeerConnectionReady
            && Data.GetDiagnostics().bPeerConnectionReady, TEXT("Overlapping runtime teardown failed."))) return false;
        Data.Stop();
        if (!Require(!Data.GetDiagnostics().bFactoryReady && !Data.GetDiagnostics().bPeerConnectionReady,
            TEXT("Data-only teardown did not clear readiness."))) return false;
        // Restart the stopped owner, then let its destructor close the final peer.
        if (!Media.Start(ENativeFactoryMode::BuiltinMedia, Error) || !Media.CreateProbePeerConnection(Error)) return false;
        Error.Reset();
        return true;
    };
    for (; Completed < 3; ++Completed)
        if (!ExerciseCycle()) break; // Stack unwinding also tears down failed cycles.

    FNativeMediaProbeResult Media;
    const bool bMediaPassed = Completed == 3 && RunNativeMediaProbe(Media, Error);
    const bool bPassed = Completed == 3 && bMediaPassed;
    auto Report = MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema"), 2);
    Report->SetStringField(TEXT("run_id"), RunId);
    Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Report->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
#if PLATFORM_IOS
    // UE's iOS process helper inherits the generic placeholder (zero).
    Report->SetNumberField(TEXT("pid"), static_cast<int32>(getpid()));
#else
    Report->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
#endif
    Report->SetBoolField(TEXT("editor_build"), WITH_EDITOR != 0);
    Report->SetBoolField(TEXT("server_build"), UE_SERVER != 0);
    Report->SetBoolField(TEXT("arm64_build"), PLATFORM_CPU_ARM_FAMILY && sizeof(void*) == 8);
#if PLATFORM_IOS && WITH_IOS_SIMULATOR
    Report->SetBoolField(TEXT("simulator_build"), true);
#else
    Report->SetBoolField(TEXT("simulator_build"), false);
#endif
    Report->SetBoolField(TEXT("with_janus"), SIMURGHRTC_WITH_JANUS != 0);
    Report->SetStringField(TEXT("artifact_manifest_sha256"), FNativeRuntime::GetMobileArtifactSHA256());
    Report->SetStringField(TEXT("native_backend"), FNativeRuntime::GetBackendVersion());
    Report->SetBoolField(TEXT("media_codecs_passed"), bMediaPassed);
    Report->SetNumberField(TEXT("decoded_video_frames"), Media.DecodedFrames);
    Report->SetNumberField(TEXT("decoded_pcm_samples"), Media.DecodedPCM.Num());
    Report->SetNumberField(TEXT("audio_packets"), Media.AudioPackets);
    Report->SetNumberField(TEXT("video_color_error"), Media.MaxColorError);
    Report->SetNumberField(TEXT("audio_tone_correlation"), Media.ToneCorrelation);
    Report->SetBoolField(TEXT("render_capture_tested"), false);
    Report->SetBoolField(TEXT("network_transport_tested"), false);
    Report->SetBoolField(TEXT("hardware_capture_tested"), false);
    Report->SetNumberField(TEXT("completed_cycles"), Completed);
    Report->SetNumberField(TEXT("factories_created"), Completed * 3);
    Report->SetNumberField(TEXT("peers_created"), Completed * 3);
    Report->SetNumberField(TEXT("explicit_teardowns"), Completed * 2);
    Report->SetNumberField(TEXT("destructor_teardowns"), Completed);
    Report->SetBoolField(TEXT("overlapping_owners_checked"), bPassed);
    Report->SetBoolField(TEXT("duplicate_calls_checked"), bPassed);
    Report->SetBoolField(TEXT("readiness_cleared_after_stop"), bPassed);
    Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
    Report->SetBoolField(TEXT("passed"), bPassed);
    Report->SetStringField(TEXT("error"), Error);
    FString Json;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("SimurghRTC");
    IFileManager::Get().MakeDirectory(*Directory, true);
    FFileHelper::SaveStringToFile(Json, *(Directory / (TEXT("client-probe-") + RunId + TEXT(".json"))));
    UE_LOG(LogSimurghRTCClientProbe, Display, TEXT("SIMURGHRTC_CLIENT_REPORT %s"), *Json);
    // Device harnesses stop only the app they launched after retaining the report.
    // Desktop automation can request exit; ordinary app startup remains untouched.
    if (FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCClientExit")))
        FPlatformMisc::RequestExitWithStatus(false, bPassed ? 0 : 5);
    return false;
}
