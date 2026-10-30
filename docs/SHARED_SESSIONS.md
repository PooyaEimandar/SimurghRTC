# Shared Direct sessions

Use `USimurghRTCSession` and `USimurghRTCOperation` to connect an Unreal client to the plugin's Unreal Server endpoint. SR-105 connects the frozen C++/Blueprint API to Direct HTTP signaling and native DataChannels. Applications need only the `SimurghRTCCore` module for these client operations.

Connect, Disconnect, OpenChannel, CloseChannel, SendText and SendBytes are implemented. Janus connections, calls, rooms, media and statistics remain separate work. The Blueprint nodes use the same implementation; [Data examples](DATA_EXAMPLES.md) provides matching Blueprint and C++ maps. The older [lifecycle graphs](LIFECYCLE_EXAMPLES.md) continue to test local success, failure and cleanup.

## Connect, then open a channel

Retain the session and dormant operation in transient `UPROPERTY` fields. Call the API on the game thread. This example assumes `Session` and `Pending` are fields on a UObject owned by your application:

```cpp
#include "SimurghRTCSession.h"
#include "SimurghRTCOperation.h"

Session = USimurghRTCSession::CreateSession(this);
FSimurghRTCSessionConfig Config;
Config.Provider = ESimurghRTCProvider::DirectUnreal;
Config.Purpose = ESimurghRTCPurpose::Echo;
Config.Endpoint = Endpoint; // Full URL ending in /simurghrtc/v1/signal.
Config.LocalIdentity = LocalIdentity;
Config.AccessToken = AccessToken; // Supplied at runtime, never a source literal.
Config.TimeoutMilliseconds = 20000;

Pending = USimurghRTCOperation::Connect(Session, Config);
Pending->OnCompletedNative.AddWeakLambda(this,
    [this](const FSimurghRTCOperationResult& Result)
    {
        if (!Result.IsSuccess()) return; // Present Result.Error to the caller.
        // Session is Connected. Start OpenChannel here.
    });
Pending->Activate();
```

The Server must run `FDirectServer` with an application authorization callback; a regular Unreal Server does not expose this endpoint automatically. See [Direct signaling](DIRECT_SIGNALING.md) and [Server routing](SERVER_ROUTING.md). The server's authorization result, rather than the client's claimed identity, determines its principal.

For local development only, an HTTP loopback URL also needs `bAllowInsecureDevelopmentSignaling = true`. Normal endpoints use HTTPS. Custom `IceServers` and `Receive` purpose currently return `NotImplemented`. Selecting Call or TextRoom does not implement those services.

After Connect succeeds, create a second operation:

```cpp
FSimurghRTCChannelOptions Options;
Options.Label = TEXT("chat"); // Ordered and reliable by default.
Pending = USimurghRTCOperation::OpenChannel(Session, Options);
Pending->OnCompletedNative.AddWeakLambda(this,
    [this](const FSimurghRTCOperationResult& Result)
    {
        if (Result.IsSuccess()) Channel = Result.Channel;
    });
Pending->Activate();
```

Success means the channel is open and its negotiated message limit is known. The operation does not expose a usable handle while it is still opening. In Blueprint, retain the session variable, connect the async success/failure/cancelled pins, and take the channel from the successful result. Blueprint async nodes activate automatically.

## Send, receive and handle a full queue

Bind `Session->OnMessage` to a reflected handler taking `const FSimurghRTCDataMessage&`. Its `Kind`, `Text` or `Bytes`, and local `Channel` identify the message. Events and operation completions arrive on the game thread. Copy event data if keeping it after the callback.

```cpp
FSimurghRTCError Error = Session->SendText(Channel, TEXT("hello"));
// Or: Session->SendBytes(Channel, Payload);
```

Success means the plugin copied the payload into a bounded send queue; it does not confirm remote receipt. `ResourceExhausted` means the message was rejected. Keep it in your own bounded queue and retry after `OnChannelChanged` or `GetChannelInfo` reports available capacity. Do not retry in a tight loop. The maximum message is 64 KiB or the smaller negotiated limit. See [DataChannels](DATA_CHANNELS.md) for ordering, reliability and buffer limits.

Each session has its own PeerConnection and local channel handles. A handle from another session, a closed connection or an earlier reconnect is rejected. The server application decides whether to echo or route messages; the transport does neither automatically.

For connection loss, handle `OnStateChanged(Failed)` and read `GetConnectionError()` in C++ or Blueprint. After connectivity returns, call Connect on the same session and open a new channel. The session ID stays the same, while the PeerConnection and channel handles are replaced. The new connection clears the previous error; the plugin does not retry a failed connection automatically.

## Close and reuse

Use `CloseChannel` to close one channel and `Disconnect` to close the whole connection. Both are async operations: bind completion and activate as above. Disconnect cancels other active operations and waits for transport closure within the session's timeout.

Connect can run again after Closed or Failed. The same session UObject keeps its GUID, but the connection, server session ID and channel handles are fresh. Cancelling an active Connect closes that attempt. Cancelling a Disconnect proxy reports Cancelled while the close already underway continues.

Call `Session->Shutdown()` from EndPlay or widget teardown. Shutdown is final and silent: it cancels pending work and detaches callbacks immediately. Native cleanup continues without invoking the former owner. Owner loss, world cleanup and session garbage collection provide the same fallback. Keep the engine ticking while normal connection cleanup completes; do not treat a silent Shutdown as a remote acknowledgement.

Dropping a session variable does not immediately destroy its UObject. Call Shutdown when callbacks must stop immediately; garbage collection provides cleanup when the object is actually collected.

The mobile data samples call Shutdown when the app deactivates, before suspension, then create a new session when it becomes active again. This policy is implemented in the sample's C++ flow and Blueprint graph; applications can choose their own lifecycle policy. See [background and resume](DATA_EXAMPLES.md#background-and-resume).

## Run the packaged checks

Build Full Game and Server packages with the same current Mac ARM64 artifact as described in [Direct signaling](DIRECT_SIGNALING.md). Then run:

```sh
python3 tools/run_session_probe.py \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --client-project "$CLIENT_PROJECT" --client-executable "$CLIENT_EXE" \
  --output Intermediate/SharedSessionProbe
```

Use a fresh output directory. The runner starts four Game processes with two shared sessions each, holds all eight connections open, then checks exact text/binary delivery, rejected foreign/stale handles, queue recovery and reconnect. Separate runs exercise cancellation, operation and HTTP timeouts, disconnect during Connect, shutdown from a callback, owner loss, world cleanup and session garbage collection. Reports must show native closure, no pending cleanup and no late callbacks. Temporary credentials are removed and only the runner's own processes are stopped.

The sample client is [ContractSessionEndpoint.cpp](../Samples/ContractHost/Source/ContractHost/ContractSessionEndpoint.cpp). It uses Core APIs for connection and data operations. Its surrounding probe reads internal cleanup counters only to verify teardown.

Validated on 2026-10-27: four packaged runs passed with eight simultaneous sessions per run, 116 exact round trips and 80 native client closures. The suite also passed 27 Unreal and 176 Python tests, plus the existing routing, DataChannel and C++/Blueprint lifecycle regressions. The [validation record](reviews/SR105-shared-session-validation.json) retains build identities, results and limitations. Include `Content/Examples` when cooking the older Blueprint lifecycle samples, as shown in their [packaging instructions](LIFECYCLE_EXAMPLES.md#package-the-examples).

These checks use explicit same-host loopback ICE, Mac ARM64 Development Full Game and dedicated Server packages. They do not establish cross-device, NAT/TURN, remote HTTPS, Android/iOS or network media acceptance. See the [roadmap](../ROADMAP.md) for those gates.
