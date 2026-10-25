# Native peers and signaling

SR-101 adds an internal PeerConnection handle with asynchronous SDP/ICE exchange. It can connect two local peers using the isolated M155 backend. The shared C++/Blueprint session API still returns `NotImplemented` for connection attempts; the [Direct HTTP provider](DIRECT_SIGNALING.md) negotiates peers internally, and [DataChannels](DATA_CHANNELS.md) are available internally through SR-103.

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

## Diagnose a connection

Each transport snapshot includes numeric ICE and DTLS states, the selected UDP/TCP protocol, candidate counts, and recent STUN request/response counts. Statistics refresh at most four times per second. `StatsAgeMs` tells you how old the last sample is. These fields contain no SDP, network addresses, or credentials.

- ICE: 0 new, 1 checking, 2 connected, 3 completed, 4 failed, 5 disconnected, 6 closed.
- DTLS: 0 unknown, 1 new, 2 connecting, 3 connected, 4 closed, 5 failed.
- Selected protocol: 0 none, 1 UDP, 2 TCP, 3 other.

A failed ICE check with requests but no responses points to connectivity or STUN handling before DTLS. It does not by itself identify the cause. Keep the failed result; reconnecting successfully is not proof that the original problem is fixed.

The 2026-10-24 investigation reproduced failures during ICE, before DTLS. A separate UDP test without WebRTC also failed to receive packets sent to this Mac's own Wi-Fi address in six attempts; loopback succeeded in all three control attempts. A diagnostic run observed STUN response delays above 13 seconds. This points to a host networking problem, but its exact OS or network-policy cause is still unknown.

Local fixtures now explicitly use loopback. Normal plugin connections keep their default network interfaces, and no host network settings were changed. This makes the local ownership/signaling tests reproducible; remote connectivity still needs separate acceptance tests.

## Validate

The standalone fixture connects four local pairs, two in each factory mode. Both peers must reach ICE Connected and open the private DTLS/SCTP channel. It deliberately sends candidates and completion before SDP, checks duplicates, rejects malformed/stale candidates, tests overflow, and closes peers with work pending. It uses explicit loopback candidates and real local sockets, with no STUN/TURN service. Add `--host-interface` to the runner to check normal network interfaces instead.

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

### Verified on 2026-10-24

The final bridge passed these Unreal 5.8.3 packaged probes. Every pair completed SDP/ICE, opened DTLS/SCTP, and closed cleanly.

| Target | Profile | Connected pairs |
|---|---|---|
| Mac dedicated Server | Server | 8/8 |
| Galaxy S25 Ultra | Direct-only | 8/8 |
| Galaxy S25 Ultra | Full | 8/8 |
| iOS 27.0 ARM64 simulator | Direct-only | 8/8 |
| iOS 27.0 ARM64 simulator | Full | 8/8 |

All **17 Full-profile Mac Editor tests passed**, with no test warnings, and all **157 Python tests passed**. The final native fixtures passed on Mac, Android and the simulator, plus 32 additional Mac pairs. All four ARM64 libraries build; the iOS device consumer links but has not run on a physical iPhone.

The [packaged validation record](reviews/SR101-packaged-signaling-validation.json) records artifacts, package audits, source hashes and the investigation above. It also retains a Server runner failure caused by saving outside the app sandbox; the same executable passed after correcting the report location. These are incremental builds on the available host, not a new repeat-build qualification.

The [earlier signaling record](reviews/SR101-signaling-validation.json) includes Mac Editor and packaged-app checks of the preceding bridge; the [ownership record](reviews/SR101-ownership-validation.json) covers its earlier lifecycle work. Local pairs do not establish remote connectivity, network media, TURN or physical iOS acceptance. The Mac interface-to-self issue remains an environment follow-up.

## Packaged connection probe

Build the sample with a current native artifact. The opt-in `-SimurghRTCSignalingProbe` works in Development Android, iOS simulator, and dedicated Server packages. It uses loopback candidates, alternates data-only and media-capable factories, and cycles through early ICE on both peers, early offer ICE, and trickled delivery. Each candidate is delivered twice. Success requires both peers to connect, open the private SCTP channel, drain their ICE queues, finish every operation, and close.

Use the mobile runner after following [Mobile probe](MOBILE_PROBE.md):

```sh
python3 tools/run_mobile_probe.py --probe signaling --pairs 8 \
  --platform Android --project "$PROJECT" --app "$APK" \
  --device "$SERIAL" --output Intermediate/AndroidSignaling
```

For the simulator, use `--platform IOSSimulator`, its UDID, and the packaged `.app`. For a dedicated Mac Server:

```sh
python3 tools/run_signaling_probe.py --project "$PROJECT" \
  --executable "$SERVER" --pairs 8 --output Intermediate/ServerSignaling
```

Use fresh output directories. Reports are correlated with a new run UUID, the launched process, and the staged artifact. The runner retains failures and stops its app. There is no automatic retry. The internal `bLoopbackProbe` option is opt-in; normal peer creation retains the default network interfaces. The native probe rejects non-loopback candidates. These are local pairs within each process; they do not test cross-device signaling, TURN, application messages, or network media.

The Server runner expects the executable inside the packaged `.app`. It stores the app's report in its own macOS container, where sandboxed builds can write, and includes the validated report in the output directory's `result.json`.
