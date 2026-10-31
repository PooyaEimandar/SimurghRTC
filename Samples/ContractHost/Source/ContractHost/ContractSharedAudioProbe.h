// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCSession.h"
#include "SimurghRTCOperation.h"
#include "ContractSharedAudioProbe.generated.h"

struct FContractAudioMeter
{
    double Energy = 0, ToneEnergy = 0;
    int64 Samples = 0;
    void Add(const FSimurghRTCAudioFrame& Frame, double Frequency);
    double Correlation() const { return Energy > 0 ? FMath::Sqrt(ToneEnergy / Energy) : 0; }
    double Rms() const { return Samples ? FMath::Sqrt(Energy / Samples) : 1; }
};

UCLASS()
class UContractSharedAudioEndpoint : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient) TObjectPtr<USimurghRTCSession> Session;
    UPROPERTY(Transient) TObjectPtr<USimurghRTCOperation> Pending;
    FString Endpoint, Token, Error;
    FSimurghRTCSourceHandle Source, OldSource;
    FSimurghRTCTrackHandle Local, Remote;
    FSimurghRTCChannelHandle Channel;
    int32 Step = 0, Phase = -1, PhasesPassed = 0, Operations = 0, Frames = 0, QuietFrames = 0, Checks = 0;
    double Started = 0, PhaseStarted = 0, NextSend = 0, QuietStarted = 0, MinimumCorrelation = 1;
    int64 SentSamples = 0;
    FContractAudioMeter Meter;
    TArray<FContractAudioMeter> Measurements;
    void Begin();
    void Update();
    void Op(USimurghRTCOperation* Operation, TFunction<void(const FSimurghRTCOperationResult&)> Completed);
    void Measure(int32 Number);
    UFUNCTION() void Track(const FSimurghRTCTrackInfo& Info, bool bAdded);
    UFUNCTION() void Audio(FSimurghRTCTrackHandle Handle, const FSimurghRTCAudioFrame& Frame);
    UFUNCTION() void Message(const FSimurghRTCDataMessage& Value);
};
class FContractSharedAudioProbe
{
public:
    FContractSharedAudioProbe();
    ~FContractSharedAudioProbe();
    void Startup();
    void Shutdown();
private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
    FDelegateHandle Exit;
};
