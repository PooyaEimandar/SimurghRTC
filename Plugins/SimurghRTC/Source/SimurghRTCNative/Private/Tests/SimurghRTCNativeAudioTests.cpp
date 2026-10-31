// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeAudioProbe.h"
#include "Async/Async.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS && SIMURGHRTC_WITH_WEBRTC
namespace
{
class FAudioTransportCheck final : public IAutomationLatentCommand
{
public:
    explicit FAudioTransportCheck(FAutomationTestBase* InTest) : Test(InTest),
        Work(Async(EAsyncExecution::Thread, [] { return SimurghRTC::RunNativeAudioTransportProbe(); })) {}
    ~FAudioTransportCheck() { if (Work.IsValid()) Work.Wait(); }
    bool Update() override
    {
        if (!Work.IsReady()) return false;
        const auto Result = Work.Get();
        Test->TestTrue(*Result.Error, Result.bPassed);
        Test->TestEqual(TEXT("Two audio pairs"), Result.Pairs, 2);
        Test->TestEqual(TEXT("All native audio peers closed"), Result.PeersClosed, 4);
        return true;
    }
private:
    FAutomationTestBase* Test;
    TFuture<SimurghRTC::FNativeAudioProbeResult> Work;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeAudioTransportTest, "SimurghRTC.Native.AudioTransport",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FNativeAudioTransportTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FAudioTransportCheck(this)); return true;
}
#endif
