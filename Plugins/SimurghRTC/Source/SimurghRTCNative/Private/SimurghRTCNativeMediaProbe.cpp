// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeMediaProbe.h"
#include "Fixtures/SimurghRTCMediaFixtureV1.h"
#if SIMURGHRTC_WITH_WEBRTC
#include "simurgh_rtc_abi.h"
#endif
namespace SimurghRTC
{
bool RunNativeMediaProbe(FNativeMediaProbeResult& Out, FString& OutError)
{
    check(IsInGameThread());
    Out = FNativeMediaProbeResult{};
    OutError.Reset();
#if SIMURGHRTC_WITH_WEBRTC
    static_assert(sizeof(FColor) == 4 && FixtureV1::Width == 64 && FixtureV1::Height == 64
        && FixtureV1::FrameCount == 12 && FixtureV1::SampleRate == 48000);
    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(12 * 64 * 64);
    for (int32 Frame = 0; Frame < 12; ++Frame)
        for (int32 Y = 0; Y < 64; ++Y) for (int32 X = 0; X < 64; ++X)
            Pixels[Frame * 64 * 64 + Y * 64 + X] = FixtureV1::VideoPixel(X, Y, Frame);
    TArray<int16> PCM;
    PCM.SetNumUninitialized(SR_PCM_SAMPLES);
    for (int32 N = 0; N < 48000; ++N) PCM[N * 2] = PCM[N * 2 + 1] = FixtureV1::AudioSample(N);
    SR_MediaInput Input{};
    Input.size = sizeof(Input); Input.abi_version = SR_ABI_VERSION;
    Input.bgra_frames = reinterpret_cast<const uint8*>(Pixels.GetData()); Input.bgra_bytes = Pixels.Num() * sizeof(FColor);
    Input.pcm = PCM.GetData(); Input.pcm_samples = PCM.Num();
    // Allocate the bounded output on Unreal's heap; the library only writes into it.
    auto Native = MakeUnique<SR_MediaOutput>();
    Native->size = sizeof(SR_MediaOutput); Native->abi_version = SR_ABI_VERSION;
    char Error[1024]{};
    if (SimurghRTC_MediaProbeV1(&Input, Native.Get(), Error, sizeof(Error)))
    { OutError = UTF8_TO_TCHAR(Error); return false; }
    if (Native->pcm_samples != SR_PCM_SAMPLES || Native->decoded_frames != 12)
    { OutError = TEXT("Native media output violates fixture bounds."); return false; }
    Out.EncodedFrames = Native->encoded_frames; Out.DecodedFrames = Native->decoded_frames;
    Out.AudioPackets = Native->audio_packets; Out.Channels = Native->channels;
    Out.MaxVideoPacketBytes = Native->max_video_packet_bytes; Out.MaxAudioPacketBytes = Native->max_audio_packet_bytes;
    Out.MaxColorError = Native->max_color_error; Out.ToneCorrelation = Native->tone_correlation; Out.AudioRms = Native->audio_rms;
    Out.VideoEncoder = UTF8_TO_TCHAR(Native->video_encoder); Out.VideoDecoder = UTF8_TO_TCHAR(Native->video_decoder);
    Out.DecodedBGRA.SetNumUninitialized(64 * 64);
    FMemory::Memcpy(Out.DecodedBGRA.GetData(), Native->bgra, SR_VIDEO_BYTES);
    Out.DecodedPCM.Append(Native->pcm, Native->pcm_samples);
    return true;
#else
    OutError = TEXT("The isolated native backend is not available for this target.");
    return false;
#endif
}
}
