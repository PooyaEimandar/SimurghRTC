# Direct HTTP signaling

SR-102 connects native peers in separate Unreal processes. The client uses Unreal's HTTP client; the Server uses HTTPServer. They exchange SDP and ICE through [protocol v1](SIGNALING_V1.md), then establish the private WebRTC DTLS/SCTP transport.

This is an internal provider API. The shared `USimurghRTCSession` facade still returns `NotImplemented` for connection attempts until SR-105 integrates it. Application DataChannels are SR-103. These probes do not send application messages or network audio/video.

On 2026-10-24, the packaged Mac ARM64 Full client and dedicated Server passed the separate-process probe: six connections across normal, lost-response and delayed-response cases; all 11 created sessions closed, with no peers left alive. Both native factory modes and early/trickled ICE passed. The fault cases below also passed. See the [validation record](reviews/SR102-direct-http-validation.json) for exact builds and results.

## Server ownership

Include `SimurghRTCDirectServer.h` from the Server module. Call `FDirectServer::Bind` on the game thread with a live UObject owner, your HTTP router and an admission callback. Keep the returned handle.

The admission callback receives the bearer credential on every request. Return the same principal for the same authenticated user, and explicitly allow or reject new sessions. The create request's identity/purpose fields are metadata; they do not authenticate anyone. Session IDs alone grant no access.

The host owns listener startup, bind address and shutdown. The provider installs one route and removes only that route when stopped; it does not stop another service's HTTP listeners. Call `Stop` during owner shutdown. Native cleanup is asynchronous; `GetSnapshot().LivePeers == 0` confirms it has finished.

## Client ownership

Include `SimurghRTCDirectClient.h` from the Direct module. For example:

```cpp
SimurghRTC::FDirectClientConfig Config;
Config.Endpoint = TEXT("https://rtc.example.com/simurghrtc/v1/signal");
Config.BearerToken = Credential; // Obtain from your application's authentication flow.
Config.Identity = TEXT("native-client");
FString Error;
Client = SimurghRTC::FDirectClient::Connect(this, MoveTemp(Config),
    [](const SimurghRTC::FDirectClientSnapshot& State)
    {
        // Runs on the game thread. Inspect State.State and State.Error.
    }, Error);
```

Retain `Client` and call `Close` during `EndPlay` or shutdown. A successful open starts negotiation; `Connected` requires both peers to establish the transport. Callbacks are suppressed after owner loss. Close cancels pending work without blocking the game thread. `bNativeClosed` confirms local cleanup; `bRemoteClosed` confirms an acknowledged Server close.

Dropping the handle sends best-effort remote cleanup. If creation succeeded but its response was lost, the client may not know the session ID; the Server's idle lease removes that orphan. Defaults are a 20-second connection deadline, a 2-second HTTP timeout and at most two retries per request. A retry keeps the original request ID, sequence and body.

Reconnect with a new handle. Renegotiation, ICE restart, public channels and media tracks are separate work. Direct/Server modules disable dynamic reload while asynchronous HTTP work may still be completing; restart the host after changing them.

## Run the process probe

Build and package a Mac Server host and a Direct-enabled Mac Game host with the same current native artifact. Follow [Building](BUILDING.md), [Server packaging](NATIVE_PROBE.md#build-and-stage-a-dedicated-server), and the existing [Game packaging commands](LIFECYCLE_EXAMPLES.md#package-the-examples).

```sh
python3 tools/run_direct_probe.py \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --client-project "$CLIENT_PROJECT" --client-executable "$CLIENT_EXE" \
  --pairs 4 --output Intermediate/DirectHttpProbe
```

The executables must be inside their packaged `.app` bundles. Use a fresh output directory. The runner creates short-lived credentials in each app's container, removes them afterward, and stops every process it launches. It checks process IDs, artifact hashes, saved reports and Server cleanup rather than relying on exit codes alone.

The probe covers:

- Separate client and dedicated Server processes, both factory modes, early ICE and clean close.
- Version/schema errors, sequence gaps, request conflicts, replayed poll snapshots and native SDP rejection.
- Authentication, rejected admission, session isolation, cache capacity and idle expiry.
- Lost/delayed replies, incorrect response correlation, oversized replies, timeout, cancellation and owner loss.

These tests explicitly restrict ICE to loopback, using the same-host test option introduced in SR-101. Normal provider connections retain the default network interfaces. They do not establish cross-device, NAT/TURN, mobile or physical iOS acceptance.

## Deployment

HTTPServer serves HTTP. For remote use, put a trusted HTTPS reverse proxy in front of its loopback listener and keep the HTTP client’s certificate checks enabled. Configure request/header/connection limits and timeouts at that boundary: HTTPServer receives the body before the plugin can reject an oversized request.

Use the final endpoint URL, with no redirects. The client rejects a changed final URL, but Unreal's underlying HTTP implementation may already have followed a redirect. Do not use redirecting endpoints for credentials. Keep credentials and SDP out of access logs.

For local development, the client accepts `http://127.0.0.1:<port>/simurghrtc/v1/signal` only when `bAllowLoopbackHttp` is enabled. The sample listener binds only to loopback. This local implementation does not close M0's external dependency acceptance gates or qualify an untrusted public service.
