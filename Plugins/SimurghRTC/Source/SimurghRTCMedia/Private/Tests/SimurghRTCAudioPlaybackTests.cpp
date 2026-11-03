// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCAudioPlaybackBuffer.h"
#include "Misc/AutomationTest.h"
#include "Async/Async.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioPlaybackBufferTest, "SimurghRTC.Media.PlaybackBuffer",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAudioPlaybackBufferTest::RunTest(const FString&)
{
    SimurghRTC::FAudioPlaybackBuffer Buffer;
    FSimurghRTCAudioFrame Frame; Frame.Samples.Init(0.25f, 4800);
    TestTrue(TEXT("First block copied"), Buffer.Push(Frame));
    Frame.Samples.Init(-0.5f, 4800); Buffer.Push(Frame);
    Frame.Samples.Init(1.0f, 4800); Buffer.Push(Frame);
    auto State = Buffer.Snapshot();
    TestEqual(TEXT("Fixed 200 ms bound"), State.QueuedSamples, 9600);
    TestEqual(TEXT("Drop oldest on overflow"), State.DroppedSamples, uint64(4800));
    TArray<int16> Output; Output.SetNumUninitialized(10000);
    Async(EAsyncExecution::Thread, [&] { Buffer.Pop(Output.GetData(), Output.Num()); }).Get();
    TestEqual(TEXT("Oldest retained PCM"), Output[0], int16(-16384));
    TestEqual(TEXT("Positive full scale clamps"), Output[4800], int16(32767));
    TestEqual(TEXT("Underflow is silence"), Output[9600], int16(0));
    State = Buffer.Snapshot(); TestEqual(TEXT("Consumed count excludes silence"), State.ConsumedSamples, uint64(9600));
    TestFalse(TEXT("Consumer runs off game thread"), State.bCallbackOnGameThread);
    Frame.SampleRate = 44100; Frame.Samples.SetNum(4410);
    TestFalse(TEXT("Wrong rate rejected"), Buffer.Push(Frame));
    Frame.SampleRate = 48000; Frame.Samples.Init(-1.0f, 480); Buffer.Push(Frame);
    Async(EAsyncExecution::Thread, [&] { Buffer.Pop(Output.GetData(), 480); }).Get();
    TestEqual(TEXT("Negative full scale is exact"), Output[0], int16(-32768));
    Buffer.Push(Frame); Buffer.Close(); Buffer.Close();
    TestFalse(TEXT("Closed queue rejects new PCM"), Buffer.Push(Frame));
    Async(EAsyncExecution::Thread, [&] { Buffer.Pop(Output.GetData(), 480); }).Get();
    TestEqual(TEXT("Close discards queued PCM immediately"), Output[0], int16(0));
    TestEqual(TEXT("Close leaves no samples"), Buffer.Snapshot().QueuedSamples, 0);
    return true;
}
#endif
