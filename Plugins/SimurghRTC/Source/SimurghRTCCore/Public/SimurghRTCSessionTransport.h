// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCTypes.h"
class UObject;

namespace SimurghRTC
{
// Internal provider boundary. Not part of the reflected v1 application contract.
enum class ESessionChannelState : uint8 { Connecting, Open, Closing, Closed };
struct FSessionChannelSnapshot
{
    FSimurghRTCChannelInfo Info;
    ESessionChannelState State = ESessionChannelState::Connecting;
};
struct FSessionTransportSnapshot
{
    ESimurghRTCSessionState State = ESimurghRTCSessionState::Connecting;
    FSimurghRTCError Error;
    bool bNativeClosed = false, bRemoteClosed = false;
};
struct FSessionTransportEvent
{
    bool bMessage = false;
    FSimurghRTCChannelInfo Channel;
    FSimurghRTCDataMessage Message;
};
using FSessionTransportEventHandler = TFunction<void(const FSessionTransportEvent&)>;
class ISessionTransport
{
public:
    virtual ~ISessionTransport() = default;
    virtual FSessionTransportSnapshot Snapshot() const = 0;
    virtual void Close() = 0;
    // Optional audio slot; defaults keep data-only/test providers compatible.
    virtual bool HasAudio() const { return false; }
    virtual bool HasRemoteAudio() const { return false; }
    virtual FSimurghRTCError PushAudio(const FSimurghRTCAudioFrame&) { return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("Audio was not negotiated.")); }
    virtual FSimurghRTCError MuteAudio(bool) { return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("Audio was not negotiated.")); }
    virtual bool PollAudio(FSimurghRTCAudioFrame&) { return false; }
    virtual void SetHandler(FSessionTransportEventHandler Handler) = 0;
    virtual FSimurghRTCError OpenChannel(const FSimurghRTCChannelOptions&, FSimurghRTCChannelHandle&) = 0;
    virtual FSimurghRTCError CloseChannel(FSimurghRTCChannelHandle) = 0;
    virtual FSimurghRTCError Channel(FSimurghRTCChannelHandle, FSessionChannelSnapshot&) const = 0;
    virtual FSimurghRTCError SendText(FSimurghRTCChannelHandle, const FString&) = 0;
    virtual FSimurghRTCError SendBytes(FSimurghRTCChannelHandle, const TArray<uint8>&) = 0;
};
using FSessionTransportFactory = TFunction<TSharedPtr<ISessionTransport>(UObject*, const FSimurghRTCSessionConfig&, FSimurghRTCError&)>;
// Game-thread only; replacement is for module setup and isolated contract tests.
SIMURGHRTCCORE_API FSessionTransportFactory SetSessionTransportFactory(ESimurghRTCProvider, FSessionTransportFactory);
SIMURGHRTCCORE_API TSharedPtr<ISessionTransport> CreateSessionTransport(UObject*, const FSimurghRTCSessionConfig&, FSimurghRTCError&);
SIMURGHRTCCORE_API bool HasSessionTransport(ESimurghRTCProvider);
SIMURGHRTCCORE_API void RetireSessionTransport(TSharedPtr<ISessionTransport>);
SIMURGHRTCCORE_API void ShutdownSessionTransports();
SIMURGHRTCCORE_API void ShutdownSharedSessions();
struct FSessionRuntimeSnapshot { int32 Created = 0, NativeClosed = 0, RemoteClosed = 0, PendingCleanup = 0; };
SIMURGHRTCCORE_API FSessionRuntimeSnapshot GetSessionRuntimeSnapshot();
}
