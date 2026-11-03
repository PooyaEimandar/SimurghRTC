// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCAudioPlayback.h"
#include "SimurghRTCPlaybackSoundWave.h"
#include "SimurghRTCSession.h"
#include "SimurghRTCSessionAudioSink.h"
#include "Components/AudioComponent.h"
#include "UObject/StrongObjectPtr.h"

namespace SimurghRTC
{
namespace
{
class FAudioSink;
TMap<TWeakObjectPtr<UAudioComponent>, TWeakPtr<FAudioSink>> Bindings;
class FAudioSink final : public ISessionAudioSink
{
public:
    TStrongObjectPtr<UAudioComponent> Target;
    TStrongObjectPtr<USimurghRTCPlaybackSoundWave> Wave;
    TSharedPtr<FAudioPlaybackBuffer, ESPMode::ThreadSafe> Buffer = MakeShared<FAudioPlaybackBuffer, ESPMode::ThreadSafe>();
    FGuid SessionId, TrackId;
    bool bAutoDestroy = false;
    virtual ~FAudioSink() override { Close(); }
    virtual bool IsAlive() const override
    {
        check(IsInGameThread());
        return IsValid(Target.Get()) && Target->IsRegistered() && Target->Sound == Wave.Get() && Target->IsPlaying();
    }
    virtual void Push(const FSimurghRTCAudioFrame& Frame) override { if (!Buffer->Push(Frame)) Close(); }
    virtual void Close() override
    {
        check(IsInGameThread()); Buffer->Close();
        // Stop can dispatch application delegates, including another session Shutdown.
        auto OwnedTarget = MoveTemp(Target);
        auto OwnedWave = MoveTemp(Wave);
        if (OwnedTarget)
        {
            Bindings.Remove(OwnedTarget.Get());
            if (IsValid(OwnedTarget.Get()))
            {
                if (OwnedTarget->Sound == OwnedWave.Get())
                {
                    OwnedTarget->Stop();
                    if (IsValid(OwnedTarget.Get()) && OwnedTarget->Sound == OwnedWave.Get()) OwnedTarget->SetSound(nullptr);
                }
                if (IsValid(OwnedTarget.Get())) OwnedTarget->bAutoDestroy = bAutoDestroy;
            }
        }
    }
};
}
FSimurghRTCError BindSessionAudioPlayback(USimurghRTCSession& Session, FSimurghRTCTrackHandle Track, UAudioComponent* Target)
{
    check(IsInGameThread());
    auto Error = Session.ValidateAudioSink(Track);
    if (Error.IsError()) return Error;
    if (!IsValid(Target) || !Target->IsRegistered() || !Target->GetWorld())
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("BindAudio requires a registered audio component in a live world."));
    if (!Target->GetAudioDevice())
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("No Unreal audio device is available for playback."));
    if (const auto* Entry = Bindings.Find(Target))
    {
        auto Existing = Entry->Pin();
        if (Existing && Existing->IsAlive())
        {
            if (Existing->SessionId != Session.GetSessionId() || Existing->TrackId != Track.Id)
                return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("The audio component is already bound to another session or track."));
            return Session.SetAudioSink(Track, Existing);
        }
        if (Existing) Existing->Close();
    }
    auto Sink = MakeShared<FAudioSink>();
    Sink->SessionId = Session.GetSessionId(); Sink->TrackId = Track.Id;
    Sink->Target.Reset(Target); Sink->bAutoDestroy = Target->bAutoDestroy;
    Target->bAutoDestroy = false;
    Sink->Wave.Reset(NewObject<USimurghRTCPlaybackSoundWave>());
    Sink->Wave->Buffer = Sink->Buffer;
    Sink->Wave->SetSampleRate(48000); Sink->Wave->NumChannels = 1;
    Sink->Wave->Duration = INDEFINITELY_LOOPING_DURATION;
    Sink->Wave->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
    Target->SetSound(Sink->Wave.Get());
    if (IsValid(Target) && Target->IsRegistered() && Session.IsUsable()) Target->Play();
    if (!Sink->IsAlive())
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("Unreal could not start the audio component."));
    Error = Session.SetAudioSink(Track, Sink);
    if (Error.IsError()) return Error;
    Bindings.Add(Target, Sink);
    return {};
}
bool GetAudioPlaybackSnapshot(UAudioComponent* Target, FAudioPlaybackSnapshot& Out)
{
    check(IsInGameThread()); Out = {};
    if (const auto* Entry = Bindings.Find(Target))
        if (const auto Sink = Entry->Pin(); Sink && Sink->IsAlive()) { Out = Sink->Buffer->Snapshot(); return true; }
    return false;
}
}
