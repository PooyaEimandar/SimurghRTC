// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCTypes.h"
#include "Misc/ScopeLock.h"

namespace SimurghRTC
{
// Fixed 200 ms mono ring. Conversion accepts common input rates up to 48 kHz;
// keeping the output rate at least as high avoids an unfiltered downsample.
class FMicrophoneBuffer
{
public:
    static constexpr int32 Capacity = 9600;
    void SetActive(bool Value)
    {
        FScopeLock Guard(&Mutex);
        if (bActive == Value) return;
        bActive = Value; ResetQueue();
    }
    void Close() { FScopeLock Guard(&Mutex); bClosed = true; bActive = false; ResetQueue(); }
    bool Push(const float* Data, int32 Frames, int32 Channels, int32 Rate)
    {
        FScopeLock Guard(&Mutex);
        if (bClosed) return true;
        if (!Data || Frames <= 0 || Frames > 8192 || Channels < 1 || Channels > 8
            || (Rate != 8000 && Rate != 16000 && Rate != 32000 && Rate != 44100 && Rate != 48000)) return false;
        ++Callbacks; InputFrames += Frames; bOnGameThread |= IsInGameThread();
        if (!bActive) return true;
        if (InputRate && InputRate != Rate) ResetQueue();
        InputRate = Rate;
        for (int32 I = 0; I < Frames; ++I)
        {
            float Value = 0;
            for (int32 C = 0; C < Channels; ++C)
            {
                const float V = Data[I * Channels + C];
                Value += FMath::IsFinite(V) ? FMath::Clamp(V, -1.0f, 1.0f) / Channels : 0;
            }
            if (!bPrevious) { Write(Value); Previous = Value; bPrevious = true; NextOutput = Rate; InputIndex = 0; continue; }
            ++InputIndex;
            while (NextOutput <= InputIndex * 48000)
            {
                const float Alpha = float(NextOutput - (InputIndex - 1) * 48000) / 48000;
                Write(FMath::Lerp(Previous, Value, Alpha)); NextOutput += Rate;
            }
            Previous = Value;
        }
        return true;
    }
    bool Poll(FSimurghRTCAudioFrame& Out)
    {
        FScopeLock Guard(&Mutex);
        if (bClosed || !bActive || Count < 480) return false;
        Out = {}; Out.TimestampMicroseconds = int64(Produced - Count) * 1000000 / 48000;
        Out.Samples.SetNumUninitialized(480);
        for (float& V : Out.Samples) { V = Samples[Read]; Read = (Read + 1) % Capacity; --Count; }
        return true;
    }
    void Snapshot(uint64& OutCallbacks, uint64& OutFrames, uint64& OutDropped, int32& OutPeak, bool& OutOnGameThread) const
    {
        FScopeLock Guard(&Mutex); OutCallbacks = Callbacks; OutFrames = InputFrames;
        OutDropped = Dropped; OutPeak = Peak; OutOnGameThread = bOnGameThread;
    }
private:
    void ResetQueue() { Read = Count = InputRate = 0; bPrevious = false; InputIndex = NextOutput = 0; }
    void Write(float Value)
    {
        if (Count == Capacity) { Read = (Read + 1) % Capacity; --Count; ++Dropped; }
        Samples[(Read + Count) % Capacity] = Value; ++Count; ++Produced; Peak = FMath::Max(Peak, Count);
    }
    mutable FCriticalSection Mutex;
    float Samples[Capacity] = {}, Previous = 0;
    int32 Read = 0, Count = 0, Peak = 0, InputRate = 0;
    uint64 Produced = 0, Callbacks = 0, InputFrames = 0, Dropped = 0;
    int64 InputIndex = 0, NextOutput = 0;
    bool bActive = false, bClosed = false, bPrevious = false, bOnGameThread = false;
};
}
