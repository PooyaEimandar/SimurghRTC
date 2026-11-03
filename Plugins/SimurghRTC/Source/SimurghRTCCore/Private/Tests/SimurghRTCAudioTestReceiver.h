// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCSession.h"
#include "SimurghRTCAudioTestReceiver.generated.h"

UCLASS()
class USimurghRTCAudioTestReceiver : public UObject
{
    GENERATED_BODY()
public:
    FSimurghRTCTrackHandle Remote;
    int32 Frames = 0;
    UFUNCTION() void Track(const FSimurghRTCTrackInfo& Info, bool bAdded)
    { if (Info.bRemote) Remote = bAdded ? Info.Handle : FSimurghRTCTrackHandle{}; }
    UFUNCTION() void Audio(FSimurghRTCTrackHandle Handle, const FSimurghRTCAudioFrame& Frame) { ++Frames; }
};
