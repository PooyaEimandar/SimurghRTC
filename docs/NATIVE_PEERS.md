# Native peers and signaling

SR-101 adds an internal PeerConnection handle with asynchronous SDP/ICE exchange. It can connect two local peers using the isolated M155 backend. The shared C++/Blueprint session API still returns `NotImplemented` for connection attempts; Direct HTTP signaling and application DataChannels come next in SR-102/SR-103.

`FNativePeer` is for plugin providers and tests. It does not change API v1 or add Blueprint nodes. The M0 factory and media probes keep their synchronous paths.

## Open and close

Call `FNativePeer::Open` on Unreal's game thread with a live UObject owner, a factory mode and an optional completion callback. Keep the returned handle. Invalid input, an unavailable backend or exhausted capacity returns a null handle and an error immediately.

An accepted open creates the factory and peer on one native worker. Its callback runs on a later game tick with `Ready` or `Failed`. **Ready means the local peer exists.** Check `GetSnapshot().Transport.Connection` for the network connection state.

Call `Close` from `EndPlay` or your owner's shutdown method, then release the handle. Close returns without waiting for WebRTC. It cancels undelivered operation callbacks and transport events; `Closed` confirms native teardown. Repeated close is safe. Dropping the last handle, losing the UObject owner or cleaning up its world also closes the peer.

Use the handle, including its destructor, on the game thread. The service holds weak references to the owner and handle. Avoid strong captures that keep either alive. Callbacks and their captures are released on the game thread.

## Exchange SDP and ICE

After opening both peers:

1. Register each peer's event handler with `SetEventHandler`.
2. Submit `CreateOffer` to the first peer. Its result contains SDP after the local description has been set.
3. Submit that SDP to the other peer as `SetRemote` with type `Offer`.
4. Submit `CreateAnswer` there, then pass the answer back as `SetRemote` with type `Answer`.
5. Throughout negotiation, forward `LocalIce` events as `AddIce` requests and `LocalIceComplete` as `EndIce`.

For example, this submits an offer request to an already opened peer:

```cpp
SimurghRTC::FNativeSignalRequest Request;
Request.Action = SimurghRTC::ENativeSignalAction::CreateOffer;
FString Error;
const bool bAccepted = Peer->Signal(MoveTemp(Request),
    [](const SimurghRTC::FNativeSignalResult& Result)
    {
        // Runs on the game thread. Check IsSuccess(), then send Result.Sdp
        // to the other peer through your signaling provider.
    }, Error);
```

`Signal` returning false means no callback was scheduled. An accepted operation completes later, unless close or owner loss cancels delivery. Check `Result.Error` and `Message`; malformed SDP and wrong negotiation order fail explicitly. Operation IDs correlate results internally.

The offer creates a private, reliable channel named `simurghrtc-negotiation`. This supplies a data media section and lets the tests verify DTLS/SCTP startup. It has no application send/receive API. The builtin-media factory can negotiate this transport too; these tests do not send audio or video.

Only the initial offer/answer exchange is supported. Close and open a fresh peer to reconnect. Renegotiation, ICE restart, glare handling and a network session timeout belong to later provider work.

## Early candidates and limits

ICE may arrive before the remote description. The backend copies and queues it, returning a successful result with `bQueued=true`. Once remote SDP is accepted, it checks each candidate's MID, media index and optional username fragment before applying it. `RemoteIceQueued` and `RemoteIceApplied` show progress.

Repeated candidates are idempotent. An end marker can arrive early too. New candidates after the marker are rejected. A queued candidate that later fails validation causes `FatalError`; the Unreal worker releases that native runtime and fails outstanding operations. The caller must close the failed handle before opening its replacement.

| Limit | Value |
|---|---|
| SDP | 128 KiB of UTF-8, CRLF terminated |
| Candidate / MID / username fragment | 2 KiB / 32 bytes / 256 bytes |
| Remote trickle ICE | 32 unique entries, including the optional end marker |
| Pending signaling operations | 64 per peer; one SDP operation at a time |
| Events | 64 at each native/game-thread boundary; overflow fails the peer |
| Operation deadline | Default 30 seconds; allowed range 1–300,000 ms, including worker queue time |
| Live or closing peers | 64 per service |

An early-ICE acknowledgement confirms storage, not application. Its operation deadline ends at that acknowledgement. These limits govern the internal transport; they do not implement the versioned HTTP protocol described in [Signaling v1](SIGNALING_V1.md).

## Threads and shutdown

The first open starts one worker. All C-interface calls and destruction stay on that worker, while WebRTC owns its internal signaling/network/media threads. Requests and events cross the boundary as copied values; no native pointer or WebRTC object reaches game callbacks.

The worker checks active signaling every 10 ms and bounds each batch of commands/events. Callback delivery rechecks owner and handle lifetime before every call, including when an earlier callback closes a peer or shuts down the service.

Only module shutdown waits for the worker. It stops delivery, closes all peers, drains commands and joins before unloading. Normal close and world cleanup do not wait. Release integration handles before unloading their module.

`FNativeRuntime` remains available for synchronous test harnesses. Call and destroy it on its creating thread. Direct C consumers must poll events during signaling and destroy a runtime after a fatal error.

## Validate

The standalone fixture connects four local pairs, two in each factory mode. Both peers must reach ICE Connected and open the private DTLS/SCTP channel. It deliberately sends candidates and completion before SDP, checks duplicates, rejects malformed/stale candidates, tests overflow, and closes peers with work pending. It uses local host candidates with no STUN/TURN service.

```sh
python3 tools/run_native_signaling_probe.py \
  --artifact LocalBuilds/Artifacts/M155-Mac \
  --output Intermediate/NativeSignaling
```

Use a freshly built artifact and a new evidence directory. This runner needs permission to open local sockets.

Follow [Building](BUILDING.md) to stage an Unreal project and run `SimurghRTC.` automation. The new `Native.LocalConnections`, `Native.SignalingFailure` and `Native.SignalingCancellation` tests exercise real peers, early ICE, failure cleanup, bounded requests, deadlines and owner loss. Existing ownership, lifecycle and codec tests run alongside them.

For a packaged Mac check, follow the [packaging steps](LIFECYCLE_EXAMPLES.md#package-the-examples), then run:

```sh
REPORT="$HOME/Library/Containers/com.YourCompany.ContractHost/Data/Library/Application Support/Epic/ContractHost/Saved/Automation/Reports/SR101"
"$APP" -unattended -NullRHI -nosound \
  -ExecCmds="Automation RunTests SimurghRTC." \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$REPORT"
```

Set `APP` to the packaged `ContractHost.app/Contents/MacOS/ContractHost`. The default Mac sample is sandboxed, so reports must be inside its Saved directory. If you change its bundle ID, use the Saved path in the runtime log. Verify the JSON test count and results; exit status alone is insufficient.

On 2026-10-24, all **17 tests passed** in the Full-profile Unreal 5.8.3 Mac Editor and packaged Development app, with no test warnings. Each connection test established four local pairs across the two factory modes. The standalone fixture passed too; an unchanged native executable also passed 48 consecutive pairs. All four updated ARM64 libraries build, and the iOS device consumer links.

An initial Editor negotiation and one standalone run failed intermittently. Subsequent runs passed, including diagnostic repeats, but the cause was not established. This remains a reliability follow-up; these results do not claim that startup is failure-free.

The [signaling validation record](reviews/SR101-signaling-validation.json) retains passing and failed attempts, artifact identities and source hashes. It distinguishes the final library from a temporary diagnostic build. The [earlier ownership record](reviews/SR101-ownership-validation.json) covers the previous bridge. Mobile packages, network media, remote endpoints and physical iOS acceptance need their own checks; a local pair in one Unreal process does not establish those results.
