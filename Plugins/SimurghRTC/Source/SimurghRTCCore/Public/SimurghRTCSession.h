// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Containers/Ticker.h"
#include "SimurghRTCTypes.h"
#include "SimurghRTCSession.generated.h"

class USimurghRTCOperation;
namespace SimurghRTC { class ISessionTransport; }
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCSessionStateEvent, ESimurghRTCSessionState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCDataEvent, const FSimurghRTCDataMessage&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCChannelEvent, const FSimurghRTCChannelInfo&, Channel);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCTrackEvent, const FSimurghRTCTrackInfo&, Track, bool, bAdded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCParticipantEvent, const FSimurghRTCParticipant&, Participant, bool, bJoined);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSimurghRTCRoomTextEvent, const FString&, RoomId, const FString&, SenderId, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCIncomingCallEvent, const FString&, CallId, const FString&, CallerId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCCallEvent, const FSimurghRTCCallInfo&, Call);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCAudioFrameEvent, FSimurghRTCTrackHandle, Track, const FSimurghRTCAudioFrame&, Frame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCVideoFrameEvent, FSimurghRTCTrackHandle, Track, const FSimurghRTCVideoFrame&, Frame);

/** Game-thread facade. Retain the returned session in a UPROPERTY/Blueprint variable. */
UCLASS(BlueprintType)
class SIMURGHRTCCORE_API USimurghRTCSession : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintPure, Category="SimurghRTC") static int32 GetApiContractVersion() { return SimurghRTC::ApiContractVersion; }
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(DefaultToSelf="Owner"))
    static USimurghRTCSession* CreateSession(UObject* Owner);

    UFUNCTION(BlueprintPure, Category="SimurghRTC") FGuid GetSessionId() const { return SessionId; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") ESimurghRTCSessionState GetState() const { return State; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCCapability GetCapability(ESimurghRTCProvider Provider, ESimurghRTCFeature Feature) const;
    // Connection/transport error; read from OnStateChanged(Failed). Cleared by a new Connect or Disconnect.
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCError GetConnectionError() const;
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCStatistics GetStatistics() const { return {}; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCCallInfo GetCallInfo() const { return {}; }
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") FSimurghRTCError GetChannelInfo(FSimurghRTCChannelHandle Channel, FSimurghRTCChannelInfo& Info) const;
    // A successful send means copied into a bounded queue, not remote delivery.
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") FSimurghRTCError SendText(FSimurghRTCChannelHandle Channel, const FString& Text);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") FSimurghRTCError SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Frames") FSimurghRTCError PushAudioFrame(FSimurghRTCSourceHandle Source, const FSimurghRTCAudioFrame& Frame);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Frames") FSimurghRTCError PushVideoFrame(FSimurghRTCSourceHandle Source, const FSimurghRTCVideoFrame& Frame);
    // Final local teardown; suppresses pending operation callbacks. Create a new facade to reuse.
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") void Shutdown();

    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCSessionStateEvent OnStateChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCDataEvent OnMessage;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCChannelEvent OnChannelChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCTrackEvent OnTrackChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCParticipantEvent OnParticipantChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCRoomTextEvent OnRoomText;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCIncomingCallEvent OnIncomingCall;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCCallEvent OnCallChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC|Frames") FSimurghRTCAudioFrameEvent OnAudioFrame;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC|Frames") FSimurghRTCVideoFrameEvent OnVideoFrame;

    virtual UWorld* GetWorld() const override;
    virtual void BeginDestroy() override;
    bool IsUsable() const;
    UFUNCTION(BlueprintPure, Category="SimurghRTC") ESimurghRTCProvider GetProvider() const { return Provider; }

private:
    friend class USimurghRTCOperation;
    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
    void SetState(ESimurghRTCSessionState NewState);
    FSimurghRTCError ChannelUnavailable(FSimurghRTCChannelHandle Channel) const;
    bool TickTransport(float);
    void ReleaseTransport();
    FSimurghRTCError StartTransport(const FSimurghRTCSessionConfig& Config);
    TSharedPtr<SimurghRTC::ISessionTransport> Transport;
    FTSTicker::FDelegateHandle TransportTicker;
    uint64 TransportGeneration = 0;
    double OperationTimeoutSeconds = 30;
    FSimurghRTCError TransportError;
    UPROPERTY(Transient) FGuid SessionId;
    UPROPERTY(Transient) ESimurghRTCSessionState State = ESimurghRTCSessionState::Idle;
    UPROPERTY(Transient) TArray<TObjectPtr<USimurghRTCOperation>> PendingOperations;
    TWeakObjectPtr<UObject> LifetimeOwner;
    FDelegateHandle WorldCleanupHandle;
    ESimurghRTCProvider Provider = ESimurghRTCProvider::DirectUnreal;
    bool bShuttingDown = false;
};
