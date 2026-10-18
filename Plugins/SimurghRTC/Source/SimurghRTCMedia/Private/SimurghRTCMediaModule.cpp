// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "SimurghRTCNativeMediaProbe.h"
#include "SimurghRTCProbeSoundWave.h"
#include "AudioThread.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogSimurghRTCMediaProbe, Log, All);

/** Opt-in feasibility runner; the shared session/media facade remains unchanged. */
class FSimurghRTCMediaModule final : public IModuleInterface
{
    FDelegateHandle InitHandle;
    FDelegateHandle ExitHandle;
    FTSTicker::FDelegateHandle TickHandle;
    TStrongObjectPtr<UTexture2D> Texture;
    TStrongObjectPtr<UTextureRenderTarget2D> Target;
    TStrongObjectPtr<USimurghRTCProbeSoundWave> Sound;
    TStrongObjectPtr<UAudioComponent> Audio;
    TSharedPtr<FJsonObject> Report;
    SimurghRTC::FNativeMediaProbeResult Media;
    FString Output;
    double Deadline = 0;
    bool bStarted = false;
    bool bFinished = false;

    void Cleanup()
    {
        if (Audio)
        {
            Audio->Stop();
            Audio->DestroyComponent();
            FAudioCommandFence Fence;
            Fence.BeginFence();
            Fence.Wait();
        }
        if (Texture || Target) FlushRenderingCommands();
        Audio.Reset();
        Sound.Reset();
        Target.Reset();
        Texture.Reset();
    }

    bool Finish(bool bPass, const FString& Error = {})
    {
        bFinished = true;
        Report->SetBoolField(TEXT("passed"), bPass);
        Report->SetStringField(TEXT("error"), Error);
        Cleanup();
        Report->SetBoolField(TEXT("resources_released"), true);
        FString Json;
        FJsonSerializer::Serialize(Report.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
        // Packaged platform-file policies can reject external ABSLOG/report paths.
        // The harness also reads this bounded, single-line report from captured stdout.
        UE_LOG(LogSimurghRTCMediaProbe, Display, TEXT("SIMURGHRTC_MEDIA_REPORT %s"), *Json);
        if (!Output.IsEmpty() && !FFileHelper::SaveStringToFile(Json, *Output))
            UE_LOG(LogSimurghRTCMediaProbe, Display, TEXT("Media report file unavailable; use structured log evidence."));
        UE_LOG(LogSimurghRTCMediaProbe, Display, TEXT("SIMURGHRTC_MEDIA_PROBE_%s %s"), bPass ? TEXT("PASSED") : TEXT("FAILED"), *Error);
        FPlatformMisc::RequestExitWithStatus(false, bPass ? 0 : 4);
        return false;
    }

    bool Capture(UWorld* World, FString& Error)
    {
        using FFixture = SimurghRTC::FNativeMediaProbeResult;
        constexpr int32 W = FFixture::Width, H = FFixture::Height;
        if (GUsingNullRHI || !GDynamicRHI || !FApp::CanEverRender())
        { Error = TEXT("Render-target probe requires a real RHI; NullRHI is not a capture pass."); return false; }
        Report->SetStringField(TEXT("rhi"), GDynamicRHI->GetName());
        Texture.Reset(UTexture2D::CreateTransient(W, H, PF_B8G8R8A8));
        if (!Texture) { Error = TEXT("Transient texture allocation failed."); return false; }
        Texture->SRGB = false;
        Texture->Filter = TF_Nearest;
        Texture->UpdateResource();
        FlushRenderingCommands();
        Target.Reset(NewObject<UTextureRenderTarget2D>());
        Target->ClearColor = FLinearColor::Black;
        Target->InitCustomFormat(W, H, PF_B8G8R8A8, true);
        Target->UpdateResourceImmediate(true);
        auto* Resource = Target->GameThread_GetRenderTargetResource();
        if (!Resource || !Texture->GetResource()) { Error = TEXT("Texture resources unavailable."); return false; }
        int32 MaxError = 0;
        // Two distinct uploads catch stale content. One upload/draw/readback is in flight.
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            TArray<FColor> Expected = Media.DecodedBGRA;
            if (Pass == 0) for (FColor& Pixel : Expected) Pixel = FColor(24, 48, 96);
            uint8* Upload = new uint8[W * H * 4];
            FMemory::Memcpy(Upload, Expected.GetData(), W * H * 4);
            auto* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
            Texture->UpdateTextureRegions(0, 1, Region, W * 4, 4, Upload,
                [](uint8* Data, const FUpdateTextureRegion2D* Regions) { delete[] Data; delete Regions; });
            FCanvas Canvas(Resource, nullptr, World, GMaxRHIFeatureLevel);
            FCanvasTileItem Tile(FVector2D::ZeroVector, Texture->GetResource(), FVector2D(W, H), FLinearColor::White);
            Tile.BlendMode = SE_BLEND_Opaque;
            Canvas.DrawItem(Tile);
            Canvas.Flush_GameThread();
            TArray<FColor> Captured;
            FReadSurfaceDataFlags Flags(RCM_UNorm);
            Flags.SetLinearToGamma(false);
            // Public plugin API. This synchronous GPU stall is only a 64x64 feasibility probe.
            if (!Resource->ReadPixels(Captured, Flags) || Captured.Num() != W * H)
            { Error = TEXT("Render target readback failed."); return false; }
            for (int32 Y : {16, 48}) for (int32 X : {16, 48})
            {
                const FColor Want = Expected[Y * W + X], Got = Captured[Y * W + X];
                MaxError = FMath::Max(MaxError, FMath::Max3(FMath::Abs(int32(Want.R) - Got.R),
                    FMath::Abs(int32(Want.G) - Got.G), FMath::Abs(int32(Want.B) - Got.B)));
            }
        }
        Report->SetNumberField(TEXT("texture_uploads"), 2);
        Report->SetNumberField(TEXT("render_target_captures"), 2);
        Report->SetNumberField(TEXT("capture_max_color_error"), MaxError);
        Report->SetNumberField(TEXT("frame_bytes"), W * H * 4);
        Report->SetNumberField(TEXT("max_in_flight_frames"), 1);
        if (MaxError > 3) { Error = TEXT("Captured texture color/orientation/stale-frame check failed."); return false; }
        return true;
    }

    bool Tick(float)
    {
        if (bFinished) return false;
        if (!bStarted)
        {
            UWorld* World = nullptr;
            if (GEngine) for (const FWorldContext& Context : GEngine->GetWorldContexts())
                if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE) World = Context.World();
            if (!World)
                return FPlatformTime::Seconds() > Deadline ? Finish(false, TEXT("Timed out waiting for a game world; use -game.")) : true;
            bStarted = true;
            FString Error;
            if (!SimurghRTC::RunNativeMediaProbe(Media, Error)) return Finish(false, Error);
            Report->SetStringField(TEXT("video_codec"), TEXT("VP8"));
            Report->SetStringField(TEXT("video_encoder"), Media.VideoEncoder);
            Report->SetStringField(TEXT("video_decoder"), Media.VideoDecoder);
            Report->SetNumberField(TEXT("encoded_frames"), Media.EncodedFrames);
            Report->SetNumberField(TEXT("decoded_frames"), Media.DecodedFrames);
            Report->SetNumberField(TEXT("codec_max_color_error"), Media.MaxColorError);
            Report->SetNumberField(TEXT("max_video_packet_bytes"), Media.MaxVideoPacketBytes);
            Report->SetStringField(TEXT("audio_codec"), TEXT("opus"));
            Report->SetNumberField(TEXT("audio_packets"), Media.AudioPackets);
            Report->SetNumberField(TEXT("max_audio_packet_bytes"), Media.MaxAudioPacketBytes);
            Report->SetNumberField(TEXT("pcm_sample_rate"), Media.SampleRate);
            Report->SetNumberField(TEXT("pcm_channels"), Media.Channels);
            Report->SetNumberField(TEXT("pcm_samples"), Media.DecodedPCM.Num());
            Report->SetNumberField(TEXT("tone_correlation"), Media.ToneCorrelation);
            Report->SetNumberField(TEXT("pcm_rms"), Media.AudioRms);
            if (!Capture(World, Error)) return Finish(false, Error);
            Sound.Reset(NewObject<USimurghRTCProbeSoundWave>());
            Sound->SetSampleRate(Media.SampleRate);
            Sound->NumChannels = Media.Channels;
            Sound->Duration = INDEFINITELY_LOOPING_DURATION;
            Sound->bLooping = false;
            Sound->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
            const int32 Bytes = Media.DecodedPCM.Num() * sizeof(int16);
            if (Bytes > 192000) return Finish(false, TEXT("PCM fixture exceeds the 192000-byte queue limit."));
            Sound->QueueAudio(reinterpret_cast<const uint8*>(Media.DecodedPCM.GetData()), Bytes);
            Report->SetNumberField(TEXT("pcm_queued_bytes"), Sound->GetAvailableAudioByteCount());
            Report->SetNumberField(TEXT("pcm_queue_limit_bytes"), 192000);
            // PlayWhenSilent keeps the real UE mixer consuming procedural audio without audible output.
            Audio.Reset(UGameplayStatics::SpawnSound2D(World, Sound.Get(), 0.0f, 1.0f, 0.0f, nullptr, false, false));
            if (!Audio) return Finish(false, TEXT("UE audio device/component unavailable; decoded PCM alone is not a mixer pass."));
            Deadline = FPlatformTime::Seconds() + 10.0;
            return true;
        }
        const int32 Consumed = Sound->ConsumedBytes.Load();
        Report->SetNumberField(TEXT("pcm_consumed_bytes"), Consumed);
        Report->SetNumberField(TEXT("audio_callbacks"), Sound->CallbackCount.Load());
        Report->SetBoolField(TEXT("audio_callback_on_game_thread"), Sound->bCallbackOnGameThread.Load());
        if (Consumed == Media.DecodedPCM.Num() * sizeof(int16) && Sound->CallbackCount.Load() > 0)
            return Finish(!Sound->bCallbackOnGameThread.Load(), Sound->bCallbackOnGameThread.Load() ? TEXT("Audio callback unexpectedly ran on game thread.") : TEXT(""));
        if (FPlatformTime::Seconds() > Deadline) return Finish(false, TEXT("UE audio mixer did not consume the PCM fixture within 10 seconds."));
        return true;
    }

    void Start()
    {
        Report = MakeShared<FJsonObject>();
        FString RunId;
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCMediaRunId="), RunId);
        FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCMediaOutput="), Output);
        Report->SetStringField(TEXT("run_id"), RunId);
        Report->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
        Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Report->SetBoolField(TEXT("editor_build"), WITH_EDITOR != 0);
        Report->SetBoolField(TEXT("network_transport_tested"), false);
        Deadline = FPlatformTime::Seconds() + 30.0;
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSimurghRTCMediaModule::Tick));
    }

public:
    virtual void StartupModule() override
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCMediaProbe"))) return;
        InitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddRaw(this, &FSimurghRTCMediaModule::Start);
        ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FSimurghRTCMediaModule::Cleanup);
    }
    virtual void ShutdownModule() override
    {
        FCoreDelegates::OnFEngineLoopInitComplete.Remove(InitHandle);
        FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        Cleanup();
    }
};

IMPLEMENT_MODULE(FSimurghRTCMediaModule, SimurghRTCMedia)
