// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCTypes.h"

namespace SimurghRTC
{
// Internal provider API, reusing the frozen common channel/message values.
enum class ENativeChannelState : uint8 { Connecting, Open, Closing, Closed };
enum class ENativeDataEvent : uint8 { None, Changed, Message, Error };
struct FNativeChannelSnapshot
{
    FSimurghRTCChannelInfo Info;
    ENativeChannelState State = ENativeChannelState::Connecting;
    bool bRemote = false;
    bool bWritable = false;
    int64 NegotiatedMaximumMessageBytes = 0;
    int32 QueuedMessages = 0;
};
struct FNativeChannelEvent
{
    ENativeDataEvent Type = ENativeDataEvent::None;
    FNativeChannelSnapshot Channel;
    FSimurghRTCDataMessage Message;
    FSimurghRTCError Error;
};
using FNativeChannelChanged = TUniqueFunction<void(const FNativeChannelEvent&)>;
struct FNativeDataLimits
{
    static constexpr int32 MessageBytes = 65536;
    static constexpr int32 ChannelBufferBytes = 262144;
    static constexpr int32 LowWaterBytes = 65536;
    static constexpr int32 PeerQueuedBytes = 1048576;
    static constexpr int32 QueuedMessages = 64;
    static constexpr int32 LiveChannels = 16;
    static constexpr int32 ChannelRecords = 64;
};

// Worker-side adapter values. No native allocation or WebRTC type crosses the ABI.
enum class ENativeDataAction : uint8 { Open = 1, Send, Close, Query };
struct FNativeDataRequest
{
    ENativeDataAction Action = ENativeDataAction::Query;
    uint64 ChannelId = 0;
    FSimurghRTCChannelOptions Options;
    TArray<uint8> Bytes;
    ESimurghRTCMessageKind Kind = ESimurghRTCMessageKind::Binary;
};
struct FNativeDataResult
{
    uint64 ChannelId = 0;
    ENativeDataEvent Type = ENativeDataEvent::None;
    FNativeChannelSnapshot Channel;
    TArray<uint8> Bytes;
    ESimurghRTCMessageKind Kind = ESimurghRTCMessageKind::Binary;
    ESimurghRTCErrorCode Error = ESimurghRTCErrorCode::None;
};
}
