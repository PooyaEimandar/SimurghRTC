// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeDataService.h"

namespace SimurghRTC
{
namespace
{
FSimurghRTCError InvalidChannel() { return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Unknown, foreign or closed channel.")); }
FSimurghRTCError Full() { return SimurghRTC::MakeError(ESimurghRTCErrorCode::ResourceExhausted, TEXT("DataChannel queue is full; wait for a writable event.")); }
bool ValidText(const TArray<uint8>& Bytes)
{
    if (Bytes.IsEmpty()) return true;
    FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    FTCHARToUTF8 Back(Text.Get(), Text.Length());
    return Back.Length() == Bytes.Num() && FMemory::Memcmp(Back.Get(), Bytes.GetData(), Bytes.Num()) == 0;
}
}
FNativeChannelSnapshot FNativeDataGame::Snapshot(const FChannel& C) const
{
    auto Out = C.Native;
    if (C.bClosing && Out.State != ENativeChannelState::Closed) Out.State = ENativeChannelState::Closing;
    Out.Info.bOpen = Out.State == ENativeChannelState::Open;
    Out.Info.BufferedBytes += C.QueuedBytes; Out.QueuedMessages = C.QueuedMessages;
    Out.bWritable = Out.Info.bOpen && Out.Info.MaximumMessageBytes > 0 && !C.bBlocked && !C.bNativeBlocked
        && Out.Info.BufferedBytes < FNativeDataLimits::ChannelBufferBytes && QueuedMessages < FNativeDataLimits::QueuedMessages
        && QueuedBytes < FNativeDataLimits::PeerQueuedBytes;
    return Out;
}
FSimurghRTCError FNativeDataGame::Get(FSimurghRTCChannelHandle Handle, FNativeChannelSnapshot& Out) const
{
    check(IsInGameThread()); Out = {};
    const auto* C = Channels.Find(Handle.Id); if (!C) return InvalidChannel();
    Out = Snapshot(*C); return {};
}
FSimurghRTCError FNativeDataGame::Open(const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out, FNativeDataCommand& Command)
{
    check(IsInGameThread()); Out = {};
    auto Error = ValidateChannelOptions(Options); if (Error.IsError()) return Error;
    FTCHARToUTF8 Label(*Options.Label, Options.Label.Len());
    FUTF8ToTCHAR Back(Label.Get(), Label.Length());
    if (Back.Length() != Options.Label.Len() || FMemory::Memcmp(Back.Get(), *Options.Label, Options.Label.Len() * sizeof(TCHAR)) != 0) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Channel label is not valid Unicode."));
    int32 Live = 0;
    for (const auto& Pair : Channels) if (Pair.Value.Native.State != ENativeChannelState::Closed) ++Live;
    if (Live >= FNativeDataLimits::LiveChannels || Channels.Num() >= FNativeDataLimits::ChannelRecords) return Full();
    Out.Id = FGuid::NewGuid(); auto& C = Channels.Add(Out.Id);
    C.Native.Info.Handle = Out; C.Native.Info.Options = Options;
    Command.Action = ENativeDataAction::Open; Command.Handle = Out; Command.Options = Options;
    FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed; Event.Channel = Snapshot(C); Notifications.Add(Out.Id, MoveTemp(Event));
    return {};
}
FSimurghRTCError FNativeDataGame::Close(FSimurghRTCChannelHandle Handle, FNativeDataCommand& Command)
{
    check(IsInGameThread()); auto* C = Channels.Find(Handle.Id); if (!C) return InvalidChannel();
    if (C->bClosing || C->Native.State == ENativeChannelState::Closed) return {};
    C->bClosing = true; Command.Action = ENativeDataAction::Close; Command.Handle = Handle;
    FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed; Event.Channel = Snapshot(*C); Notifications.Add(Handle.Id, MoveTemp(Event));
    return {};
}
FSimurghRTCError FNativeDataGame::Send(FSimurghRTCChannelHandle Handle, const TArray<uint8>& Bytes, ESimurghRTCMessageKind Kind, FNativeDataCommand& Command)
{
    check(IsInGameThread()); auto* C = Channels.Find(Handle.Id);
    if (!C || C->bClosing || C->Native.State != ENativeChannelState::Open) return InvalidChannel();
    if (C->Native.Info.MaximumMessageBytes <= 0) return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("Channel message limit is unknown."));
    if (Bytes.Num() > C->Native.Info.MaximumMessageBytes || Bytes.Num() > FNativeDataLimits::MessageBytes || (Kind == ESimurghRTCMessageKind::Text && !ValidText(Bytes)))
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Message exceeds the negotiated limit or text is not valid UTF-8."));
    if (C->bBlocked || C->bNativeBlocked || C->QueuedBytes + C->Native.Info.BufferedBytes + Bytes.Num() > FNativeDataLimits::ChannelBufferBytes
        || QueuedBytes + Bytes.Num() > FNativeDataLimits::PeerQueuedBytes || QueuedMessages >= FNativeDataLimits::QueuedMessages)
    {
        if (!C->bBlocked)
        {
            C->bBlocked = true; FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed; Event.Channel = Snapshot(*C);
            Notifications.Add(Handle.Id, MoveTemp(Event));
        }
        return Full();
    }
    C->QueuedBytes += Bytes.Num(); ++C->QueuedMessages; QueuedBytes += Bytes.Num(); ++QueuedMessages;
    Command.Action = ENativeDataAction::Send; Command.Handle = Handle; Command.Bytes = Bytes; Command.Kind = Kind;
    Command.Deadline = FPlatformTime::Seconds() + 30;
    return {};
}
void FNativeDataGame::Notify(const FNativeChannelEvent& Event, TFunctionRef<bool()> Live)
{
    if (!Live()) return;
    const auto Callback = OnChannel; if (Callback) (*Callback)(Event);
}
void FNativeDataGame::Shutdown()
{
    check(IsInGameThread()); OnChannel.Reset(); Notifications.Reset(); Channels.Reset(); QueuedBytes = QueuedMessages = 0;
    FScopeLock Lock(&Shared->Mutex); Shared->bActive = false; Shared->Events.Reset(); Shared->Updates.Reset(); Shared->EventBytes = 0;
}
void FNativeDataGame::Tick(TFunctionRef<bool()> Live)
{
    check(IsInGameThread());
    auto Local = MoveTemp(Notifications); Notifications.Reset();
    for (const auto& Pair : Local) { Notify(Pair.Value, Live); if (!Live()) return; }
    TMap<FGuid, FNativeDataUpdate> Updates; TArray<FNativeChannelEvent> Events;
    {
        FScopeLock Lock(&Shared->Mutex);
        Updates = MoveTemp(Shared->Updates); Shared->Updates.Reset();
        Events = MoveTemp(Shared->Events); Shared->Events.Reset(); Shared->EventBytes = 0;
    }
    TSet<FGuid> Changed;
    for (const auto& Pair : Updates)
    {
        if (!Channels.Contains(Pair.Key) && Channels.Num() >= FNativeDataLimits::ChannelRecords)
        { FScopeLock Lock(&Shared->Mutex); Shared->Failure = ESimurghRTCErrorCode::ResourceExhausted; return; }
        auto& C = Channels.FindOrAdd(Pair.Key); const auto& U = Pair.Value;
        if (!C.Native.Info.Handle.IsValid()) { C.Native = U.Snapshot; C.Native.State = ENativeChannelState::Connecting; }
        C.QueuedBytes -= U.AcknowledgedBytes; C.QueuedMessages -= U.AcknowledgedMessages;
        QueuedBytes -= U.AcknowledgedBytes; QueuedMessages -= U.AcknowledgedMessages;
        check(C.QueuedBytes >= 0 && C.QueuedMessages >= 0 && QueuedBytes >= 0 && QueuedMessages >= 0);
        C.Native.Info.BufferedBytes = U.Snapshot.Info.BufferedBytes;
        C.Native.Info.MaximumMessageBytes = U.Snapshot.Info.MaximumMessageBytes;
        C.Native.NegotiatedMaximumMessageBytes = U.Snapshot.NegotiatedMaximumMessageBytes;
        C.bNativeBlocked = U.bBlocked;
        Changed.Add(Pair.Key);
    }
    for (auto& Event : Events)
    {
        if (!Live()) return;
        auto* C = Channels.Find(Event.Channel.Info.Handle.Id); if (!C) continue;
        if (Event.Type == ENativeDataEvent::Message)
        {
            if (C->bClosing) continue;
            Event.Channel = Snapshot(*C); Notify(Event, Live);
        }
        else
        {
            // Command results can sample Open before older queued Connecting events arrive.
            // A channel never returns to an earlier lifecycle state.
            if (uint8(Event.Channel.State) < uint8(C->Native.State)) continue;
            C->Native.State = Event.Channel.State; C->Native.Info.Options = Event.Channel.Info.Options;
            C->Native.bRemote = Event.Channel.bRemote;
            Event.Channel = Snapshot(*C); Notify(Event, Live);
        }
    }
    for (auto& Pair : Channels)
    {
        auto& C = Pair.Value;
        if (C.bBlocked && !C.bNativeBlocked && C.QueuedBytes + C.Native.Info.BufferedBytes <= FNativeDataLimits::LowWaterBytes
            && QueuedMessages < FNativeDataLimits::QueuedMessages / 2 && QueuedBytes <= FNativeDataLimits::PeerQueuedBytes / 2)
        { C.bBlocked = false; Changed.Add(Pair.Key); }
    }
    for (const auto& Id : Changed)
    {
        if (!Live()) return;
        const auto* C = Channels.Find(Id); if (!C) continue;
        FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed; Event.Channel = Snapshot(*C); Notify(Event, Live);
    }
}
void FNativeDataWorker::Fail(ESimurghRTCErrorCode Code)
{
    FScopeLock Lock(&Shared->Mutex); if (Shared->bActive && Shared->Failure == ESimurghRTCErrorCode::None) Shared->Failure = Code;
}
void FNativeDataWorker::Emit(FNativeChannelEvent Event)
{
    const int32 Bytes = Event.Message.Bytes.Num() + Event.Message.Text.Len() * sizeof(TCHAR);
    FScopeLock Lock(&Shared->Mutex); if (!Shared->bActive) return;
    if (Shared->Events.Num() >= 64 || Shared->EventBytes + Bytes > FNativeDataLimits::PeerQueuedBytes)
    { Shared->Failure = ESimurghRTCErrorCode::ResourceExhausted; return; }
    Shared->EventBytes += Bytes; Shared->Events.Add(MoveTemp(Event));
}
void FNativeDataWorker::Update(const FNativeDataResult& R, FChannel& C, int32 Bytes, int32 Messages, bool bBlocked)
{
    auto Snapshot = R.Channel; Snapshot.Info.Handle = C.Handle;
    {
        FScopeLock Lock(&Shared->Mutex); if (!Shared->bActive) return;
        auto& U = Shared->Updates.FindOrAdd(C.Handle.Id);
        U.Snapshot = Snapshot; U.AcknowledgedBytes += Bytes; U.AcknowledgedMessages += Messages; U.bBlocked = bBlocked;
    }
    if (C.LastState != int32(Snapshot.State))
    {
        C.LastState = int32(Snapshot.State);
        FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Changed; Event.Channel = Snapshot; Emit(MoveTemp(Event));
    }
}
void FNativeDataWorker::Acknowledge(const FNativeDataCommand& Command, FChannel& C, const FNativeDataResult& R)
{ Update(R, C, Command.Bytes.Num(), 1); }
void FNativeDataWorker::Submit(FNativeDataCommand Command)
{
    if (Command.Action == ENativeDataAction::Send) { Pending.Add(MoveTemp(Command)); return; }
    if (Command.Action == ENativeDataAction::Open)
    {
        FNativeDataRequest Q; Q.Action = ENativeDataAction::Open; Q.Options = Command.Options;
        FNativeDataResult R; const auto Error = Runtime.Data(Q, R);
        FChannel C; C.Id = R.ChannelId; C.Handle = Command.Handle;
        if (Error != ESimurghRTCErrorCode::None) { R.Channel.State = ENativeChannelState::Closed; R.Channel.Info.Options = Command.Options; }
        auto& Stored = Channels.Add(C.Handle.Id, C); if (C.Id) Ids.Add(C.Id, C.Handle.Id);
        Update(R, Stored);
        if (Error != ESimurghRTCErrorCode::None)
        {
            FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Error; Event.Channel = R.Channel; Event.Channel.Info.Handle = C.Handle;
            Event.Error = SimurghRTC::MakeError(Error, TEXT("Native channel creation was rejected.")); Emit(MoveTemp(Event));
        }
        return;
    }
    if (auto* C = Channels.Find(Command.Handle.Id))
    {
        C->bClosing = true;
        FNativeDataRequest Q; Q.Action = ENativeDataAction::Close; Q.ChannelId = C->Id;
        FNativeDataResult R; const auto Error = Runtime.Data(Q, R);
        if (Error != ESimurghRTCErrorCode::None) R.Channel.State = ENativeChannelState::Closed;
        for (int32 I = Pending.Num() - 1; I >= 0; --I) if (Pending[I].Handle.Id == C->Handle.Id)
        { Acknowledge(Pending[I], *C, R); Pending.RemoveAt(I); }
        Update(R, *C);
    }
}
bool FNativeDataWorker::Poll()
{
    {
        FScopeLock Lock(&Shared->Mutex);
        if (!Shared->bActive) return true;
        if (Shared->Failure != ESimurghRTCErrorCode::None) return false;
    }
    TSet<FGuid> Blocked;
    for (int32 I = 0, Tried = 0; I < Pending.Num() && Tried < 16;)
    {
        auto& Command = Pending[I]; auto* C = Channels.Find(Command.Handle.Id);
        if (!C) { Fail(ESimurghRTCErrorCode::InvalidState); return false; }
        if (Blocked.Contains(C->Handle.Id)) { ++I; continue; }
        ++Tried;
        if (!C->bClosing && FPlatformTime::Seconds() >= Command.Deadline) { Fail(ESimurghRTCErrorCode::Timeout); return false; }
        FNativeDataRequest Q; Q.Action = C->bClosing ? ENativeDataAction::Query : ENativeDataAction::Send;
        Q.ChannelId = C->Id; Q.Kind = Command.Kind;
        if (!C->bClosing) Q.Bytes = MoveTemp(Command.Bytes);
        FNativeDataResult R; const auto Error = Runtime.Data(Q, R);
        if (!C->bClosing) Command.Bytes = MoveTemp(Q.Bytes);
        if (!C->bClosing && Error == ESimurghRTCErrorCode::ResourceExhausted && FPlatformTime::Seconds() < Command.Deadline)
        { Update(R, *C, 0, 0, true); Blocked.Add(C->Handle.Id); ++I; continue; }
        if (!C->bClosing && Error != ESimurghRTCErrorCode::None)
        { Fail(Error == ESimurghRTCErrorCode::ResourceExhausted ? ESimurghRTCErrorCode::Timeout : Error); return false; }
        Acknowledge(Command, *C, R); Pending.RemoveAt(I);
    }
    for (int32 I = 0; I < 16; ++I)
    {
        FNativeDataResult R;
        if (!Runtime.PollData(R)) { Fail(ESimurghRTCErrorCode::InternalError); return false; }
        if (R.Type == ENativeDataEvent::None) break;
        if (R.Type == ENativeDataEvent::Error) { Fail(R.Error); return false; }
        FGuid* Id = Ids.Find(R.ChannelId);
        if (!Id)
        {
            if (Channels.Num() >= FNativeDataLimits::ChannelRecords) { Fail(ESimurghRTCErrorCode::ResourceExhausted); return false; }
            FChannel C; C.Id = R.ChannelId; C.Handle.Id = FGuid::NewGuid();
            Channels.Add(C.Handle.Id, C); Ids.Add(C.Id, C.Handle.Id); Id = Ids.Find(C.Id);
        }
        auto& C = Channels.FindChecked(*Id);
        // State/message events are FIFO; buffer credit must use a current sample, not an
        // older event captured before a send was acknowledged.
        FNativeDataRequest Query; Query.Action = ENativeDataAction::Query; Query.ChannelId = C.Id;
        FNativeDataResult Current;
        if (Runtime.Data(Query, Current) != ESimurghRTCErrorCode::None) { Fail(ESimurghRTCErrorCode::InternalError); return false; }
        R.Channel.Info.BufferedBytes = Current.Channel.Info.BufferedBytes;
        R.Channel.Info.MaximumMessageBytes = Current.Channel.Info.MaximumMessageBytes;
        R.Channel.NegotiatedMaximumMessageBytes = Current.Channel.NegotiatedMaximumMessageBytes;
        Update(R, C, 0, 0, Blocked.Contains(C.Handle.Id));
        if (R.Type == ENativeDataEvent::Message && !C.bClosing)
        {
            FNativeChannelEvent Event; Event.Type = ENativeDataEvent::Message;
            Event.Channel = R.Channel; Event.Channel.Info.Handle = C.Handle;
            Event.Message.Channel = C.Handle; Event.Message.Kind = R.Kind;
            if (R.Kind == ESimurghRTCMessageKind::Binary) Event.Message.Bytes = MoveTemp(R.Bytes);
            else if (!R.Bytes.IsEmpty())
            {
                FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(R.Bytes.GetData()), R.Bytes.Num());
                Event.Message.Text = FString(Text.Length(), Text.Get());
            }
            Emit(MoveTemp(Event));
        }
    }
    FScopeLock Lock(&Shared->Mutex); return Shared->Failure == ESimurghRTCErrorCode::None;
}
}
