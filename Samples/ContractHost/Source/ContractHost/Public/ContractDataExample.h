// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/ApplicationLifecycleComponent.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCSession.h"
#include "ContractDataExample.generated.h"

class FContractChannelValidation;
class FContractNetworkValidation;
// Plain reporting state survives actor destruction without retaining any UObject.
struct FContractDataExampleResult
{
    FString Error, Scenario = TEXT("messages"), Implementation, ClassPath;
    FGuid SessionId;
    TArray<FGuid> ChannelIds;
    TSharedPtr<FContractChannelValidation> ChannelValidation;
    TSharedPtr<FContractNetworkValidation> NetworkValidation;
    TArray<int32> ConnectionErrors;
    int32 Round = 0, CompletedRounds = 0, StaleHandleChecks = 0;
    bool bRoundRecorded = false, bAwaitingFailure = false, bFailureObserved = false;
    bool IsRecovery() const { return Scenario == TEXT("reconnect") || Scenario == TEXT("signaling-loss") || Scenario == TEXT("relay-loss"); }
    bool IsApplicationLifecycle() const { return Scenario == TEXT("background-connected") || Scenario == TEXT("background-connect"); }
    bool IsTwoConnections() const { return IsRecovery() || IsApplicationLifecycle(); }
    TArray<FGuid> SessionIds;
    int32 InactiveEvents = 0, ActiveEvents = 0, BackgroundEvents = 0, ForegroundEvents = 0, BackgroundCompletions = 0;
    bool bInBackground = false, bBackgroundShutdown = false, bBackgroundPending = false, bBackgroundCancelled = false;
    bool bRetiredObjectsCollected = false;
    double BackgroundStarted = 0, OSBackgroundAt = 0, BackgroundSeconds = 0, ForegroundAt = 0, ShutdownSeconds = 0;
    TArray<FString> Outcomes;
    TSet<FGuid> StartedIds, CompletedIds;
    TArray<ESimurghRTCSessionState> States;
    int32 Sent = 0, Received = 0, ChannelEvents = 0, OpenEvents = 0, EndPlayCalls = 0, LateCallbacks = 0;
    int32 LastFailure = 0;
    bool bGameThread = true, bComplete = false, bEnded = false, bEndPlayClosed = false;
    bool bRelayOnly = false, bHttps = false;
    bool bTextReceived = false, bBytesReceived = false, bInfoChecked = false;
    void Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
    void Callback() { bGameThread &= IsInGameThread(); if (bEnded || bInBackground) { ++LateCallbacks; Fail(TEXT("Callback after EndPlay or background shutdown.")); } }
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
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="RTC Example") TObjectPtr<UApplicationLifecycleComponent> ApplicationLifecycle;
    UPROPERTY(Transient, BlueprintReadOnly, Category="RTC Example") bool bValidationText = false;

    UFUNCTION(BlueprintCallable, Category="RTC Example") bool PrepareExample();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void TrackOperation(USimurghRTCOperation* Operation);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordOutcome(FName Step, FName Branch, const FSimurghRTCOperationResult& Result);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordChannelInfo(const FSimurghRTCError& Error, const FSimurghRTCChannelInfo& Info);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordSend(const FSimurghRTCError& Error);
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool BeginChannelValidation();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordValidationSend(const FSimurghRTCError& Error);
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestValidationSend();
    virtual void RequestValidationSend_Implementation();
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestCloseChannel();
    virtual void RequestCloseChannel_Implementation();
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestOpenChannel();
    virtual void RequestOpenChannel_Implementation();
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool FinishChannelClose();
    void TickChannelValidation();
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool RecordMessage(const FSimurghRTCDataMessage& Message);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordChannelEvent(const FSimurghRTCChannelInfo& Info);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordStateEvent(ESimurghRTCSessionState State);
    UFUNCTION(BlueprintCallable, Category="RTC Example") void CompleteExample();
    // Reload runtime credentials and reset round assertions; does not connect or disconnect.
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool PrepareNextRound();
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestConnect();
    virtual void RequestConnect_Implementation();
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestSession();
    virtual void RequestSession_Implementation();
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool PrepareBackground();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordBackgroundShutdown();
    UFUNCTION(BlueprintCallable, Category="RTC Example") bool PrepareForeground();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordOSBackground();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordOSForeground();
    void WriteLifecycleProgress() const;
    void CheckRetiredObjectsCollected();
    UFUNCTION(BlueprintCallable, Category="RTC Example") void RecordEndPlay();
    UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category="RTC Example") void RequestDisconnect();
    virtual void RequestDisconnect_Implementation();
    TSharedPtr<FContractDataExampleResult> GetExampleResult() const { return Report; }
private:
    bool LoadConnectionConfig();
    void SetRoundPayload();
    void CheckStaleChannel();
    TSharedPtr<FContractDataExampleResult> Report;
    TWeakObjectPtr<USimurghRTCSession> RetiredSession;
    TWeakObjectPtr<USimurghRTCOperation> RetiredOperation;
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
    void RequestConnect_Implementation() override;
    void RequestSession_Implementation() override;
    void RequestValidationSend_Implementation() override;
    void RequestCloseChannel_Implementation() override;
    void RequestOpenChannel_Implementation() override;
private:
    void Start(USimurghRTCOperation* Operation, FName Step);
    void Completed(FName Step, const FSimurghRTCOperationResult& Result);
    UFUNCTION() void ReceivedMessage(const FSimurghRTCDataMessage& Message);
    UFUNCTION() void ChannelChanged(const FSimurghRTCChannelInfo& Info);
    UFUNCTION() void StateChanged(ESimurghRTCSessionState State);
    UFUNCTION() void EnterBackground();
    UFUNCTION() void EnterForeground();
};
