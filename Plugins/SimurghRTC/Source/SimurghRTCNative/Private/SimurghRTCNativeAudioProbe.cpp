// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeAudioProbe.h"
#include "SimurghRTCNativeRuntime.h"
#include "HAL/PlatformProcess.h"

namespace SimurghRTC
{
namespace
{
struct FTone
{
    double Energy = 0, ToneEnergy = 0; uint64 Samples = 0;
    void Add(const FSimurghRTCAudioFrame& Frame, double Frequency)
    {
        double Sine = 0, Cosine = 0;
        const int32 Frames = Frame.Samples.Num() / Frame.Channels;
        for (int32 I = 0; I < Frames; ++I)
        {
            const double Value = Frame.Samples[I * Frame.Channels], Phase = 2 * PI * Frequency * I / Frame.SampleRate;
            Energy += Value * Value; Sine += Value * FMath::Sin(Phase); Cosine += Value * FMath::Cos(Phase);
        }
        Samples += Frames; ToneEnergy += 2 * (Sine * Sine + Cosine * Cosine) / Frames;
    }
    double Correlation() const { return Energy > 0 ? FMath::Sqrt(ToneEnergy / Energy) : 0; }
    double Rms() const { return Samples ? FMath::Sqrt(Energy / Samples) : 1; }
};
struct FEndpoint
{
    FNativeRuntime Runtime;
    uint64 NextId = 0;
    FNativeTransportSnapshot Transport;
    TMap<uint64, FNativeSignalResult> Completed;
    int64 LastAudioTimestamp = -1;
};
class FAudioPair
{
public:
    FEndpoint Ends[2]; FString Error; double Deadline = FPlatformTime::Seconds() + 35;
    bool Require(bool Value, const TCHAR* Message) { if (!Value && Error.IsEmpty()) Error = Message; return Value; }
    uint64 Send(int32 Side, FNativeSignalRequest Request)
    {
        auto& End = Ends[Side]; FNativeSignalResult Failure; const uint64 Id = ++End.NextId;
        if (!End.Runtime.Signal(Id, Request, Failure)) Error = Failure.Message;
        return Id;
    }
    bool Poll()
    {
        for (int32 Side = 0; Side < 2; ++Side) for (int32 N = 0; N < 32; ++N)
        {
            FNativeTransportEvent Event;
            if (!Require(Ends[Side].Runtime.PollSignal(Event), TEXT("Native audio signaling poll failed."))) return false;
            Ends[Side].Transport = Event.Transport;
            if (Event.Type == ENativeSignalEvent::None) break;
            if (Event.Type == ENativeSignalEvent::FatalError || !Event.Result.IsSuccess()) { Error = Event.Result.Message; return false; }
            if (Event.Type == ENativeSignalEvent::OperationComplete) Ends[Side].Completed.Add(Event.Result.OperationId, Event.Result);
            if (Event.Type == ENativeSignalEvent::LocalIce || Event.Type == ENativeSignalEvent::LocalIceComplete)
            {
                FNativeSignalRequest Request; Request.Action = Event.Type == ENativeSignalEvent::LocalIce ? ENativeSignalAction::AddIce : ENativeSignalAction::EndIce;
                Request.Ice = Event.Ice; Send(1 - Side, MoveTemp(Request));
            }
        }
        return Error.IsEmpty();
    }
    bool Wait(TFunctionRef<bool()> Ready)
    {
        while (Error.IsEmpty() && FPlatformTime::Seconds() < Deadline)
        {
            if (!Poll()) return false;
            if (Ready()) return true;
            FPlatformProcess::SleepNoStats(.001f);
        }
        return Require(false, TEXT("Audio transport probe timed out."));
    }
    bool Negotiate(int32 Rate, int32 Channels)
    {
        for (auto& End : Ends)
        {
            if (!End.Runtime.Start(ENativeFactoryMode::BuiltinMedia, Error) || !End.Runtime.CreateProbePeerConnection(Error, true)) return false;
            if (!Require(End.Runtime.ConfigureAudio(Rate, Channels) == ENativeSignalError::None, TEXT("Native PCM configuration failed."))) return false;
        }
        FNativeSignalRequest Request;
        uint64 Id = Send(0, Request);
        if (!Wait([&] { return Ends[0].Completed.Contains(Id); })) return false;
        Request.Action = ENativeSignalAction::SetRemote; Request.SdpType = ENativeSdpType::Offer; Request.Sdp = Ends[0].Completed[Id].Sdp;
        if (!Require(Request.Sdp.Contains(TEXT("m=audio")) && Request.Sdp.Contains(TEXT("opus/48000/2")), TEXT("Opus audio absent from SDP."))) return false;
        Id = Send(1, Request); if (!Wait([&] { return Ends[1].Completed.Contains(Id); })) return false;
        Request = {}; Request.Action = ENativeSignalAction::CreateAnswer;
        Id = Send(1, Request); if (!Wait([&] { return Ends[1].Completed.Contains(Id); })) return false;
        Request.Action = ENativeSignalAction::SetRemote; Request.SdpType = ENativeSdpType::Answer; Request.Sdp = Ends[1].Completed[Id].Sdp;
        Send(0, Request);
        return Wait([&] { return Ends[0].Transport.Connection == ENativeConnectionState::Connected && Ends[1].Transport.Connection == ENativeConnectionState::Connected
            && Ends[0].Transport.bDataTransportOpen && Ends[1].Transport.bDataTransportOpen; });
    }
    bool Stream(int32 Rate, int32 Channels, FNativeAudioProbeResult& Result)
    {
        FTone Steady[2], Muted[2], Resumed[2];
        const double Started = FPlatformTime::Seconds(); double Next = Started;
        uint64 FrameNumber = 1; bool bMuted = false, bResumed = false;
        while (FPlatformTime::Seconds() - Started < 7 && Error.IsEmpty())
        {
            if (!Poll()) return false;
            const double Now = FPlatformTime::Seconds(), Elapsed = Now - Started;
            if (Elapsed >= 3.5 && !bMuted) { if (!Require(Ends[0].Runtime.MuteAudio(true) == ENativeSignalError::None, TEXT("Mute failed."))) return false; bMuted = true; }
            if (Elapsed >= 5 && !bResumed) { if (!Require(Ends[0].Runtime.MuteAudio(false) == ENativeSignalError::None, TEXT("Unmute failed."))) return false; bResumed = true; }
            if (Now >= Next)
            {
                for (int32 Side = 0; Side < 2; ++Side)
                {
                    FSimurghRTCAudioFrame Frame; Frame.SampleRate = Rate; Frame.Channels = Channels; Frame.TimestampMicroseconds = FrameNumber * 10000;
                    Frame.Samples.SetNumUninitialized(Rate / 100 * Channels);
                    for (int32 I = 0; I < Frame.Samples.Num(); ++I)
                        Frame.Samples[I] = .25f * FMath::Sin(2 * PI * (Side ? 800 : 400) * double(FrameNumber * (Rate / 100) + I / Channels) / Rate);
                    if (!Require(Ends[Side].Runtime.PushAudio(Frame) == ENativeSignalError::None, TEXT("PCM submission failed."))) return false;
                    for (float& Value : Frame.Samples) Value = 0; // Caller mutation must not change admitted samples.
                }
                ++FrameNumber; Next += .01; if (Next < Now - .02) Next = Now + .01;
            }
            for (int32 Side = 0; Side < 2; ++Side) for (int32 N = 0; N < 20; ++N)
            {
                FSimurghRTCAudioFrame Frame;
                if (!Require(Ends[Side].Runtime.PollAudio(Frame) == ENativeSignalError::None, TEXT("PCM receive failed."))) return false;
                if (Frame.Samples.IsEmpty()) break;
                if (!Require(Frame.TimestampMicroseconds > Ends[Side].LastAudioTimestamp && Frame.SampleRate == 48000, TEXT("Decoded PCM clock or rate differs."))) return false;
                Ends[Side].LastAudioTimestamp = Frame.TimestampMicroseconds;
                const double Frequency = Side ? 400 : 800;
                if (Elapsed >= 1.5 && Elapsed < 3.5) Steady[Side].Add(Frame, Frequency);
                if (Elapsed >= 4.3 && Elapsed < 5) Muted[Side].Add(Frame, Frequency);
                if (Elapsed >= 5.8) Resumed[Side].Add(Frame, Frequency);
                Result.DecodedSamples += Frame.Samples.Num();
            }
            FPlatformProcess::SleepNoStats(.001f);
        }
        for (int32 Side = 0; Side < 2; ++Side)
        {
            if (!Require(Steady[Side].Samples >= 48000 && Steady[Side].Correlation() > .9 && Steady[Side].Rms() > .1
                && Resumed[Side].Samples >= 24000 && Resumed[Side].Correlation() > .9, TEXT("Remote generated tone did not survive Opus transport."))) return false;
            Result.MinimumCorrelation = FMath::Min(Result.MinimumCorrelation, FMath::Min(Steady[Side].Correlation(), Resumed[Side].Correlation()));
            FNativeAudioState State;
            if (!Require(Ends[Side].Runtime.AudioState(State) == ENativeSignalError::None && State.bRemoteTrack
                && State.AdmittedBlocks == FrameNumber - 1 && State.QueuedInputBlocks <= 20 && State.QueuedOutputBlocks <= 20, TEXT("Audio queue/track evidence differs."))) return false;
        }
        Result.MaximumMutedRms = FMath::Max(Result.MaximumMutedRms, Muted[1].Rms());
        return Require(Muted[1].Samples >= 24000 && Muted[1].Rms() < .002 && Muted[0].Correlation() > .9, TEXT("Mute affected the wrong audio direction or leaked samples."));
    }
};
}
FNativeAudioProbeResult RunNativeAudioTransportProbe()
{
    check(!IsInGameThread());
    FNativeAudioProbeResult Result; const double Started = FPlatformTime::Seconds();
    for (int32 Round = 0; Round < 2; ++Round)
    {
        FAudioPair Pair; const int32 Rate = Round ? 44100 : 48000, Channels = Round ? 2 : 1;
        const bool bPassed = Pair.Negotiate(Rate, Channels) && Pair.Stream(Rate, Channels, Result);
        for (auto& End : Pair.Ends) { End.Runtime.Stop(); ++Result.PeersClosed; }
        if (!bPassed) { Result.Error = Pair.Error; Result.Seconds = FPlatformTime::Seconds() - Started; return Result; }
        ++Result.Pairs;
    }
    Result.Seconds = FPlatformTime::Seconds() - Started; Result.bPassed = true; return Result;
}
}
