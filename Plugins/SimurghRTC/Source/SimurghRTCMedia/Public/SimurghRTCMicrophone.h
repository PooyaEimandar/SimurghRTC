// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
namespace SimurghRTC
{
// Process-local diagnostics. No device names or recorded PCM are retained here.
struct FMicrophoneSnapshot
{
    int32 Workers = 0;
    uint64 Opened = 0, Closed = 0, Callbacks = 0, InputFrames = 0, DroppedSamples = 0;
    int32 PeakQueuedSamples = 0;
    bool bCallbackOnGameThread = false;
};
SIMURGHRTCMEDIA_API FMicrophoneSnapshot GetMicrophoneSnapshot();
}
