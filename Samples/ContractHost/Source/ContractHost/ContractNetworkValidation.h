// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCTypes.h"
class AContractDataExample;

/** Assertions for externally impaired TURN traffic; all operations use the sample's public API flow. */
class FContractNetworkValidation
{
public:
    explicit FContractNetworkValidation(int32 InMode);
    static FSimurghRTCChannelOptions Options(int32 Mode, int32 Index = 0);
    static bool ParseOptions(const FString& Label, FSimurghRTCChannelOptions& Out);
    void Opened(AContractDataExample& Actor);
    bool Closed(AContractDataExample& Actor);
    void Tick(AContractDataExample& Actor);
    void SendResult(AContractDataExample& Actor, const FSimurghRTCError& Error);
    void Message(AContractDataExample& Actor, const FSimurghRTCDataMessage& Message);
    void Changed(AContractDataExample& Actor, const FSimurghRTCChannelInfo& Info);
    TSharedRef<FJsonObject> Report() const;
private:
    bool Gate(bool Release = false) const;
    void Send(AContractDataExample& Actor, int32 ChannelIndex, int32 Seed, int32 Size, bool Expired = false, bool Reject = false, bool Text = false);
    int32 Mode, Phase = 0, Received = 0, Rejected = 0, ExpiredSent = 0, ClosedCount = 0, BufferEvents = 0;
    int64 PeakBuffered = 0, EmptyChannelAtRejection = -1;
    bool bSending = false, bExpired = false, bReject = false, bOpening = false, bClosing = false, bRecovered = false;
    double QuietAt = 0;
    FString GatePath;
    TArray<FSimurghRTCChannelHandle> Channels;
    TArray<FSimurghRTCDataMessage> Expected;
    FSimurghRTCDataMessage Sending;
};
