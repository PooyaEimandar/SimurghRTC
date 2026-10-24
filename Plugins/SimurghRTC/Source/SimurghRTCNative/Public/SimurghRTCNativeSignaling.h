// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"

namespace SimurghRTC
{
// Internal transport API. These types do not change the frozen public session contract.
enum class ENativeSignalAction : uint8 { CreateOffer = 1, CreateAnswer, SetRemote, AddIce, EndIce };
enum class ENativeSdpType : uint8 { None, Offer, Answer };
enum class ENativeSignalError : uint8 { None, InvalidArgument, InvalidState, ResourceExhausted, TimedOut, NativeError };
enum class ENativeSignalEvent : uint8 { None, OperationComplete, LocalIce, LocalIceComplete,
    ConnectionChanged, RemoteIceApplied, DataTransportChanged, FatalError };
enum class ENativeConnectionState : uint8 { New, Connecting, Connected, Disconnected, Failed, Closed };

struct FNativeIceCandidate
{
    FString Candidate;
    FString Mid;
    FString UsernameFragment;
    int32 MLineIndex = 0;
};
struct FNativeTransportSnapshot
{
    ENativeConnectionState Connection = ENativeConnectionState::New;
    bool bLocalDescriptionSet = false;
    bool bRemoteDescriptionSet = false;
    bool bLocalIceComplete = false;
    bool bRemoteIceComplete = false;
    bool bDataTransportOpen = false;
    uint32 RemoteIceQueued = 0;
    uint32 RemoteIceApplied = 0;
    // Redacted native diagnostics; enum values are documented in the native peer guide.
    int32 IceState = 0, DtlsState = 0, SelectedProtocol = 0;
    uint32 LocalUdpCandidates = 0, LocalTcpCandidates = 0, CandidateErrors = 0, LastCandidateError = 0;
    uint32 StatsSamples = 0, StatsAgeMs = 0, CandidatePairs = 0, SucceededPairs = 0, FailedPairs = 0;
    uint64 StunRequestsSent = 0, StunResponsesReceived = 0, PacketsDiscardedOnSend = 0;
};
struct FNativeSignalRequest
{
    ENativeSignalAction Action = ENativeSignalAction::CreateOffer;
    ENativeSdpType SdpType = ENativeSdpType::None;
    FString Sdp;
    FNativeIceCandidate Ice;
    uint32 TimeoutMs = 30000;
};
struct FNativeSignalResult
{
    uint64 OperationId = 0;
    ENativeSignalError Error = ENativeSignalError::None;
    FString Message;
    ENativeSdpType SdpType = ENativeSdpType::None;
    FString Sdp;
    // Queued ICE is acknowledged before the remote description; application can fail later.
    bool bQueued = false;
    bool IsSuccess() const { return Error == ENativeSignalError::None; }
};
struct FNativeTransportEvent
{
    ENativeSignalEvent Type = ENativeSignalEvent::None;
    FNativeSignalResult Result;
    FNativeIceCandidate Ice;
    FNativeTransportSnapshot Transport;
};
using FNativeSignalCompleted = TUniqueFunction<void(const FNativeSignalResult&)>;
using FNativeTransportChanged = TUniqueFunction<void(const FNativeTransportEvent&)>;
}
