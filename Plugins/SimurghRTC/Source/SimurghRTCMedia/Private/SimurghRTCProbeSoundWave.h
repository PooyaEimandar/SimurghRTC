// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Sound/SoundWaveProcedural.h"
#include "SimurghRTCProbeSoundWave.generated.h"

/** Queue once before playback; the real UE mixer is the sole consumer. */
UCLASS(Transient)
class USimurghRTCProbeSoundWave final : public USoundWaveProcedural
{
    GENERATED_BODY()
public:
    TAtomic<int32> ConsumedBytes{0};
    TAtomic<int32> CallbackCount{0};
    TAtomic<bool> bCallbackOnGameThread{false};
    virtual int32 GeneratePCMData(uint8* PCMData, const int32 SamplesNeeded) override
    {
        const int32 Before = GetAvailableAudioByteCount();
        const int32 Produced = Super::GeneratePCMData(PCMData, SamplesNeeded);
        ConsumedBytes += FMath::Min(Before, Produced); // Excludes underrun silence.
        ++CallbackCount;
        if (IsInGameThread()) bCallbackOnGameThread.Store(true);
        return Produced;
    }
};
