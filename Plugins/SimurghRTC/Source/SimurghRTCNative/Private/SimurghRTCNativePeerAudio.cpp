// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativePeerService.h"
#include "Misc/ScopeLock.h"

namespace SimurghRTC
{
FSimurghRTCError FNativeAudioQueue::Push(const FSimurghRTCAudioFrame& Frame)
{
    auto Error = ValidateAudioFrame(Frame);
    if (Error.IsError()) return Error;
    if (!bConfigured || bMuted) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Audio is unavailable or muted."));
    if (Frame.SampleRate != 48000 || Frame.Channels != 1)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::ServiceUnsupported, TEXT("Shared generated audio currently accepts 48 kHz mono PCM."));
    if (Input.Num() + Frame.Samples.Num() > MaxSamples)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::ResourceExhausted, TEXT("Audio submission queue is full."));
    Input.Append(Frame.Samples);
    return {};
}

bool FNativeAudioQueue::Pump(FNativeRuntime& Runtime, uint64& AppliedRevision, int64& Time)
{
    if (!bConfigured) return true;
    if (Revision != AppliedRevision)
    {
        // Flush the native queue even when replacing one unmuted source with another.
        if (Runtime.MuteAudio(true) != ENativeSignalError::None || Runtime.MuteAudio(bMuted) != ENativeSignalError::None) return false;
        AppliedRevision = Revision;
    }
    for (int32 Count = 0; !bMuted && Input.Num() >= 480 && Count < 20; ++Count)
    {
        FSimurghRTCAudioFrame Frame; Frame.TimestampMicroseconds = Time;
        Frame.Samples.Append(Input.GetData(), 480);
        const auto Status = Runtime.PushAudio(Frame);
        if (Status == ENativeSignalError::ResourceExhausted) break;
        if (Status != ENativeSignalError::None) return false;
        Input.RemoveAt(0, 480, EAllowShrinking::No); Time += 10000;
    }
    for (int32 Count = 0; Count < MaxOutputBlocks; ++Count)
    {
        FSimurghRTCAudioFrame Frame;
        if (Runtime.PollAudio(Frame) != ENativeSignalError::None) return false;
        if (Frame.Samples.IsEmpty()) break;
        if (Output.Num() == MaxOutputBlocks) Output.RemoveAt(0, 1, EAllowShrinking::No);
        Output.Add(MoveTemp(Frame));
    }
    FNativeAudioState State;
    if (Runtime.AudioState(State) != ENativeSignalError::None) return false;
    bRemote = State.bRemoteTrack;
    return true;
}

FSimurghRTCError FNativePeer::PushAudio(const FSimurghRTCAudioFrame& Frame)
{
    check(IsInGameThread()); FScopeLock Lock(&SharedState->Mutex);
    if (SharedState->Snapshot.State != ENativePeerState::Ready || SharedState->Snapshot.Transport.Connection != ENativeConnectionState::Connected)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Audio requires a connected peer."));
    return SharedState->Audio.Push(Frame);
}
FSimurghRTCError FNativePeer::MuteAudio(bool bMuted)
{
    check(IsInGameThread()); FScopeLock Lock(&SharedState->Mutex);
    if (SharedState->Snapshot.State != ENativePeerState::Ready || !SharedState->Audio.bConfigured)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("This peer has no audio slot."));
    SharedState->Audio.Mute(bMuted); return {};
}
bool FNativePeer::PollAudio(FSimurghRTCAudioFrame& Out)
{
    check(IsInGameThread()); FScopeLock Lock(&SharedState->Mutex); Out = {};
    if (SharedState->Snapshot.State != ENativePeerState::Ready || SharedState->Audio.Output.IsEmpty()) return false;
    Out = MoveTemp(SharedState->Audio.Output[0]); SharedState->Audio.Output.RemoveAt(0, 1, EAllowShrinking::No); return true;
}
bool FNativePeer::HasRemoteAudio() const
{
    check(IsInGameThread()); FScopeLock Lock(&SharedState->Mutex);
    return SharedState->Snapshot.State == ENativePeerState::Ready && SharedState->Audio.bRemote;
}
}
