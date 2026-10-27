// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeDataService.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
using namespace SimurghRTC;
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeDataAdmission, "SimurghRTC.Native.DataAdmissionAndLifetime",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FNativeDataAdmission::RunTest(const FString&)
{
    auto Shared = MakeShared<FNativeDataShared, ESPMode::ThreadSafe>(); FNativeDataGame Game(Shared);
    FSimurghRTCChannelOptions Options; Options.Label = TEXT("test"); FSimurghRTCChannelHandle Handle;
    FNativeDataCommand Command;
    TestFalse(TEXT("Open admitted"), Game.Open(Options, Handle, Command).IsError());
    TestEqual(TEXT("Send before open rejected"), Game.Send(Handle, {}, ESimurghRTCMessageKind::Binary, Command).Code, ESimurghRTCErrorCode::InvalidState);
    auto& U = Shared->Updates.Add(Handle.Id); U.Snapshot.Info.Handle = Handle;
    U.Snapshot.Info.MaximumMessageBytes = 65536; U.Snapshot.State = ENativeChannelState::Open;
    FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed; Event.Channel = U.Snapshot; Shared->Events.Add(Event);
    Game.Tick([] { return true; });
    TArray<uint8> Payload; Payload.Init(37, 65536);
    for (int32 I = 0; I < 4; ++I) TestFalse(TEXT("Maximum-size send admitted"), Game.Send(Handle, Payload, ESimurghRTCMessageKind::Binary, Command).IsError());
    Payload[0] = 9; TestEqual(TEXT("Caller mutation does not change queued copy"), Command.Bytes[0], uint8(37));
    TestEqual(TEXT("Byte budget enforced"), Game.Send(Handle, Payload, ESimurghRTCMessageKind::Binary, Command).Code, ESimurghRTCErrorCode::ResourceExhausted);
    bool Blocked = false, Resumed = false;
    Game.SetHandler([&](const FNativeChannelEvent& E) { if (!E.Channel.bWritable) Blocked = true; else if (Blocked) Resumed = true; });
    auto& Ack = Shared->Updates.Add(Handle.Id); Ack.Snapshot = Event.Channel; Ack.AcknowledgedBytes = 262144; Ack.AcknowledgedMessages = 4;
    Game.Tick([] { return true; });
    TestTrue(TEXT("Blocked callback survives same-tick drain"), Blocked); TestTrue(TEXT("Writable resumes after drain"), Resumed);
    for (int32 I = 0; I < 64; ++I) TestFalse(TEXT("Empty message admitted within count limit"), Game.Send(Handle, {}, ESimurghRTCMessageKind::Binary, Command).IsError());
    TestEqual(TEXT("Empty messages consume queue slots"), Game.Send(Handle, {}, ESimurghRTCMessageKind::Binary, Command).Code, ESimurghRTCErrorCode::ResourceExhausted);
    FSimurghRTCChannelHandle Foreign; Foreign.Id = FGuid::NewGuid(); FNativeChannelSnapshot Snapshot;
    TestEqual(TEXT("Foreign handle rejected"), Game.Get(Foreign, Snapshot).Code, ESimurghRTCErrorCode::InvalidState);
    TSharedPtr<int> Capture = MakeShared<int>(1); TWeakPtr<int> Weak = Capture;
    Game.SetHandler([Capture](const FNativeChannelEvent&) {}); Capture.Reset();
    TestTrue(TEXT("Callback capture retained"), Weak.IsValid()); Game.Shutdown();
    TestFalse(TEXT("Shutdown releases callback synchronously"), Weak.IsValid());
    TestFalse(TEXT("Worker delivery disabled"), Shared->bActive);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeDataStateOrder, "SimurghRTC.Native.DataStateNeverRegresses",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FNativeDataStateOrder::RunTest(const FString&)
{
    auto Shared = MakeShared<FNativeDataShared, ESPMode::ThreadSafe>(); FNativeDataGame Game(Shared);
    FSimurghRTCChannelHandle Handle; FNativeDataCommand Command;
    Game.Open({},Handle,Command);
    FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed;
    Event.Channel.Info.Handle = Handle; Event.Channel.Info.MaximumMessageBytes = 65536;
    Event.Channel.State = ENativeChannelState::Open;
    Shared->Updates.Add(Handle.Id).Snapshot = Event.Channel; Shared->Events.Add(Event);
    Game.Tick([] { return true; });
    // The worker's Open command may observe Open before Poll delivers an older Connecting event.
    Event.Channel.State = ENativeChannelState::Connecting; Shared->Events.Add(Event);
    Game.Tick([] { return true; }); FNativeChannelSnapshot Snapshot; Game.Get(Handle,Snapshot);
    TestEqual(TEXT("Stale Connecting cannot revoke Open"),Snapshot.State,ENativeChannelState::Open);
    TestFalse(TEXT("Sending after successful open remains admitted"),Game.Send(Handle,{},ESimurghRTCMessageKind::Binary,Command).IsError());
    Event.Channel.State = ENativeChannelState::Closed; Shared->Events.Add(Event); Game.Tick([] { return true; });
    Event.Channel.State = ENativeChannelState::Open; Shared->Events.Add(Event); Game.Tick([] { return true; });
    Game.Get(Handle,Snapshot);
    TestEqual(TEXT("Stale Open cannot revive Closed"),Snapshot.State,ENativeChannelState::Closed);
    TestEqual(TEXT("Closed channel still rejects sends"),Game.Send(Handle,{},ESimurghRTCMessageKind::Binary,Command).Code,ESimurghRTCErrorCode::InvalidState);
    Game.Shutdown(); return true;
}
#endif
