# Concurrent Server sessions

SR-104 adds session inspection, per-user limits and addressed routing to the internal `FDirectServer` provider. Each connected client owns a separate PeerConnection and channel handles. Clients now connect through the [shared session API](SHARED_SESSIONS.md); [Data examples](DATA_EXAMPLES.md) provides matching Blueprint and C++ clients.

On 2026-10-25, four packaged Unreal 5.8.3 Mac ARM64 clients passed concurrent routing against the dedicated Server. Both traffic phases passed without unexpected messages; all five sessions, including the replacement, released their native peers. All 20 Unreal and 171 Python tests pass, along with the existing 292-message DataChannel regression. See the [validation record](reviews/SR104-concurrent-server-validation.json).

## Own and inspect sessions

Bind the Server as described in [Direct signaling](DIRECT_SIGNALING.md). All calls and callbacks run on Unreal's game thread.

- `OnSessionChanged` reports connecting, connected and terminal states, with the session ID, authenticated principal and client metadata.
- `GetSessions()` lists sessions whose native peer has not closed. Pass `true` to include retained closed records. `GetSessionInfo` reads one record.
- `CloseSession(Id)` closes only that session. Repeating it is harmless while the record is retained. Unknown or expired IDs return `InvalidState`.
- `bNativeClosed` confirms the native peer has been released. `Closing` alone does not confirm cleanup. Failed sessions also report native closure.

Closing cancels that session's channel callbacks and pending sends; lifecycle notifications continue until native closure. Bytes already sent can arrive. A new connection gets a fresh session ID and channel handles. A departed client is removed when its signaling idle lease expires, even if it never sends a close request.

The defaults allow **16 live sessions total and four per authenticated principal**, configurable from 1 to 32. Closing peers still consume capacity until native teardown finishes. Closed records remain briefly for HTTP retry handling; the existing 64-record bound also applies. A full Server returns the Direct v1 `ResourceLimit` error (HTTP 503).

The host retains the Server handle and calls `Stop` during shutdown. Stop suppresses application callbacks, removes the signaling route and closes all peers. Wait for `LivePeers == 0` when validating cleanup; do not depend on callbacks after Stop or owner loss.

## Route messages

An address is a **session ID plus a Server-local channel handle**. Use the source session ID and message handle from `OnChannelEvent(SessionId, Event)`. Keep destination addresses from each session's open-channel events. Remove them when their channel or session closes.

`EchoMessage(SessionId, Event.Message)` sends a private echo. `SendText` and `SendBytes` send host-originated messages to one address. They verify channel ownership but leave application authorization to the host.

For client-to-client traffic, use `RouteMessage` and install `CanRoute` before binding. Cross-session routes are denied without that policy. Use the authenticated `Principal` to look up your application's room membership; `Identity`, `Purpose` and message text are client-supplied metadata.

```cpp
Config.CanRoute = [RoomForPrincipal](const SimurghRTC::FDirectSessionInfo& From,
    const SimurghRTC::FDirectSessionInfo& To, ESimurghRTCMessageKind Kind)
{
    const FString Room = RoomForPrincipal(From.Principal);
    return !Room.IsEmpty() && Room == RoomForPrincipal(To.Principal);
};
```

Here `RoomForPrincipal` is your application's authenticated membership lookup. The executable [text-room fixture](../Samples/ContractHost/Source/ContractHost/ContractConcurrentScenario.cpp) shows admission, routing and cleanup together.

From an addressed message callback:

```cpp
const SimurghRTC::FDirectChannelAddress Source{SessionId, Event.Message.Channel};
TArray<SimurghRTC::FDirectRouteResult> Results;
const auto Error = Server->RouteMessage(Source, Event.Message, Destinations, Results);
// Check Error first, then each result's Error and Destination.
```

A request accepts **1–32 distinct addresses**. Invalid input or an invalid source rejects the entire call before sending. Otherwise every destination gets its own result: accepted, unauthorized, closed, or unable to accept more data. Accepted recipients are not rolled back when another recipient fails. Retry only rejected recipients, using your own bounded queue.

Success means the payload was copied for sending. It does not mean the recipient acknowledged delivery. Existing [message limits and backpressure](DATA_CHANNELS.md#payload-ownership-and-limits) apply separately to each peer. The Server rechecks session/channel state after authorization, so a policy callback may close a session without sending to its stale address.

## Run the packaged check

Build a Mac ARM64 dedicated Server and Direct-enabled Game app from the same source and native artifact, then run:

```sh
python3 tools/run_concurrent_probe.py \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --client-project "$CLIENT_PROJECT" --client-executable "$CLIENT_EXE" \
  --output Intermediate/ConcurrentProbe
```

The runner starts four separate client processes with distinct credentials and identical client metadata. It checks private text/binary echoes, room fan-out, addressed binary messages, denied cross-room messages and global/per-principal capacity. Ordered barriers detect extra or leaked messages. It then closes one session during authorization, kills another client, waits for idle cleanup, and reconnects with a fresh session. Remaining clients continue exchanging messages.

The runner checks saved reports, process IDs, package/artifact hashes and native cleanup. Temporary credentials are removed and all launched processes are stopped. These are controlled Mac loopback checks; mobile, cross-device and NAT/TURN acceptance remain separate work.
