// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "SimurghRTCNativeSignaling.h"
#include "SimurghRTCNativeData.h"

namespace SimurghRTC
{
struct FNativeIceConfig
{
    TArray<FSimurghRTCIceServer> Servers;
    bool bRelayOnly = false;
};
enum class ENativeFactoryMode : uint8 { DataOnly, BuiltinMedia };

/** Evidence about a local factory, never negotiated session capabilities. */
struct FNativeFactoryDiagnostics
{
    bool bFactoryReady = false;
    bool bPeerConnectionReady = false;
    bool bDummyAudioDevice = false;
    bool bAudioProcessing = false;
    TArray<FString> AudioCodecs;
    TArray<FString> VideoCodecs;
};

struct FNativeAudioState
{
    uint32 QueuedInputBlocks = 0, QueuedOutputBlocks = 0;
    uint64 AdmittedBlocks = 0, ConsumedBlocks = 0, UnderrunBlocks = 0, ReceivedBlocks = 0, DroppedOutputBlocks = 0;
    bool bMuted = false, bRemoteTrack = false;
};

/** Internal synchronous adapter. All calls/destruction must use the thread that constructed it. */
class SIMURGHRTCNATIVE_API FNativeRuntime final
{
public:
    FNativeRuntime();
    ~FNativeRuntime();
    FNativeRuntime(const FNativeRuntime&) = delete;
    FNativeRuntime& operator=(const FNativeRuntime&) = delete;

    static bool IsSupported();
    static FString GetBackendVersion();
    // SHA-256 of the isolated backend manifest selected by UBT, including desktop builds.
    static FString GetMobileArtifactSHA256();
    bool Start(ENativeFactoryMode Mode, FString& OutError);
    // Creates an idle, encrypted-by-default Unified Plan peer. No offer, ICE gathering or remote connection.
    bool CreateProbePeerConnection(FString& OutError, bool bLoopbackOnly = false, const FNativeIceConfig& Ice = {});
    FNativeFactoryDiagnostics GetDiagnostics() const;
    bool Signal(uint64 Id, const FNativeSignalRequest& Request, FNativeSignalResult& OutFailure);
    bool PollSignal(FNativeTransportEvent& OutEvent);
    ESimurghRTCErrorCode Data(const FNativeDataRequest& Request, FNativeDataResult& Out);
    bool PollData(FNativeDataResult& Out);
    // Initial internal audio path: configure once before SDP, then submit exactly
    // 10 ms blocks with this format. No device access or physical AEC reference.
    ENativeSignalError ConfigureAudio(int32 SampleRate, int32 Channels);
    ENativeSignalError PushAudio(const FSimurghRTCAudioFrame& Frame);
    ENativeSignalError PollAudio(FSimurghRTCAudioFrame& Out);
    ENativeSignalError AudioState(FNativeAudioState& Out);
    ENativeSignalError MuteAudio(bool bMuted);
    void Stop();

private:
    const uint32 OwnerThreadId;
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
}
