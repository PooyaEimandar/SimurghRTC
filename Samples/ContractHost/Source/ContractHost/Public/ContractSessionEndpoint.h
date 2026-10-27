// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCSession.h"
#include "SimurghRTCOperation.h"
#include "ContractSessionEndpoint.generated.h"

// Packaged C++ fixture: all connection, channel and data operations use Core only.
UCLASS()
class CONTRACTHOST_API UContractSessionEndpoint : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY() TObjectPtr<USimurghRTCSession> Session;
    UPROPERTY() TObjectPtr<USimurghRTCOperation> Operation;
    FSimurghRTCChannelHandle Channel, OldChannel;
    FGuid FacadeId;
    TArray<FSimurghRTCDataMessage> Expected;
    TSet<FGuid> OperationIds;
    FString Error, Trigger;
    TWeakObjectPtr<UObject> LifetimeOwner;
    bool bGameThread = true, bQuiet = false, bTriggered = false, bCollectSession = false, bShutdownOnCompletion = false;
    int32 Completions = 0, StateEvents = 0, ChannelEvents = 0, Received = 0, Unexpected = 0, Operations = 0;
    void Initialize(UObject* Owner);
    void Start(USimurghRTCOperation* Op);
    bool Done() const { return Operation && Operation->IsFinished(); }
    bool Success() const { return Done() && Operation->GetResult().IsSuccess(); }
    void Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
    void Send(FSimurghRTCDataMessage Message);
    void SendPair(int32 Role, int32 Index);
    UFUNCTION() void StateChanged(ESimurghRTCSessionState State);
    UFUNCTION() void ChannelChanged(const FSimurghRTCChannelInfo& Info);
    UFUNCTION() void Message(const FSimurghRTCDataMessage& Data);
};
