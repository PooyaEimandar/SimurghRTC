// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCSession.h"
#include "ContractLifecycleExample.generated.h"

/** Shared reporting only. The Blueprint asset implements its own lifecycle graph. */
UCLASS(Blueprintable)
class CONTRACTHOST_API AContractLifecycleExample : public AActor
{
    GENERATED_BODY()
public:
    AContractLifecycleExample();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Example") FSimurghRTCSessionConfig Config;
    UPROPERTY(Transient, BlueprintReadWrite, Category="Example") TObjectPtr<USimurghRTCSession> Session;
    UPROPERTY(Transient, BlueprintReadWrite, Category="Example") TObjectPtr<USimurghRTCOperation> PendingOperation;
    UFUNCTION(BlueprintCallable, Category="Example") void RecordOutcome(FName Step, const FSimurghRTCOperationResult& Result);
    UFUNCTION(BlueprintCallable, Category="Example") void UnexpectedCompletion(const FSimurghRTCOperationResult& Result);
    UFUNCTION(BlueprintCallable, Category="Example") void VerifyQuietTeardown(USimurghRTCOperation* Operation);
    bool IsComplete() const { return bComplete; }
    bool Passed() const { return bComplete && Errors.IsEmpty() && Outcomes.Num() == 3; }
    bool PrepareEndPlayCheck();
    bool EndPlayPassed() const;
    const TArray<FString>& GetErrors() const { return Errors; }
    const TArray<FString>& GetOutcomes() const { return Outcomes; }
private:
    bool bComplete = false;
    TArray<FString> Outcomes;
    TSet<FGuid> OperationIds;
    TArray<FString> Errors;
};

/** Native equivalent of /Game/Examples/BP_RtcLifecycle. */
UCLASS()
class CONTRACTHOST_API AContractLifecycleCpp : public AContractLifecycleExample
{
    GENERATED_BODY()
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    void AfterDisconnect(const FSimurghRTCOperationResult& Result);
    void AfterConnect(const FSimurghRTCOperationResult& Result);
    void AfterCancel(const FSimurghRTCOperationResult& Result);
};
