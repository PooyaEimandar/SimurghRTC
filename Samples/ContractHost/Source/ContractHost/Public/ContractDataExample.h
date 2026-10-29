// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCSession.h"
#include "ContractDataExample.generated.h"

// Plain reporting state survives actor destruction without retaining any UObject.
struct FContractDataExampleResult
{
    FString Error, Scenario = TEXT("messages"), Implementation, ClassPath;
    FGuid SessionId;
    TArray<FString> Outcomes;
    TSet<FGuid> StartedIds, CompletedIds;
    TArray<ESimurghRTCSessionState> States;
    int32 Sent = 0, Received = 0, ChannelEvents = 0, OpenEvents = 0, EndPlayCalls = 0, LateCallbacks = 0;
    int32 LastFailure = 0;
    bool bGameThread = true, bComplete = false, bEnded = false, bEndPlayClosed = false;
    bool bTextReceived = false, bBytesReceived = false, bInfoChecked = false;
    void Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
    void Callback() { bGameThread &= IsInGameThread(); if (bEnded) { ++LateCallbacks; Fail(TEXT("Callback after EndPlay.")); } }
};

/** Configuration and assertions only. BP_RtcData implements its own public-node transport flow. */
UCLASS(Blueprintable)
class CONTRACTHOST_API AContractDataExample : public AActor
{
    GENERATED_BODY()
public:
    AContractDataExample();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RTC Example") FSimurghRTCSessionConfig Config;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RTC Example") FSimurghRTCChannelOptions Options;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RTC Example") FString TextToSend;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RTC Example") TArray<uint8> BytesToSend;
    UPROPERTY(Transient, BlueprintReadWrite, Category="RTC Example") TObjectPtr<USimurghRTCSession> Session;
    UPROPERTY(Transient, BlueprintReadWrite, Category="RTC Example") TObjectPtr<USimurghRTCOperation> PendingOperation;
    UPROPERTY(Transient, BlueprintReadWrite, Category="RTC Example") FSimurghRTCChannelHandle Channel;

    UFUNCTION(BlueprintCallable, Category="RTC Example") bool PrepareExample();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void TrackOperation(USimurghRTCOperation* Operation);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordOutcome(FName Step, FName Branch, const FSimurghRTCOperationResult& Result);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordChannelInfo(const FSimurghRTCError& Error, const FSimurghRTCChannelInfo& Info);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordSend(const FSimurghRTCError& Error);
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool RecordMessage(const FSimurghRTCDataMessage& Message);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordChannelEvent(const FSimurghRTCChannelInfo& Info);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordStateEvent(ESimurghRTCSessionState State);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void CompleteExample();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordEndPlay();
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestDisconnect();
    virtual void RequestDisconnect_Implementation();
    TSharedPtr<FContractDataExampleResult> GetExampleResult() const { return Report; }
private:
    TSharedPtr<FContractDataExampleResult> Report;
};

/** Native equivalent of /Game/Examples/Direct/BP_RtcData. */
UCLASS()
class CONTRACTHOST_API AContractDataCpp : public AContractDataExample
{
    GENERATED_BODY()
protected:
    void BeginPlay() override;
    void EndPlay(const EEndPlayReason::Type Reason) override;
    void RequestDisconnect_Implementation() override;
private:
    void Start(USimurghRTCOperation* Operation, FName Step);
    void Completed(FName Step, const FSimurghRTCOperationResult& Result);
    UFUNCTION() void ReceivedMessage(const FSimurghRTCDataMessage& Message);
    UFUNCTION() void ChannelChanged(const FSimurghRTCChannelInfo& Info);
    UFUNCTION() void StateChanged(ESimurghRTCSessionState State);
};
