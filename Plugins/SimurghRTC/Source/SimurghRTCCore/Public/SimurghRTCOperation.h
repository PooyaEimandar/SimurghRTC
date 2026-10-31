// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Containers/Ticker.h"
#include "SimurghRTCTypes.h"
#include "SimurghRTCOperation.generated.h"
class USimurghRTCSession;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCOperationEvent, const FSimurghRTCOperationResult&, Result);
DECLARE_MULTICAST_DELEGATE_OneParam(FSimurghRTCOperationNativeEvent, const FSimurghRTCOperationResult&);

/** Factories are dormant until Activate. Blueprint async nodes activate after binding output pins. */
UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Operation"))
class SIMURGHRTCCORE_API USimurghRTCOperation : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* Connect(USimurghRTCSession* Session, const FSimurghRTCSessionConfig& Config);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* Disconnect(USimurghRTCSession* Session);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* OpenChannel(USimurghRTCSession* Session, const FSimurghRTCChannelOptions& Options);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* CloseChannel(USimurghRTCSession* Session, FSimurghRTCChannelHandle Channel);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* Call(USimurghRTCSession* Session, const FString& PeerId);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* AcceptCall(USimurghRTCSession* Session, const FString& CallId);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* RejectCall(USimurghRTCSession* Session, const FString& CallId);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* HangUp(USimurghRTCSession* Session);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* JoinTextRoom(USimurghRTCSession* Session, const FString& RoomId);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* LeaveTextRoom(USimurghRTCSession* Session, const FString& RoomId);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* SendRoomText(USimurghRTCSession* Session, const FString& RoomId, const FString& Text);

    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Frames", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* CreateGeneratedSource(USimurghRTCSession* Session, ESimurghRTCTrackKind Kind);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* AttachSource(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* ReplaceSource(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, FSimurghRTCSourceHandle Source);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* SetSourceMuted(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source, bool bMuted);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* StopSource(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* DetachTrack(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Frames", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCOperation* SetDecodedFrameDelivery(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, bool bEnabled);

    UFUNCTION(BlueprintCallable, Category="SimurghRTC") void Cancel();
    UFUNCTION(BlueprintPure, Category="SimurghRTC") bool IsFinished() const { return bFinished; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FGuid GetOperationId() const { return OperationId; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCOperationResult GetResult() const { return Result; }
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCOperationEvent OnSuccess;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCOperationEvent OnFailure;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCOperationEvent OnCancelled;
    FSimurghRTCOperationNativeEvent OnCompletedNative;
    virtual void Activate() override;
    virtual void BeginDestroy() override;

protected:
    void Initialize(USimurghRTCSession* Session, FName Name, ESimurghRTCFeature Feature, const FSimurghRTCError& Validation = {});

private:
    friend class USimurghRTCSession;
    void Execute();
    void Finish(FSimurghRTCError Error, bool bBroadcast = true);
    void Abandon();
    TWeakObjectPtr<USimurghRTCSession> Session;
    FGuid OperationId;
    FGuid SessionId;
    FName OperationName;
    ESimurghRTCFeature RequiredFeature = ESimurghRTCFeature::DataChannels;
    FSimurghRTCError ValidationError;
    FSimurghRTCChannelOptions ChannelOptions;
    FSimurghRTCChannelHandle PendingChannel;
    FSimurghRTCSourceHandle MediaSource;
    FSimurghRTCTrackHandle MediaTrack;
    ESimurghRTCTrackKind MediaKind = ESimurghRTCTrackKind::Audio;
    bool bMediaEnabled = false;
    double Deadline = 0;
    uint64 TransportGeneration = 0;
    bool bStarted = false;
    UPROPERTY(Transient) FSimurghRTCSessionConfig ConnectConfig;
    UPROPERTY(Transient) FSimurghRTCOperationResult Result;
    FTSTicker::FDelegateHandle TickHandle;
    bool bActivated = false;
    bool bFinished = false;
};
