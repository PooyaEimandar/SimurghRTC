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

/** Internal plugin integration surface. All calls/destruction must occur on the game thread. */
class SIMURGHRTCNATIVE_API FNativeRuntime final
{
public:
    FNativeRuntime();
    ~FNativeRuntime();
    FNativeRuntime(const FNativeRuntime&) = delete;
    FNativeRuntime& operator=(const FNativeRuntime&) = delete;

    static bool IsSupported();
    bool Start(ENativeFactoryMode Mode, FString& OutError);
    // Creates an idle, encrypted-by-default Unified Plan peer. No offer, ICE gathering or remote connection.
    bool CreateProbePeerConnection(FString& OutError);
    FNativeFactoryDiagnostics GetDiagnostics() const;
    void Stop();

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
}
