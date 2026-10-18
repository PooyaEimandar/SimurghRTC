// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

namespace SimurghRTC
{
/** Fixed, local codec fixture. No devices, PeerConnection, RTP, or negotiated capabilities. */
struct FNativeMediaProbeResult
{
    static constexpr int32 Width = 64;
    static constexpr int32 Height = 64;
    static constexpr int32 FrameCount = 12;
    static constexpr int32 SampleRate = 48000;
    int32 EncodedFrames = 0;
    int32 DecodedFrames = 0;
    int32 AudioPackets = 0;
    int32 Channels = 0;
    int32 MaxVideoPacketBytes = 0;
    int32 MaxAudioPacketBytes = 0;
    int32 MaxColorError = 0;
    double ToneCorrelation = 0;
    double AudioRms = 0;
    FString VideoEncoder;
    FString VideoDecoder;
    TArray<FColor> DecodedBGRA; // One latest frame, never a growing frame queue.
    TArray<int16> DecodedPCM; // At most one second of 48 kHz stereo signed PCM16.
};

/** Bounded, blocking feasibility test on a private codec worker; call only on the game thread. */
SIMURGHRTCNATIVE_API bool RunNativeMediaProbe(FNativeMediaProbeResult& Out, FString& OutError);
}
