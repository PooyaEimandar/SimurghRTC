// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractSharedAudioProbe.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#if CONTRACTHOST_WITH_MEDIA
#include "SimurghRTCMediaOperation.h"
#include "SimurghRTCAudioPlayback.h"
#endif

UAudioComponent* UContractSharedAudioEndpoint::NewPlaybackTarget()
{
    if (GEngine) for (const auto& Context : GEngine->GetWorldContexts())
    {
        UWorld* World = Context.World();
        if (!World || !World->IsGameWorld()) continue;
        auto* Component = NewObject<UAudioComponent>(World);
        Component->bAutoActivate = false; Component->bAutoDestroy = false;
        Component->SetVolumeMultiplier(0); // Exercise the real mixer without audible fixture tones.
        Component->RegisterComponentWithWorld(World);
        return Component;
    }
    Error = TEXT("No game world for the playback fixture."); return nullptr;
}
void UContractSharedAudioEndpoint::CapturePlayback(UAudioComponent* Target, int32 MinimumSamples)
{
#if CONTRACTHOST_WITH_MEDIA
    SimurghRTC::FAudioPlaybackSnapshot S;
    if (!SimurghRTC::GetAudioPlaybackSnapshot(Target, S)) { Error = TEXT("Playback binding is unavailable."); return; }
    const double Rms = S.ConsumedSamples ? FMath::Sqrt(S.ConsumedEnergy / double(S.ConsumedSamples)) : 0;
    auto Row = MakeShared<FJsonObject>();
    Row->SetNumberField(TEXT("consumed_samples"), double(S.ConsumedSamples));
    Row->SetNumberField(TEXT("callbacks"), double(S.Callbacks));
    Row->SetNumberField(TEXT("peak_queued_samples"), S.PeakQueuedSamples);
    Row->SetNumberField(TEXT("dropped_samples"), double(S.DroppedSamples));
    Row->SetNumberField(TEXT("silence_samples"), double(S.SilenceSamples));
    Row->SetNumberField(TEXT("rms"), Rms);
    Row->SetBoolField(TEXT("callback_on_game_thread"), S.bCallbackOnGameThread);
    PlaybackMeasurements.Add(MakeShared<FJsonValueObject>(Row));
    if (S.ConsumedSamples < uint64(MinimumSamples) || S.Callbacks == 0 || S.bCallbackOnGameThread || Rms < 0.1
        || S.PeakQueuedSamples > S.CapacitySamples || S.bClosed) Error = TEXT("The Unreal mixer did not consume valid bounded decoded PCM.");
#endif
}
bool UContractSharedAudioEndpoint::CheckPlaybackLifecycle()
{
#if CONTRACTHOST_WITH_MEDIA
    const double Now = FPlatformTime::Seconds();
    if (PlaybackStage == 0)
    {
        CapturePlayback(PlaybackB, 48000);
        PlaybackB->DestroyComponent(); QuietStarted = Now; PlaybackStage = 1;
    }
    else if (PlaybackStage == 1 && Now - QuietStarted > 0.25)
    {
        SimurghRTC::FAudioPlaybackSnapshot S;
        if (SimurghRTC::GetAudioPlaybackSnapshot(PlaybackB, S)) Error = TEXT("Destroyed playback component retained its binding.");
        PlaybackC = NewPlaybackTarget();
        Op(USimurghRTCMediaOperation::BindAudio(Session, Remote, PlaybackC), [this](const auto&)
        { PlaybackStage = 2; QuietStarted = FPlatformTime::Seconds(); });
    }
    else if (PlaybackStage == 2 && Now - QuietStarted > 0.6)
    {
        CapturePlayback(PlaybackC, 4800); ++PlaybackChecks; PlaybackStage = 3;
    }
    return PlaybackStage == 3;
#else
    Error = TEXT("Client playback module is excluded."); return false;
#endif
}
void UContractSharedAudioEndpoint::ReleasePlayback()
{
    for (auto* Component : {PlaybackA.Get(), PlaybackB.Get(), PlaybackC.Get()})
        if (IsValid(Component) && Component->IsRegistered()) Component->DestroyComponent();
    PlaybackA = nullptr; PlaybackB = nullptr; PlaybackC = nullptr;
}
