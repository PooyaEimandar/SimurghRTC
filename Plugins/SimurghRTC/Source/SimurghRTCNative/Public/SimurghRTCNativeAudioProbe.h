// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
namespace SimurghRTC
{
struct FNativeAudioProbeResult
{
    bool bPassed = false;
    FString Error;
    int32 Pairs = 0, PeersClosed = 0;
    uint64 DecodedSamples = 0;
    double MinimumCorrelation = 1, MaximumMutedRms = 0, Seconds = 0;
};
// Bounded integration fixture, called on a dedicated worker, never the game
// thread. Uses loopback ICE and generated PCM; no devices or public session API.
SIMURGHRTCNATIVE_API FNativeAudioProbeResult RunNativeAudioTransportProbe();
}
