// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCAudioPlayback.h"
#include "SimurghRTCTypes.h"
#include "Containers/StaticArray.h"
#include "Misc/ScopeLock.h"

namespace SimurghRTC
{
// Fixed storage. The mixer only copies samples under a short lock; it never calls a session/UObject.
class FAudioPlaybackBuffer
{
    mutable FCriticalSection Mutex;
    TStaticArray<int16, FAudioPlaybackSnapshot::CapacitySamples> Samples;
    int32 Read = 0;
    FAudioPlaybackSnapshot State;
public:
    bool Push(const FSimurghRTCAudioFrame& Frame)
    {
        if (ValidateAudioFrame(Frame).IsError() || Frame.SampleRate != 48000 || Frame.Channels != 1) return false;
        FScopeLock Lock(&Mutex);
        if (State.bClosed) return false;
        const int32 Drop = FMath::Max(0, State.QueuedSamples + Frame.Samples.Num() - State.CapacitySamples);
        Read = (Read + Drop) % State.CapacitySamples;
        State.QueuedSamples -= Drop; State.DroppedSamples += Drop;
        for (const float Value : Frame.Samples)
        {
            const int32 Write = (Read + State.QueuedSamples++) % State.CapacitySamples;
            Samples[Write] = int16(FMath::Clamp(FMath::RoundToInt(Value * 32768.0f), -32768, 32767));
        }
        State.PeakQueuedSamples = FMath::Max(State.PeakQueuedSamples, State.QueuedSamples);
        return true;
    }
    void Pop(int16* Out, int32 Count)
    {
        if (Count <= 0) return;
        FMemory::Memzero(Out, Count * sizeof(int16));
        FScopeLock Lock(&Mutex);
        ++State.Callbacks; State.bCallbackOnGameThread |= IsInGameThread();
        const int32 Take = FMath::Min(Count, State.QueuedSamples);
        for (int32 I = 0; I < Take; ++I)
        {
            Out[I] = Samples[Read]; Read = (Read + 1) % State.CapacitySamples;
            const double Value = double(Out[I]) / 32768.0;
            State.ConsumedEnergy += Value * Value;
        }
        State.QueuedSamples -= Take; State.ConsumedSamples += Take; State.SilenceSamples += Count - Take;
    }
    void Close() { FScopeLock Lock(&Mutex); State.bClosed = true; State.QueuedSamples = 0; Read = 0; }
    FAudioPlaybackSnapshot Snapshot() const { FScopeLock Lock(&Mutex); return State; }
};
}
