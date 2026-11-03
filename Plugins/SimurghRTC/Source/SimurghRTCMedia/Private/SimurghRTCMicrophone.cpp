// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCMicrophone.h"
#include "SimurghRTCMicrophoneInternal.h"
namespace SimurghRTC
{
#if WITH_DEV_AUTOMATION_TESTS
namespace { FMicrophoneTestFactory TestFactory; }
FMicrophoneTestFactory SetMicrophoneTestFactory(FMicrophoneTestFactory Factory)
{ check(IsInGameThread()); auto Previous = MoveTemp(TestFactory); TestFactory = MoveTemp(Factory); return Previous; }
#endif
TSharedPtr<IMicrophoneRequest> RequestMicrophone(const FString& DeviceId)
{
    check(IsInGameThread());
#if WITH_DEV_AUTOMATION_TESTS
    if (TestFactory) return TestFactory(DeviceId);
#endif
    return RequestPlatformMicrophone(DeviceId);
}
}
#if !PLATFORM_MAC
namespace SimurghRTC
{
TSharedPtr<IMicrophoneRequest> RequestPlatformMicrophone(const FString&) { return {}; }
FSimurghRTCError EnumerateMicrophones(TArray<FSimurghRTCDeviceInfo>& Devices)
{
    Devices.Reset();
    return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("Microphone capture is currently implemented on Mac only."));
}
FMicrophoneSnapshot GetMicrophoneSnapshot() { return {}; }
void StartMicrophoneService() {}
void StopMicrophoneService() {}
}
#endif
