// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SimurghRTCTypes.h"
#include "SimurghRTCSession.generated.h"

class USimurghRTCOperation;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCSessionStateEvent, ESimurghRTCSessionState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCDataEvent, const FSimurghRTCDataMessage&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSimurghRTCChannelEvent, const FSimurghRTCChannelInfo&, Channel);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCTrackEvent, const FSimurghRTCTrackInfo&, Track, bool, bAdded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCParticipantEvent, const FSimurghRTCParticipant&, Participant, bool, bJoined);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSimurghRTCRoomTextEvent, const FString&, RoomId, const FString&, SenderId, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSimurghRTCIncomingCallEvent, const FString&, CallId, const FString&, CallerId);

/** Game-thread facade. Retain the returned session in a UPROPERTY/Blueprint variable. */
UCLASS(BlueprintType)
class SIMURGHRTCCORE_API USimurghRTCSession : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="SimurghRTC", meta=(DefaultToSelf="Owner"))
    static USimurghRTCSession* CreateSession(UObject* Owner);

    UFUNCTION(BlueprintPure, Category="SimurghRTC") FGuid GetSessionId() const { return SessionId; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") ESimurghRTCSessionState GetState() const { return State; }
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCCapability GetCapability(ESimurghRTCProvider Provider, ESimurghRTCFeature Feature) const;
    UFUNCTION(BlueprintPure, Category="SimurghRTC") FSimurghRTCStatistics GetStatistics() const { return {}; }
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") FSimurghRTCError GetChannelInfo(FSimurghRTCChannelHandle Channel, FSimurghRTCChannelInfo& Info) const;
    // A successful send means accepted into a bounded queue, not delivery. No queue exists in M0.
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") FSimurghRTCError SendText(FSimurghRTCChannelHandle Channel, const FString& Text);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") FSimurghRTCError SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes);
    // Final local teardown; suppresses pending operation callbacks. Create a new facade to reuse.
    UFUNCTION(BlueprintCallable, Category="SimurghRTC") void Shutdown();

    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCSessionStateEvent OnStateChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCDataEvent OnMessage;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCChannelEvent OnChannelChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCTrackEvent OnTrackChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCParticipantEvent OnParticipantChanged;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCRoomTextEvent OnRoomText;
    UPROPERTY(BlueprintAssignable, Category="SimurghRTC") FSimurghRTCIncomingCallEvent OnIncomingCall;

    virtual UWorld* GetWorld() const override;
    virtual void BeginDestroy() override;
    bool IsUsable() const;
    UFUNCTION(BlueprintPure, Category="SimurghRTC") ESimurghRTCProvider GetProvider() const { return Provider; }

private:
    friend class USimurghRTCOperation;
    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
    void SetState(ESimurghRTCSessionState NewState);
    FSimurghRTCError ChannelUnavailable(FSimurghRTCChannelHandle Channel) const;
    UPROPERTY(Transient) FGuid SessionId;
    UPROPERTY(Transient) ESimurghRTCSessionState State = ESimurghRTCSessionState::Idle;
    UPROPERTY(Transient) TArray<TObjectPtr<USimurghRTCOperation>> PendingOperations;
    TWeakObjectPtr<UObject> LifetimeOwner;
    FDelegateHandle WorldCleanupHandle;
    ESimurghRTCProvider Provider = ESimurghRTCProvider::DirectUnreal;
    bool bShuttingDown = false;
};
