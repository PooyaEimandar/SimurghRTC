// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#if SIMURGHRTC_WITH_DIRECT
#include "SimurghRTCDirectClient.h"
class FContractConcurrentClient
{
public:
    FContractConcurrentClient(TSharedRef<SimurghRTC::FDirectClient> InClient, int32 InRole, bool bInReplacement,
        TFunction<bool(TSharedRef<FJsonObject>)> InProgress);
    ~FContractConcurrentClient();
    void Tick();
    bool Finished(const SimurghRTC::FDirectClientSnapshot& Snapshot);
    TSharedRef<FJsonObject> Report() const;
    FString Error;
private:
    void Event(const SimurghRTC::FNativeChannelEvent& Event);
    void Begin(int32 NewPhase);
    void SendText(const FString& Text);
    void Fail(const TCHAR* Why);
    TSharedRef<SimurghRTC::FDirectClient> Client;
    FSimurghRTCChannelHandle Channel;
    TFunction<bool(TSharedRef<FJsonObject>)> Progress;
    TArray<FSimurghRTCDataMessage> Expected;
    TArray<TSharedPtr<FJsonValue>> Phases;
    int32 Role = 0, Phase = 0, Received = 0, ExpectedCount = 0;
    bool bReplacement = false, bOpened = false, bDone = false, bSealed = false, bFinish = false, bGameThread = true;
};
#endif
#if CONTRACTHOST_WITH_DIRECT_SERVER
#include "SimurghRTCDirectServer.h"
class FContractConcurrentServer
{
public:
    explicit FContractConcurrentServer(TFunction<void(const FString&)> InFail) : Fail(MoveTemp(InFail)) {}
    bool Configure(SimurghRTC::FDirectServerConfig& Config, const FString& CredentialFile);
    void Attach(TSharedPtr<SimurghRTC::FDirectServer> InServer) { Server = InServer; }
    bool Command(const FString& Action);
    TSharedRef<FJsonObject> Report() const;
private:
    void Session(const SimurghRTC::FDirectSessionInfo& Info);
    void Event(const FString& Id, const SimurghRTC::FNativeChannelEvent& Event);
    void Send(const SimurghRTC::FDirectChannelAddress& Address, const FString& Text);
    void Message(const FString& Id, const FSimurghRTCDataMessage& Message);
    TWeakPtr<SimurghRTC::FDirectServer> Server;
    TFunction<void(const FString&)> Fail;
    TMap<FString, FString> Tokens;
    TMap<FString, SimurghRTC::FDirectSessionInfo> Sessions;
    TMap<FString, SimurghRTC::FDirectChannelAddress> Addresses;
    TMap<FString, TSet<FString>> Inputs;
    TSet<FString> Done, Sealed, NativeClosed;
    TArray<TSharedPtr<FJsonValue>> PhaseReports;
    SimurghRTC::FDirectChannelAddress Retired;
    FString OriginalBeta;
    int32 Phase = 0, ExpectedClients = 0, Echoes = 0, Routed = 0, Denied = 0, IsolationChecks = 0;
    bool bBarrier = false, bGameThread = true, bCloseDuringPolicy = false;
};
#endif
