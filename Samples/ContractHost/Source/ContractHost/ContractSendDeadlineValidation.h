// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCTypes.h"
class AContractDataExample;
class FContractSendDeadlineValidation
{
public:
    explicit FContractSendDeadlineValidation(bool Expire) : bExpire(Expire) {}
    void Opened(AContractDataExample& A);
    void Tick(AContractDataExample& A);
    void SendResult(AContractDataExample& A, const FSimurghRTCError& Error);
    void Message(AContractDataExample& A, const FSimurghRTCDataMessage& Message);
    TSharedRef<FJsonObject> Report() const;
private:
    bool bExpire, bSending = false;
    int32 Phase = 0, Received = 0;
    double AdmittedAt = 0, SettledAfter = 0, QuietAt = 0;
    int64 PeakBuffered = 0;
    TArray<FSimurghRTCDataMessage> Expected;
};
