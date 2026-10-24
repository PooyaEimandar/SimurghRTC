// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "SimurghRTCNativeRuntime.h"

class UObject;

namespace SimurghRTC
{
class FNativePeerService;
struct FNativePeerSharedState;

enum class ENativePeerState : uint8 { Opening, Ready, Failed, Closing, Closed };

/** Ready means an idle local peer exists. It does not mean a network connection exists. */
struct FNativePeerSnapshot
{
    ENativePeerState State = ENativePeerState::Opening;
    FNativeFactoryDiagnostics Diagnostics;
    FString Error;
};

using FNativePeerOpened = TUniqueFunction<void(const FNativePeerSnapshot&)>;

/**
 * Internal plugin handle, not part of the frozen C++/Blueprint session contract.
 * Open, inspection, Close and destruction are game-thread only. Native work uses a dedicated thread.
 * Keep the handle alive until Close. Dropping it, losing LifetimeOwner or world cleanup requests close.
 * Explicitly Close during EndPlay/Shutdown; owner invalidation is also checked on each game tick.
 */
class SIMURGHRTCNATIVE_API FNativePeer final
{
public:
    using FHandle = TSharedPtr<FNativePeer, ESPMode::ThreadSafe>;

    /**
     * Returns null with OutError for invalid input, unavailable backend/thread, shutdown or capacity.
     * Otherwise OnOpened runs once on a later game tick with Ready or Failed, while owner/handle live.
     * Close (including cancellation before readiness) suppresses any undelivered callback.
     * This creates no SDP, gathers no ICE and sends no network traffic.
     */
    static FHandle Open(UObject* LifetimeOwner, ENativeFactoryMode Mode,
        FNativePeerOpened OnOpened, FString& OutError);

    ~FNativePeer();
    FNativePeer(const FNativePeer&) = delete;
    FNativePeer& operator=(const FNativePeer&) = delete;

    FNativePeerSnapshot GetSnapshot() const;
    // Idempotent and nonblocking: Closed is published after native teardown finishes.
    void Close();

private:
    friend class FNativePeerService;
    FNativePeer(TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe> InState,
        TWeakPtr<FNativePeerService, ESPMode::ThreadSafe> InService);

    TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe> SharedState;
    TWeakPtr<FNativePeerService, ESPMode::ThreadSafe> Service;
};
}
