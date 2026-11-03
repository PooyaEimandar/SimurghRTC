// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Sound/SoundWaveProcedural.h"
#include "SimurghRTCAudioPlaybackBuffer.h"
#include "SimurghRTCPlaybackSoundWave.generated.h"

UCLASS(Transient)
class USimurghRTCPlaybackSoundWave final : public USoundWaveProcedural
{
    GENERATED_BODY()
public:
    // Assigned before Play and never replaced while the mixer can read it.
    TSharedPtr<SimurghRTC::FAudioPlaybackBuffer, ESPMode::ThreadSafe> Buffer;
    virtual int32 GeneratePCMData(uint8* Out, int32 SamplesNeeded) override
    {
        if (SamplesNeeded <= 0) return 0;
        if (Buffer) Buffer->Pop(reinterpret_cast<int16*>(Out), SamplesNeeded);
        else FMemory::Memzero(Out, SamplesNeeded * sizeof(int16));
        return SamplesNeeded * sizeof(int16);
    }
};
