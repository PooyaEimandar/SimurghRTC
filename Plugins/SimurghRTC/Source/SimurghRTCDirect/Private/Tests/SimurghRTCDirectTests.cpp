// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCDirectProtocol.h"
#include "SimurghRTCDirectClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_DEV_AUTOMATION_TESTS
using namespace SimurghRTC;
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDirectProtocolTest, "SimurghRTC.Direct.Protocol", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDirectProtocolTest::RunTest(const FString&)
{
    const FString Create = TEXT("{\"protocol\":\"simurghrtc.direct\",\"version\":1,\"type\":\"create\",\"request_id\":\"a\",\"sequence\":0,\"body\":{\"purpose\":\"data\",\"identity\":\"test\"}}");
    Direct::FRequest A, B; FString Error;
    TestTrue(TEXT("v1 create"), Direct::ParseRequest(Direct::Bytes(Create), A, Error));
    const FString Reordered = TEXT("{\"body\":{\"identity\":\"test\",\"purpose\":\"data\"},\"sequence\":0,\"request_id\":\"a\",\"type\":\"create\",\"version\":1,\"protocol\":\"simurghrtc.direct\"}");
    TestTrue(TEXT("Key order is immaterial"), Direct::ParseRequest(Direct::Bytes(Reordered), B, Error));
    TestEqual(TEXT("Canonical replay digest"), A.Digest, B.Digest);
    for (const FString& Bad : {Create.Replace(TEXT("\"version\":1"), TEXT("\"version\":2")),
        Create.Replace(TEXT("\"sequence\":0"), TEXT("\"sequence\":0.0")),
        Create.Replace(TEXT("\"sequence\":0"), TEXT("\"sequence\":0e0")),
        Create.Replace(TEXT("\"sequence\":0"), TEXT("\"sequence\":true")),
        Create.Replace(TEXT("\"sequence\":0"), TEXT("\"sequence\":-1")),
        Create.Replace(TEXT("\"sequence\":0"), TEXT("\"sequence\":0,\"sequence\":0")),
        Create.Replace(TEXT("\"identity\":\"test\""), TEXT("\"identity\":\"test\\u0000\"")),
        Create.Replace(TEXT("\"identity\":\"test\""), TEXT("\"identity\":\"test\\ud800\"")),
        Create.Replace(TEXT("\"identity\":\"test\""), TEXT("\"identity\":\"test\",\"extra\":1")),
        Create + TEXT("{}")}) TestFalse(TEXT("Malformed/ambiguous envelope rejected"), Direct::ParseRequest(Direct::Bytes(Bad), B, Error));
    auto InvalidUtf8 = Direct::Bytes(Create); InvalidUtf8[2] = 255;
    TestFalse(TEXT("Malformed UTF-8 rejected"), Direct::ParseRequest(InvalidUtf8, B, Error));
    TArray<uint8> Huge; Huge.Init(' ', Direct::MaxMessageBytes + 1);
    TestFalse(TEXT("Body bounded before JSON"), Direct::ParseRequest(Huge, B, Error)); TestEqual(TEXT("Size code"), Error, FString(TEXT("PayloadTooLarge")));
    FString Fixture;
    if (!TestTrue(TEXT("Versioned shared fixture is staged"), FFileHelper::LoadFileToString(Fixture, *(FPaths::ProjectConfigDir() / TEXT("SimurghRTC-signaling-v1.json"))))) return false;
    TSharedPtr<FJsonObject> Json;
    if (!TestTrue(TEXT("Fixture parses"), Direct::ParseJson(Direct::Bytes(Fixture), Json, Error))) return false;
    for (const auto& Step : Json->GetArrayField(TEXT("steps")))
    {
        const TSharedPtr<FJsonObject>* Request;
        if (Step->AsObject()->TryGetObjectField(TEXT("request"), Request))
            TestTrue(TEXT("C++ accepts the shared v1 request"), Direct::ParseRequest(Direct::Bytes(Direct::Write(Request->ToSharedRef())), B, Error));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDirectConfigTest, "SimurghRTC.Direct.Configuration", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDirectConfigTest::RunTest(const FString&)
{
    TStrongObjectPtr<UGameInstance> Owner(NewObject<UGameInstance>()); FString Error;
    FDirectClientConfig Config; Config.Identity = TEXT("test"); Config.BearerToken = TEXT("fixture-token");
    for (const FString Url : {TEXT("http://example.com/simurghrtc/v1/signal"), TEXT("http://127.0.0.1:18777/simurghrtc/v1/signal"),
        TEXT("https://user@example.com/simurghrtc/v1/signal"), TEXT("https://example.com/wrong"), TEXT("https://example.com/simurghrtc/v1/signal?x=1")})
    { Config.Endpoint = Url; TestFalse(TEXT("Reject invalid/unencrypted endpoint"), FDirectClient::Connect(Owner.Get(), Config, {}, Error).IsValid()); }
    Config.Endpoint = TEXT("https://example.com/simurghrtc/v1/signal"); Config.BearerToken = TEXT("token\r\nother");
    TestFalse(TEXT("Reject malformed credentials"), FDirectClient::Connect(Owner.Get(), Config, {}, Error).IsValid());
    return true;
}
#endif
