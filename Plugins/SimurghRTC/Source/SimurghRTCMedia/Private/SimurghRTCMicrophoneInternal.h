// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCSessionAudioSource.h"
#include "SimurghRTCMediaOperation.h"
namespace SimurghRTC
{
class IMicrophoneRequest : public ISessionAudioSource
{
public:
    // Called on the game thread; permission and device opening may still be pending.
    virtual FSimurghRTCError Advance(bool& bReady) = 0;
};
TSharedPtr<IMicrophoneRequest> RequestMicrophone(const FString& DeviceId);
TSharedPtr<IMicrophoneRequest> RequestPlatformMicrophone(const FString& DeviceId);
#if WITH_DEV_AUTOMATION_TESTS
using FMicrophoneTestFactory = TFunction<TSharedPtr<IMicrophoneRequest>(const FString&)>;
FMicrophoneTestFactory SetMicrophoneTestFactory(FMicrophoneTestFactory Factory);
#endif
FSimurghRTCError EnumerateMicrophones(TArray<FSimurghRTCDeviceInfo>& Devices);
void StartMicrophoneService();
void StopMicrophoneService();
}
