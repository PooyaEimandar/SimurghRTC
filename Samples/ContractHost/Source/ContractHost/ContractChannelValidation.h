// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCTypes.h"
class AContractDataExample;

/** Bounded sample assertions. Sends run through the actual C++ flow or Blueprint nodes. */
class FContractChannelValidation
{
public:
    FContractChannelValidation(int32 InMode, int32 InLimit) : Mode(InMode), Limit(InLimit) {}
    void Opened(AContractDataExample& Actor);
    void Tick(AContractDataExample& Actor);
    void SendResult(AContractDataExample& Actor, const FSimurghRTCError& Error);
    void Message(AContractDataExample& Actor, const FSimurghRTCDataMessage& Message);
    void Changed(const FSimurghRTCChannelInfo& Info);
    TSharedRef<FJsonObject> Report() const;
    int32 Mode, Limit;
private:
    void Send(AContractDataExample& Actor, FSimurghRTCDataMessage Message, ESimurghRTCErrorCode ExpectedCode = ESimurghRTCErrorCode::None);
    TArray<FSimurghRTCDataMessage> Expected;
    FSimurghRTCDataMessage Sending;
    ESimurghRTCErrorCode ExpectedCode = ESimurghRTCErrorCode::None;
    int32 Phase = 0, Payload = 0, Received = 0, TextMessages = 0, BinaryMessages = 0;
    int32 Rejected = 0, ByteQueueRejections = 0, SlotQueueRejections = 0, BufferEvents = 0;
    int64 PeakBuffered = 0;
    bool bSending = false, bByteQueueRecovered = false, bSlotQueueRecovered = false, bOptionsChecked = false;
};
