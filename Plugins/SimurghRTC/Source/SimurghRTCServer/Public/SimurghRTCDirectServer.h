// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCNativeSignaling.h"
#include "SimurghRTCNativeData.h"
class UObject;
class IHttpRouter;

namespace SimurghRTC
{
struct FDirectAdmissionRequest
{
    FString BearerToken;
    FString Purpose, Identity; // Create metadata only; identity is not authentication.
    bool bCreate = false;
};
struct FDirectAdmissionResult
{
    FString Principal; // Stable authenticated identity; empty rejects the request.
    bool bAllowCreate = false;
};
struct FDirectServerConfig
{
    TFunction<FDirectAdmissionResult(const FDirectAdmissionRequest&)> Admit;
    TFunction<void(const FString& SessionId, const FNativeChannelEvent&)> OnChannelEvent;
    int32 MaxSessions = 16;
    double IdleSeconds = 30;
    double ReplaySeconds = 30;
    bool bLoopbackProbe = false; // Explicit same-host tests only.
};
struct FDirectServerSnapshot
{
    int32 LivePeers = 0, Records = 0, Created = 0, Connected = 0, Closed = 0, Expired = 0;
    int32 Replays = 0, Rejected = 0, EarlyIce = 0;
    bool bRunning = false;
    FNativeTransportSnapshot LastConnectedTransport;
    TArray<FString> ConnectedSessionIds, ClosedSessionIds; // Most recent 64; no credentials.
};

/** Internal provider API. Game-thread only. The host owns the HTTP listener and TLS deployment.
 * Bind installs one route on the supplied router; Stop removes only that route and closes peers.
 * Retain the handle, call Stop at owner shutdown, then wait for LivePeers == 0 when validating cleanup.
 */
class SIMURGHRTCSERVER_API FDirectServer final
{
public:
    static TSharedPtr<FDirectServer> Bind(UObject* Owner, TSharedRef<IHttpRouter> Router,
        FDirectServerConfig Config, FString& Error);
    ~FDirectServer();
    void Stop();
    FSimurghRTCError OpenChannel(const FString& SessionId, const FSimurghRTCChannelOptions& Options, FSimurghRTCChannelHandle& Out);
    FSimurghRTCError CloseChannel(const FString& SessionId, FSimurghRTCChannelHandle Channel);
    FSimurghRTCError GetChannelInfo(const FString& SessionId, FSimurghRTCChannelHandle Channel, FNativeChannelSnapshot& Out) const;
    FSimurghRTCError SendText(const FString& SessionId, FSimurghRTCChannelHandle Channel, const FString& Text);
    FSimurghRTCError SendBytes(const FString& SessionId, FSimurghRTCChannelHandle Channel, const TArray<uint8>& Bytes);

    FDirectServerSnapshot GetSnapshot() const;
private:
    class FImpl;
    explicit FDirectServer(TSharedRef<FImpl> InImpl);
    TSharedRef<FImpl> Impl;
};
}
