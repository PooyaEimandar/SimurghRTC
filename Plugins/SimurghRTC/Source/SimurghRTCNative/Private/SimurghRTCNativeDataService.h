// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCNativeRuntime.h"
#include "Misc/ScopeLock.h"

namespace SimurghRTC
{
struct FNativeDataCommand
{
    ENativeDataAction Action = ENativeDataAction::Query;
    FSimurghRTCChannelHandle Handle;
    FSimurghRTCChannelOptions Options;
    TArray<uint8> Bytes;
    ESimurghRTCMessageKind Kind = ESimurghRTCMessageKind::Binary;
    double Deadline = 0;
};
struct FNativeDataUpdate
{
    FNativeChannelSnapshot Snapshot;
    int32 AcknowledgedBytes = 0, AcknowledgedMessages = 0;
    bool bBlocked = false;
};
struct FNativeDataShared
{
    FCriticalSection Mutex;
    TMap<FGuid, FNativeDataUpdate> Updates;
    TArray<FNativeChannelEvent> Events;
    int32 EventBytes = 0;
    bool bActive = true;
    ESimurghRTCErrorCode Failure = ESimurghRTCErrorCode::None;
};
class FNativeDataGame
{
public:
    explicit FNativeDataGame(TSharedRef<FNativeDataShared, ESPMode::ThreadSafe> InShared) : Shared(InShared) {}
    FSimurghRTCError Open(const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out, FNativeDataCommand& Command);
    FSimurghRTCError Close(FSimurghRTCChannelHandle Handle, FNativeDataCommand& Command);
    FSimurghRTCError Send(FSimurghRTCChannelHandle Handle, const TArray<uint8>& Bytes, ESimurghRTCMessageKind Kind, FNativeDataCommand& Command);
    FSimurghRTCError Get(FSimurghRTCChannelHandle Handle, FNativeChannelSnapshot& Out) const;
    void SetHandler(FNativeChannelChanged Handler) { if (Handler) OnChannel = MakeShared<FNativeChannelChanged>(MoveTemp(Handler)); else OnChannel.Reset(); }
    void Tick(TFunctionRef<bool()> Live);
    void Shutdown();
private:
    struct FChannel
    {
        FNativeChannelSnapshot Native;
        int32 QueuedBytes = 0, QueuedMessages = 0;
        bool bClosing = false, bBlocked = false, bNativeBlocked = false;
    };
    FNativeChannelSnapshot Snapshot(const FChannel& Channel) const;
    void Notify(const FNativeChannelEvent& Event, TFunctionRef<bool()> Live);
    TSharedRef<FNativeDataShared, ESPMode::ThreadSafe> Shared;
    TMap<FGuid, FChannel> Channels;
    TMap<FGuid, FNativeChannelEvent> Notifications;
    TSharedPtr<FNativeChannelChanged> OnChannel;
    int32 QueuedBytes = 0, QueuedMessages = 0;
};
class FNativeDataWorker
{
public:
    FNativeDataWorker(FNativeRuntime& InRuntime, TSharedRef<FNativeDataShared, ESPMode::ThreadSafe> InShared) : Runtime(InRuntime), Shared(InShared) {}
    void Submit(FNativeDataCommand Command);
    bool Poll();
private:
    struct FChannel { uint64 Id = 0; FSimurghRTCChannelHandle Handle; int32 LastState = -1; bool bClosing = false; };
    void Update(const FNativeDataResult& Result, FChannel& Channel, int32 Bytes = 0, int32 Messages = 0, bool bBlocked = false);
    void Emit(FNativeChannelEvent Event);
    void Fail(ESimurghRTCErrorCode Code);
    void Acknowledge(const FNativeDataCommand& Command, FChannel& Channel, const FNativeDataResult& Result);
    FNativeRuntime& Runtime;
    TSharedRef<FNativeDataShared, ESPMode::ThreadSafe> Shared;
    TMap<FGuid, FChannel> Channels;
    TMap<uint64, FGuid> Ids;
    TArray<FNativeDataCommand> Pending;
};
}
