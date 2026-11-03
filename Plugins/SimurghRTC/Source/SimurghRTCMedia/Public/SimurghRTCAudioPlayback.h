// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
class UAudioComponent;

namespace SimurghRTC
{
// Internal diagnostics for mixer/queue validation, not end-to-end latency or AEC statistics.
struct FAudioPlaybackSnapshot
{
    static constexpr int32 CapacitySamples = 9600; // 200 ms, 48 kHz mono.
    int32 QueuedSamples = 0, PeakQueuedSamples = 0;
    uint64 ConsumedSamples = 0, DroppedSamples = 0, SilenceSamples = 0, Callbacks = 0;
    double ConsumedEnergy = 0;
    bool bClosed = false, bCallbackOnGameThread = false;
};
SIMURGHRTCMEDIA_API bool GetAudioPlaybackSnapshot(UAudioComponent* Target, FAudioPlaybackSnapshot& Out);
}
