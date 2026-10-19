// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SimurghRTCTypes.h"
#include "ContractExtensionExample.generated.h"

/** Reporting for the optional Janus Blueprint. No dependency on the extension module. */
UCLASS(Blueprintable)
class CONTRACTHOST_API AContractExtensionExample : public AActor
{
    GENERATED_BODY()
public:
    // The graph intentionally calls JoinVideoRoom with no session. Exactly one failure is expected.
    UFUNCTION(BlueprintCallable, Category="Example")
    void RecordExtensionResult(const FSimurghRTCOperationResult& Result, bool bFailureBranch);
    bool IsComplete() const { return CompletionCount > 0; }
    bool Passed() const { return CompletionCount == 1 && bExpectedFailure; }
private:
    int32 CompletionCount = 0;
    bool bExpectedFailure = false;
};
