// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCMediaOperation.h"
#include "SimurghRTCSession.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimurghRTCMediaContractTest, "SimurghRTC.Contract.ClientMedia",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FSimurghRTCMediaContractTest::RunTest(const FString&)
{
    TArray<FSimurghRTCDeviceInfo> Devices;
    Devices.AddDefaulted();
    auto Error = USimurghRTCMediaOperation::GetDevices(ESimurghRTCSourceKind::Microphone, Devices);
    TestEqual(TEXT("Discovery does not claim no devices or permission success"), Error.Code, ESimurghRTCErrorCode::NotImplemented);
    TestTrue(TEXT("Discovery clears stale caller output"), Devices.IsEmpty());
    Error = USimurghRTCMediaOperation::GetDevices(ESimurghRTCSourceKind::GeneratedVideo, Devices);
    TestEqual(TEXT("Generated sources are not enumerated devices"), Error.Code, ESimurghRTCErrorCode::InvalidArgument);

    TStrongObjectPtr<UGameInstance> Owner(NewObject<UGameInstance>());
    TStrongObjectPtr<USimurghRTCSession> Session(USimurghRTCSession::CreateSession(Owner.Get()));
    FSimurghRTCSourceOptions Options; Options.Kind = ESimurghRTCSourceKind::GeneratedAudio;
    TStrongObjectPtr<USimurghRTCMediaOperation> Source(USimurghRTCMediaOperation::CreateSource(Session.Get(), Options));
    Source->Activate(); FTSTicker::GetCoreTicker().Tick(0.0f);
    TestEqual(TEXT("Local source creation can precede Connect and reaches the implementation boundary"),
        Source->GetResult().Error.Code, ESimurghRTCErrorCode::NotImplemented);
    TestFalse(TEXT("No source handle is fabricated"), Source->GetResult().Source.IsValid());
    Options.Kind = ESimurghRTCSourceKind::RenderTarget;
    TStrongObjectPtr<USimurghRTCMediaOperation> Invalid(USimurghRTCMediaOperation::CreateSource(Session.Get(), Options));
    Invalid->Activate(); FTSTicker::GetCoreTicker().Tick(0.0f);
    TestEqual(TEXT("A render target reference is required"), Invalid->GetResult().Error.Code, ESimurghRTCErrorCode::InvalidArgument);
    Options.Kind = static_cast<ESimurghRTCSourceKind>(StaticEnum<ESimurghRTCSourceKind>()->GetMaxEnumValue());
    TStrongObjectPtr<USimurghRTCMediaOperation> InvalidKind(USimurghRTCMediaOperation::CreateSource(Session.Get(), Options));
    InvalidKind->Activate(); FTSTicker::GetCoreTicker().Tick(0.0f);
    TestEqual(TEXT("Generated source MAX is not a capture kind"), InvalidKind->GetResult().Error.Code, ESimurghRTCErrorCode::InvalidArgument);
    Session->Shutdown();
    return true;
}
#endif
