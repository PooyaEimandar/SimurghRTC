// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCNativeSignaling.h"
#include "SimurghRTCNativeData.h"
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
    TArray<FSimurghRTCIceServer> IceServers;
    bool bRelayOnly = false;
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
    static FSimurghRTCError ValidateConfig(const FDirectClientConfig& Config);
    static TSharedPtr<FDirectClient> Connect(UObject* Owner, FDirectClientConfig Config,
        FDirectClientChanged OnChanged, FString& Error);
    ~FDirectClient();
    void Close();
    FSimurghRTCError OpenChannel(const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out);
    FSimurghRTCError CloseChannel(FSimurghRTCChannelHandle Channel);
    FSimurghRTCError GetChannelInfo(FSimurghRTCChannelHandle Channel, FNativeChannelSnapshot& Out) const;
    FSimurghRTCError SendText(FSimurghRTCChannelHandle Channel, const FString& Text);
    FSimurghRTCError SendBytes(FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes);
    void SetChannelHandler(FNativeChannelChanged Handler);
    FSimurghRTCError PushAudio(const FSimurghRTCAudioFrame& Frame);
    FSimurghRTCError MuteAudio(bool bMuted);
    bool PollAudio(FSimurghRTCAudioFrame& Out);
    bool HasRemoteAudio() const;
    FDirectClientSnapshot GetSnapshot() const;
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FDirectNativeFailureOrderTest;
#endif
    class FImpl;
    explicit FDirectClient(TSharedRef<FImpl> InImpl);
    TSharedRef<FImpl> Impl;
};
}
