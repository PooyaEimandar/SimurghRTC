// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCNativeData.h"
#if SIMURGHRTC_WITH_DIRECT
#include "SimurghRTCDirectClient.h"
/** Lossless loopback fixture. Does not assert delivery guarantees under packet loss. */
class FContractDataScenario
{
public:
    explicit FContractDataScenario(TSharedRef<SimurghRTC::FDirectClient> InClient);
    ~FContractDataScenario();
    bool Tick();
    TSharedRef<FJsonObject> Report() const;
    FString Error;
    static FSimurghRTCChannelOptions Options(int32 Mode);
private:
    void Event(const SimurghRTC::FNativeChannelEvent& Event);
    bool Send(FSimurghRTCDataMessage Message);
    void Fail(const TCHAR* Why);
    TSharedRef<SimurghRTC::FDirectClient> Client;
    FSimurghRTCChannelHandle Channel;
    TArray<FSimurghRTCDataMessage> Expected;
    int32 Mode = 0, Phase = 0, Payload = 0, Received = 0, TextMessages = 0, BinaryMessages = 0, Closed = 0;
    int32 Rejected = 0;
    bool bBlocked = false, bResumed = false, bGameThread = true;
};
#endif
