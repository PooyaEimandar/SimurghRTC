// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCNativeSignaling.h"
class UObject;

namespace SimurghRTC
{
enum class EDirectClientState : uint8 { Connecting, Connected, Closing, Closed, Failed };
struct FDirectClientConfig
{
    FString Endpoint; // Full URL ending in /simurghrtc/v1/signal. HTTPS, or explicit loopback HTTP.
    FString BearerToken;
    FString Identity;
    FString Purpose = TEXT("data");
    float RequestTimeoutSeconds = 2;
    float ConnectTimeoutSeconds = 20;
    bool bAllowLoopbackHttp = false;
    bool bLoopbackProbe = false;
    bool bEarlyIceProbe = false;
};
struct FDirectClientSnapshot
{
    EDirectClientState State = EDirectClientState::Connecting;
    FString SessionId, Error;
    FNativeTransportSnapshot Transport;
    int32 AcceptedRequests = 0, Retries = 0, Events = 0;
    bool bNativeClosed = false, bRemoteClosed = false;
};
using FDirectClientChanged = TFunction<void(const FDirectClientSnapshot&)>;

/** Internal Direct provider; the public C++/Blueprint session facade is integrated in SR-105.
 * Game-thread only. Retain the handle and Close at owner shutdown. Close is asynchronous.
 * Dropping the handle cancels callbacks/local work and sends best-effort remote cleanup;
 * the Server lease also removes abandoned sessions. bRemoteClosed confirms an acknowledged close.
 */
class SIMURGHRTCDIRECT_API FDirectClient final
{
public:
    static TSharedPtr<FDirectClient> Connect(UObject* Owner, FDirectClientConfig Config,
        FDirectClientChanged OnChanged, FString& Error);
    ~FDirectClient();
    void Close();
    FDirectClientSnapshot GetSnapshot() const;
private:
    class FImpl;
    explicit FDirectClient(TSharedRef<FImpl> InImpl);
    TSharedRef<FImpl> Impl;
};
}
