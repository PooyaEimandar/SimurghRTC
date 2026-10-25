// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativePeerService.h"
namespace SimurghRTC
{
namespace
{
FSimurghRTCError Unavailable() { return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Native peer owner is no longer ready.")); }
}
FNativeDataGame* FNativePeerService::Data(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State)
{
    check(IsInGameThread());
    if (bShuttingDown) return nullptr;
    { FScopeLock Lock(&State->Mutex);
      if (State->Snapshot.State != ENativePeerState::Opening && State->Snapshot.State != ENativePeerState::Ready) return nullptr; }
    for (const auto& Entry : Entries) if (Entry->State == State && Entry->Owner.IsValid() && Entry->Peer.IsValid()) return Entry->Data.Get();
    return nullptr;
}
void FNativePeerService::SubmitData(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State, FNativeDataCommand Data)
{
    if (Data.Action == ENativeDataAction::Query) return;
    FCommand Command; Command.Type = ECommand::Data; Command.State = State; Command.Data = MoveTemp(Data); Enqueue(MoveTemp(Command));
}
FSimurghRTCError FNativePeer::OpenChannel(const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out)
{
    check(IsInGameThread()); Out = {};
    const auto State = GetSnapshot();
    if (State.State != ENativePeerState::Ready || State.Transport.Connection != ENativeConnectionState::Connected || !State.Transport.bDataTransportOpen)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("Connect the peer before opening an application channel."));
    if (const auto P = Service.Pin()) if (auto* Data = P->Data(SharedState))
    {
        FNativeDataCommand Command; auto Error = Data->Open(Options, Out, Command);
        if (!Error.IsError()) P->SubmitData(SharedState, MoveTemp(Command)); return Error;
    }
    return Unavailable();
}
FSimurghRTCError FNativePeer::CloseChannel(FSimurghRTCChannelHandle Channel)
{
    check(IsInGameThread());
    if (const auto P = Service.Pin()) if (auto* Data = P->Data(SharedState))
    {
        FNativeDataCommand Command; auto Error = Data->Close(Channel, Command);
        if (!Error.IsError()) P->SubmitData(SharedState, MoveTemp(Command)); return Error;
    }
    return Unavailable();
}
FSimurghRTCError FNativePeer::GetChannelInfo(FSimurghRTCChannelHandle Channel, FNativeChannelSnapshot& Out) const
{
    check(IsInGameThread()); Out = {};
    if (const auto P = Service.Pin()) if (auto* Data = P->Data(SharedState)) return Data->Get(Channel, Out);
    return Unavailable();
}
FSimurghRTCError FNativePeer::SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes)
{
    check(IsInGameThread());
    if (const auto P = Service.Pin()) if (auto* Data = P->Data(SharedState))
    {
        FNativeDataCommand Command; auto Error = Data->Send(Channel, Bytes, ESimurghRTCMessageKind::Binary, Command);
        if (!Error.IsError()) P->SubmitData(SharedState, MoveTemp(Command)); return Error;
    }
    return Unavailable();
}
FSimurghRTCError FNativePeer::SendText(FSimurghRTCChannelHandle Channel, const FString& Text)
{
    check(IsInGameThread());
    // Reject before conversion/allocation when even UTF-16 length is beyond the byte budget.
    if (Text.Len() > FNativeDataLimits::MessageBytes) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Text exceeds the message limit."));
    FTCHARToUTF8 Utf8(*Text, Text.Len()); FUTF8ToTCHAR Back(Utf8.Get(), Utf8.Length());
    if (Back.Length() != Text.Len() || FMemory::Memcmp(Back.Get(), *Text, Text.Len() * sizeof(TCHAR)) != 0) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Text is not valid Unicode."));
    TArray<uint8> Bytes; Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    if (const auto P = Service.Pin()) if (auto* Data = P->Data(SharedState))
    {
        FNativeDataCommand Command; auto Error = Data->Send(Channel, Bytes, ESimurghRTCMessageKind::Text, Command);
        if (!Error.IsError()) P->SubmitData(SharedState, MoveTemp(Command)); return Error;
    }
    return Unavailable();
}
void FNativePeer::SetChannelHandler(FNativeChannelChanged Handler)
{
    check(IsInGameThread());
    if (const auto P = Service.Pin()) if (auto* Data = P->Data(SharedState)) Data->SetHandler(MoveTemp(Handler));
}
}
