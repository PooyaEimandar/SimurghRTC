// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

namespace SimurghRTC
{
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
    bool CreateProbePeerConnection(FString& OutError);
    FNativeFactoryDiagnostics GetDiagnostics() const;
    void Stop();

private:
    const uint32 OwnerThreadId;
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
}
