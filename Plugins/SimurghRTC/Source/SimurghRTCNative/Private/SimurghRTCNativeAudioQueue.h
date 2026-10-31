// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCNativeRuntime.h"

namespace SimurghRTC
{
// Protected by the peer mutex. No callbacks or UObject access under this lock.
struct FNativeAudioQueue
{
    static constexpr int32 MaxSamples = 9600, MaxOutputBlocks = 20;
    bool bConfigured = false, bMuted = true, bRemote = false;
    uint64 Revision = 0;
    TArray<float> Input;
    TArray<FSimurghRTCAudioFrame> Output;
    FSimurghRTCError Push(const FSimurghRTCAudioFrame& Frame);
    void Mute(bool Muted) { Input.Reset(); bMuted = Muted; ++Revision; }
    void Reset() { *this = {}; }
    bool Pump(FNativeRuntime& Runtime, uint64& AppliedRevision, int64& Time);
};
}
