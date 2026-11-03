// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCMicrophoneBuffer.h"
#include "SimurghRTCMicrophoneInternal.h"
#include "SimurghRTCSession.h"
#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_DEV_AUTOMATION_TESTS
using namespace SimurghRTC;
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMicrophoneBufferTest, "SimurghRTC.Media.MicrophoneBuffer",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMicrophoneBufferTest::RunTest(const FString&)
{
    for (int32 Rate : {8000, 16000, 32000, 44100, 48000})
    {
        FMicrophoneBuffer Whole, Split; Whole.SetActive(true); Split.SetActive(true);
        TArray<float> Input; Input.SetNumUninitialized(Rate / 10 * 2);
        for (int32 I = 0; I < Input.Num()/2; ++I) Input[I*2] = Input[I*2+1] = .25f * FMath::Sin(2*PI*400*I/Rate);
        TestTrue(TEXT("Supported capture format"), Whole.Push(Input.GetData(), Input.Num()/2, 2, Rate));
        for (int32 I = 0; I < Input.Num()/2;)
        { const int32 N = FMath::Min(137, Input.Num()/2-I); Split.Push(Input.GetData()+I*2, N, 2, Rate); I += N; }
        FSimurghRTCAudioFrame A, B; int32 Samples = 0; double Energy = 0, Error = 0;
        while (Whole.Poll(A))
        {
            TestTrue(TEXT("Callback boundaries preserve conversion"), Split.Poll(B));
            TestEqual(TEXT("Same timestamp"), A.TimestampMicroseconds, B.TimestampMicroseconds);
            TestTrue(TEXT("Same converted samples"), A.Samples == B.Samples);
            for (float V : A.Samples) { const double Want=.25*FMath::Sin(2*PI*400*Samples/48000); Energy+=Want*Want; Error+=(V-Want)*(V-Want); ++Samples; }
        }
        TestTrue(TEXT("Resampled tone preserves signal"), Samples >= 4320 && Error/Energy < .001);
    }
    FMicrophoneBuffer Ring; TArray<float> Values; Values.Init(.25f, 480);
    Ring.Push(Values.GetData(), 480, 1, 48000); FSimurghRTCAudioFrame F;
    TestFalse(TEXT("Unattached capture is discarded"), Ring.Poll(F)); Ring.SetActive(true);
    for (int32 I=0;I<30;++I) Ring.Push(Values.GetData(),480,1,48000);
    uint64 Calls, Frames, Dropped; int32 Peak; bool bGT; Ring.Snapshot(Calls,Frames,Dropped,Peak,bGT);
    TestEqual(TEXT("Fixed 200 ms capacity"),Peak,9600); TestEqual(TEXT("Oldest overflow dropped"),Dropped,uint64(4800));
    TestTrue(TEXT("Fresh tail available"),Ring.Poll(F)); TestEqual(TEXT("Overflow advances timestamp"),F.TimestampMicroseconds,int64(100000));
    const auto Previous=F.TimestampMicroseconds; Ring.SetActive(false); Ring.SetActive(true);
    TestFalse(TEXT("Mute clears stale capture"),Ring.Poll(F)); Ring.Push(Values.GetData(),480,1,48000);Ring.Poll(F);
    TestTrue(TEXT("Resume timestamp remains monotonic"),F.TimestampMicroseconds>Previous);
    TestFalse(TEXT("Unsupported downsampling rejected"),Ring.Push(Values.GetData(),480,1,96000));
    Ring.Close(); Ring.SetActive(true); Ring.Push(Values.GetData(),480,1,48000);
    TestFalse(TEXT("Closed callback cannot enqueue"),Ring.Poll(F));
    return true;
}
namespace
{
class FPendingMicrophone final : public IMicrophoneRequest
{
public:
    bool bReady=false; int32 Closes=0; FSimurghRTCError Failure;
    FSimurghRTCError Advance(bool& Ready) override { Ready=bReady; return Failure; }
    void SetActive(bool) override {}
    bool Poll(FSimurghRTCAudioFrame&) override { return false; }
    FSimurghRTCError Error() const override { return {}; }
    void Close() override { ++Closes; }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMicrophoneOperationTest, "SimurghRTC.Media.MicrophoneOperation",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMicrophoneOperationTest::RunTest(const FString&)
{
    if (!SIMURGHRTC_WITH_DIRECT) return true;
    for (int32 Case=0;Case<4;++Case)
    {
        TStrongObjectPtr<UGameInstance> Owner(NewObject<UGameInstance>());
        TStrongObjectPtr<USimurghRTCSession> Session(USimurghRTCSession::CreateSession(Owner.Get()));
        auto Request=MakeShared<FPendingMicrophone>();
        auto Previous=SetMicrophoneTestFactory([Request](const FString&){return Request;});
        TStrongObjectPtr<USimurghRTCMediaOperation> Op(USimurghRTCMediaOperation::CreateSource(Session.Get(),{}));
        int32 Completions=0;Op->OnCompletedNative.AddLambda([&](const auto&){++Completions;});
        Op->Activate();FTSTicker::GetCoreTicker().Tick(0);
        TestFalse(TEXT("Permission/open waits without publishing a source"),Op->IsFinished() || Op->GetResult().Source.IsValid());
        if(Case==0) Op->Cancel();
        else if(Case==1) Session->Shutdown();
        else if(Case==2) {Request->Failure=SimurghRTC::MakeError(ESimurghRTCErrorCode::PermissionDenied,TEXT("Denied"));FTSTicker::GetCoreTicker().Tick(0);}
        else {Request->bReady=true;FTSTicker::GetCoreTicker().Tick(0);TestTrue(TEXT("Ready capture publishes source"),Op->GetResult().Source.IsValid());TestEqual(TEXT("Session owns capture after completion"),Request->Closes,0);Session->Shutdown();}
        TestTrue(TEXT("Pending operation finishes"),Op->IsFinished());
        TestEqual(TEXT("Exactly one release"),Request->Closes,1);
        Request->bReady=true;FTSTicker::GetCoreTicker().Tick(0);
        TestEqual(TEXT("No late completion after cancellation/owner shutdown"),Completions,Case==1?0:1);
        if(Case<3) TestFalse(TEXT("Failure cannot publish a handle"),Op->GetResult().Source.IsValid());
        Session->Shutdown(); SetMicrophoneTestFactory(MoveTemp(Previous));
    }
    return true;
}
#endif
