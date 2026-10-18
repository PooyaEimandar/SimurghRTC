// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCJanusOperation.generated.h"

// This entire reflected module is absent from Janus-disabled packages. No UHT feature guards.
USTRUCT(BlueprintType)
struct SIMURGHRTCJANUSEXTENSIONS_API FSimurghRTCJanusRoomOptions
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC|Janus") FString RoomId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC|Janus") FString DisplayName;
};

UCLASS(BlueprintType)
class SIMURGHRTCJANUSEXTENSIONS_API USimurghRTCJanusOperation : public USimurghRTCOperation
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Janus", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCJanusOperation* JoinVideoRoom(USimurghRTCSession* Session, const FSimurghRTCJanusRoomOptions& Options);
};
