// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeRuntime.h"
#include "SimurghRTCNativeMediaProbe.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && SIMURGHRTC_WITH_WEBRTC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCNativeMediaTest, "SimurghRTC.Native.MediaCodecs",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimurghRTCNativeMediaTest::RunTest(const FString&)
{
    SimurghRTC::FNativeMediaProbeResult Result;
    FString Error;
    // Repeat to exercise codec callback teardown and fresh decoder state.
    for (int32 Run = 0; Run < 2; ++Run)
    {
        UE_RETURN_ON_ERROR(SimurghRTC::RunNativeMediaProbe(Result, Error), Error);
        TestEqual(TEXT("All generated frames were decoded"), Result.DecodedFrames, 12);
        TestEqual(TEXT("Only latest video frame retained"), Result.DecodedBGRA.Num(), 64 * 64);
        TestEqual(TEXT("One second of stereo PCM decoded"), Result.DecodedPCM.Num(), 96000);
        TestTrue(TEXT("Decoded tone retains frequency"), Result.ToneCorrelation >= 0.95);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCNativeLifetimeTest, "SimurghRTC.Native.FactoryLifetime",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimurghRTCNativeLifetimeTest::RunTest(const FString&)
{
    using namespace SimurghRTC;
    FNativeRuntime Media;
    FNativeRuntime Data;
    FString Error;
    TestFalse(TEXT("Peer creation requires factory"), Media.CreateProbePeerConnection(Error));
    UE_RETURN_ON_ERROR(Media.Start(ENativeFactoryMode::BuiltinMedia, Error), Error);
    TestFalse(TEXT("Duplicate start is rejected"), Media.Start(ENativeFactoryMode::BuiltinMedia, Error));
    auto Report = Media.GetDiagnostics();
    TestTrue(TEXT("Headless factory uses dummy audio"), Report.bDummyAudioDevice);
    TestTrue(TEXT("Audio processing is linked and created"), Report.bAudioProcessing);
    TestTrue(TEXT("Opus is advertised"), Report.AudioCodecs.Contains(TEXT("opus")));
    TestTrue(TEXT("Builtin video codecs exist"), Report.VideoCodecs.Num() > 0);
    UE_RETURN_ON_ERROR(Media.CreateProbePeerConnection(Error), Error);
    TestFalse(TEXT("Duplicate probe peer rejected"), Media.CreateProbePeerConnection(Error));
    // Two simultaneous owners exercise SSL/thread lifetime isolation.
    UE_RETURN_ON_ERROR(Data.Start(ENativeFactoryMode::DataOnly, Error), Error);
    TestFalse(TEXT("Data-only factory has no audio device"), Data.GetDiagnostics().bDummyAudioDevice);
    TestEqual(TEXT("Data-only factory has no audio codecs"), Data.GetDiagnostics().AudioCodecs.Num(), 0);
    TestEqual(TEXT("Data-only factory has no video codecs"), Data.GetDiagnostics().VideoCodecs.Num(), 0);
    Media.Stop();
    Media.Stop();
    TestFalse(TEXT("Stop clears readiness"), Media.GetDiagnostics().bFactoryReady);
    UE_RETURN_ON_ERROR(Data.CreateProbePeerConnection(Error), Error);
    Data.Stop();
    UE_RETURN_ON_ERROR(Media.Start(ENativeFactoryMode::BuiltinMedia, Error), Error);
    UE_RETURN_ON_ERROR(Media.CreateProbePeerConnection(Error), Error);
    // Destruction closes the final peer/factory and all owned threads.
    return true;
}
#endif
