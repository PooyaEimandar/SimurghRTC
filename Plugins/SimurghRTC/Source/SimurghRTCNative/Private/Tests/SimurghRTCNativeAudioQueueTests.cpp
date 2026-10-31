// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeAudioQueue.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeAudioQueueTest,"SimurghRTC.Native.AudioQueueAdmission", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FNativeAudioQueueTest::RunTest(const FString&)
{
    SimurghRTC::FNativeAudioQueue Q; FSimurghRTCAudioFrame F; F.Samples.Init(0.25f,4800);
    TestEqual(TEXT("Unconfigured input rejected"),Q.Push(F).Code,ESimurghRTCErrorCode::InvalidState);
    Q.bConfigured = true; Q.Mute(false);
    TestFalse(TEXT("100 ms accepted"),Q.Push(F).IsError()); F.Samples[0] = -0.5f;
    TestEqual(TEXT("Accepted samples are copied"),Q.Input[0],0.25f);
    TestFalse(TEXT("200 ms bound reached"),Q.Push(F).IsError());
    TestEqual(TEXT("Whole submission rejected at capacity"),Q.Push(F).Code,ESimurghRTCErrorCode::ResourceExhausted);
    TestEqual(TEXT("No partial admission"),Q.Input.Num(),9600);
    Q.Mute(true); TestEqual(TEXT("Mute discards pending PCM"),Q.Input.Num(),0);
    TestEqual(TEXT("Muted input rejected"),Q.Push(F).Code,ESimurghRTCErrorCode::InvalidState);
    Q.Mute(false); F.SampleRate=44100; F.Samples.SetNum(4410);
    TestEqual(TEXT("Valid but unsupported shared format"),Q.Push(F).Code,ESimurghRTCErrorCode::ServiceUnsupported);
    F.SampleRate=48000; F.Samples.SetNum(37);
    TestFalse(TEXT("Partial 10 ms block accepted within bound"),Q.Push(F).IsError());
    Q.Reset(); TestEqual(TEXT("Close clears partial block"),Q.Input.Num(),0); TestFalse(TEXT("Close clears configuration"),Q.bConfigured);
    return true;
}
#endif
