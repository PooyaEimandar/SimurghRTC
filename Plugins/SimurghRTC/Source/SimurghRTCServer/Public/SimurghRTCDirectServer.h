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
enum class EDirectServerSessionState : uint8 { Connecting, Connected, Closing, Closed, Failed };
struct FDirectSessionInfo
{
    FString SessionId, Principal, Identity, Purpose;
    EDirectServerSessionState State = EDirectServerSessionState::Connecting;
    bool bNativeClosed = false;
};
struct FDirectChannelAddress
{
    FString SessionId;
    FSimurghRTCChannelHandle Channel;
};
struct FDirectRouteResult
{
    FDirectChannelAddress Destination;
    FSimurghRTCError Error; // Success means copied for sending, not delivered.
};
struct FDirectServerConfig
{
    TFunction<FDirectAdmissionResult(const FDirectAdmissionRequest&)> Admit;
    TFunction<void(const FString& SessionId, const FNativeChannelEvent&)> OnChannelEvent;
    TFunction<void(const FDirectSessionInfo&)> OnSessionChanged;
    // Cross-session routing is denied without this policy. Principal is authenticated;
    // Identity/Purpose are client metadata. Self-echo does not require this callback.
    TFunction<bool(const FDirectSessionInfo& Source, const FDirectSessionInfo& Destination,
        ESimurghRTCMessageKind Kind)> CanRoute;
    int32 MaxSessions = 16;
    int32 MaxSessionsPerPrincipal = 4;
    double IdleSeconds = 30;
    double ReplaySeconds = 30;
    TArray<FSimurghRTCIceServer> IceServers;
    bool bRelayOnly = false;
    bool bLoopbackProbe = false; // Explicit same-host tests only.
};
struct FDirectServerSnapshot
{
    int32 LivePeers = 0, Records = 0, Created = 0, Connected = 0, Closed = 0, Expired = 0;
    int32 Replays = 0, Rejected = 0, EarlyIce = 0;
    int32 ConnectedSessions = 0, PeakLivePeers = 0, PeakConnectedSessions = 0;
    int32 RoutedRecipients = 0, RejectedRecipients = 0;
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
    FSimurghRTCError GetSessionInfo(const FString& SessionId, FDirectSessionInfo& Out) const;
    TArray<FDirectSessionInfo> GetSessions(bool bIncludeClosed = false) const;
    FSimurghRTCError CloseSession(const FString& SessionId);
    // Bounded fan-out (1–32 distinct addresses). Invalid input/source rejects the
    // entire request. Otherwise inspect each result; accepted recipients are not
    // rolled back if another recipient is denied/full/closed. Retry only failures.
    FSimurghRTCError RouteMessage(const FDirectChannelAddress& Source, const FSimurghRTCDataMessage& Message,
        const TArray<FDirectChannelAddress>& Destinations, TArray<FDirectRouteResult>& OutResults);
    FSimurghRTCError EchoMessage(const FString& SessionId, const FSimurghRTCDataMessage& Message);
private:
    class FImpl;
    explicit FDirectServer(TSharedRef<FImpl> InImpl);
    TSharedRef<FImpl> Impl;
};
}
