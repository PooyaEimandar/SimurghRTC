# Shared API contract — version 1

Version **1** freezes the shared C++ and Blueprint surface for **Unreal Engine 5.8.3**. Applications can query `USimurghRTCSession::GetApiContractVersion()`. The [lifecycle examples](LIFECYCLE_EXAMPLES.md) execute local success, failure, cancellation and owner cleanup through both interfaces.

Direct sessions now connect, open/close DataChannels, send text/binary messages and disconnect through this API. Start with [Shared sessions](SHARED_SESSIONS.md). Janus connections, calls, rooms, device capture and media delivery still return explicit implementation errors. Native media feasibility tests do not establish network media support.

The SR-001 review fixes ownership, call topology, headless media, device discovery, cancellation and errors below. Public v1 names, parameter types, Blueprint pins, enum values and these lifecycle rules are compatibility commitments. Additive APIs are allowed; breaking changes require a new contract version and migration notes. Append enum values rather than reordering them. The earlier M0 draft had no compatibility guarantee.

## Use the same API for both providers

Applications depend on `SimurghRTCCore` and include `SimurghRTCSession.h` and `SimurghRTCOperation.h`. Common types contain Unreal values, opaque string identities and plugin-owned GUID handles. Public headers expose no libwebrtc types, Janus numeric handles or provider JSON.

| Surface | C++ entry points | Blueprint equivalent |
|---|---|---|
| Session | `CreateSession(Owner)`, `GetSessionId`, `GetState`, `GetProvider`, `Shutdown` | Callable functions and pure queries |
| Connection | `Connect(Session, Config)`, `Disconnect(Session)` | Async Connect / Disconnect |
| Channels | `OpenChannel`, `CloseChannel` | Matching async nodes |
| Data | `Session->SendText`, `SendBytes`, `GetChannelInfo` | Callable nodes with a structured error |
| Calls | `Call`, `AcceptCall`, `RejectCall`, `HangUp`; `Session->GetCallInfo` | Async nodes and a pure call query |
| Text rooms | `JoinTextRoom`, `LeaveTextRoom`, `SendRoomText` | Matching async nodes |
| Generated media | `CreateGeneratedSource`, `AttachSource`, `ReplaceSource`, `SetSourceMuted`, `StopSource`, `DetachTrack` | Matching async nodes in Core |
| CPU frames | `Session->PushAudioFrame`, `PushVideoFrame`; `SetDecodedFrameDelivery` | Callable submissions and async delivery control |
| Diagnostics | `GetCapability(Provider, Feature)`, `GetStatistics`, `GetApiContractVersion` | Pure queries |

Unless prefixed with `Session->`, async operations in this table are static factories on `USimurghRTCOperation`. Output fields are in `FSimurghRTCOperationResult`: `Error`, `Channel`, `Source`, `Track`, `Call` and `RoomId`. Only fields belonging to the successful operation are populated; failure never manufactures a handle.

Session events are `OnStateChanged`, `OnMessage`, `OnChannelChanged`, `OnTrackChanged`, `OnParticipantChanged`, `OnRoomText`, `OnIncomingCall`, `OnCallChanged`, `OnAudioFrame` and `OnVideoFrame`. Both C++ and Blueprints can bind the reflected events. Operation completion additionally offers `OnCompletedNative` for C++.

### Configuration and topology

`FSimurghRTCSessionConfig` selects provider, purpose, endpoint, local identity, access token, ICE servers and timeout. Direct permits HTTP(S); Janus also permits WS(S). Insecure signaling needs explicit development opt-in. Endpoints require a host and cannot embed user credentials, fragments or whitespace. Credentials are transient and omitted from errors/logs. Operation completion releases its configuration copy. Application code and Blueprint literals must not embed production secrets.

The current Direct adapter requires the full `/simurghrtc/v1/signal` URL, a local identity and an access token. HTTP is limited to explicitly enabled loopback development endpoints. Custom ICE servers and receive-only sessions return `NotImplemented`; they are not silently ignored. Purpose selects the server admission category, but does not implement calls or rooms.

| Purpose | Direct Unreal | Janus |
|---|---|---|
| Echo | The plugin's Unreal endpoint is the remote peer | Janus EchoTest is the remote peer |
| Call | One client calls the server participant identified by the application | VideoCall joins two registered participants through Janus |
| TextRoom | The Unreal application's room service routes messages | Janus TextRoom routes messages |
| Receive | Receive tracks offered by the Unreal endpoint | Receive from a configured Janus service, subject to its capabilities |

Both providers use the same native libwebrtc adapter. Before Connect, `GetProvider` defaults to Direct if included, otherwise Janus. Provider selection happens at Connect; disconnect before changing it. No live PeerConnection migration is promised. Direct does not turn an ordinary Unreal dedicated server into a WebRTC endpoint, or add client-to-client calling implicitly. Janus VideoRoom/SFU operations are separate extensions, not the portable one-to-one `Call` operation. [Signaling v1](SIGNALING_V1.md) describes private provider messages.

A session has at most one call. `Call` success means an active call and returns its opaque `CallId` and `PeerId`; progress arrives through `OnCallChanged` (`Dialing`, `Ringing`, `Active`, `Ended`). Incoming calls supply the identity needed by `AcceptCall`/`RejectCall`. `HangUp` ends the current call. `GetCallInfo` returns `None` with empty identities when no call exists. Call operations are not implemented yet.

Rooms use opaque string IDs. Successful join/leave/send results identify `RoomId`; participant and text events carry explicit identities. A room is an application service, not an automatic multicast DataChannel. Common operations do not expose administrative Janus credentials.

## Lifecycle and ownership

All public calls and callbacks run on the **game thread**. Retain a session in a `UPROPERTY` or Blueprint variable. Its Outer alone is not an owning reference. One owner can keep several independent sessions.

C++ factories return dormant operations: retain the proxy, bind completion, then call `Activate()`. Blueprint async nodes bind their output pins and activate automatically. Activated operations survive until completion, even when a null session must produce a deferred argument failure. A dormant proxy remains the caller's responsibility.

Each operation has a GUID and one final result. `GetResult()` returns `InvalidState` until `IsFinished()` is true. Every operation result carries its operation ID and, when supplied, its session ID in `Error`, including on success. Synchronous session calls carry only a session ID; static validation and device queries have neither. Activation is idempotent. Ordinary completion is deferred to a later core-ticker tick; explicit `Cancel()` can complete immediately, including before activation. Bind listeners first.

For a live owner, exactly one of **On Success**, **On Failure** or **On Cancelled** fires. `OnCompletedNative` receives the same result before the Blueprint event. Repeated cancellation or activation after completion does nothing. If a native listener shuts down the session, no subsequent Blueprint completion is started. A multicast dispatch already in progress may finish its current listener list.

| Action | Result |
|---|---|
| `Disconnect` | Cancel other active operations, wait for transport closure, retain a reusable session. Repeated local disconnect succeeds. |
| `Cancel` | Finish that operation with `Cancelled`. A started Connect closes its attempt; a started Disconnect continues closing. |
| `Shutdown` | Final local teardown: silently cancel active operations and permanently close the session. Repeat calls do nothing. |
| Owner EndPlay / widget teardown | Call `Shutdown`; do not wait for a completion event. |
| World cleanup / session destruction | Perform silent shutdown as a fallback. |
| Activate a dormant proxy after shutdown | Settle it as cancelled without calling the former owner. |

Direct Connect goes `Idle/Closed/Failed → Connecting → Connected`, or `Failed` on a connection error. Validation failures leave session state unchanged. Cancelling during `Connecting` returns to `Idle`; cancellation before execution leaves the prior state unchanged. Disconnect moves through `Closing` to `Closed`. Cancelling its proxy cannot undo a close already sent. Reconnect keeps the facade's session GUID but creates a fresh PeerConnection, server session and channel handles.

Transport callbacks use weak UObject references and return to the game thread. Shutdown removes listeners immediately; native cleanup continues without calling the former owner. Connect enforces the configured deadline (1–300,000 ms, starting at activation). Other remote operations use the connected session's timeout. A timeout settles the operation while remaining cleanup continues in the background.

## Channels, sources and sinks

Handles belong to one session. Applications must not fabricate them or reuse them after close/stop. Channel operations reject unknown, foreign and stale handles. `IsValid()` means only that a GUID is present, not that its resource exists. `OpenChannel` returns a handle only after the native channel opens and its message limit is known; cancelling a pending open closes its partial channel.

Channel options separate ordering and reliability. `Reliable` requires a zero bound; `MaxRetransmits` takes a count and `MaxLifetime` takes milliseconds, both 0–65,535. Labels are limited to 65,535 UTF-8 bytes and cannot contain a null character. `MaximumMessageBytes == 0` means unknown, not unlimited. Send success means a copied payload was admitted to a bounded local queue. It does not acknowledge remote delivery. Use `GetChannelInfo` and `OnChannelChanged` to observe buffer drain after `ResourceExhausted`; [DataChannels](DATA_CHANNELS.md) lists the limits.

Track information distinguishes local and remote tracks with `bRemote`; `Source` identifies a local track's source and is empty for remote tracks. `OnTrackChanged` with `bAdded=true` adds or updates information, and `false` removes it.

Source ownership is independent of track ownership. Creating a source can precede Connect; attaching it requires a connected session and returns a track. Replacing a source preserves that track's identity and media kind. Muting retains the source; stopping invalidates it and ends its attached tracks. Detaching a track leaves the source available for another attachment. Disconnect closes channels/tracks and stops session-owned sources. Shutdown also releases sink references. These operations currently return explicit errors rather than fabricating resources.

### Headless media in Core

Generated sources and decoded frames require no RHI texture, audio component or client capture module. They are available as a contract on Server targets too. Data-only client profiles exclude media capability.

| Frame | Layout and validation |
|---|---|
| `FSimurghRTCAudioFrame` | Interleaved float PCM in [-1, 1]; finite samples; 1 or 2 channels; 8,000 / 16,000 / 32,000 / 44,100 / 48,000 Hz; complete sample frames, at most 100 ms per submission |
| `FSimurghRTCVideoFrame` | Tightly packed, top-down BGRA8; positive dimensions up to 3840 × 2160; exactly `Width × Height × 4` bytes |
| Both | Nonnegative microsecond timestamp, relative to a source's monotonic clock; no platform buffer pointers |

`CreateGeneratedSource(Session, Kind)` selects audio or video. Submit through `PushAudioFrame`/`PushVideoFrame`; the implementation must copy accepted input before returning. Source kind and monotonically increasing timestamps must be checked by the future source registry. The structural validators already reject invalid lengths, dimensions and samples.

Decoded frame delivery is off by default. Enable it per track with `SetDecodedFrameDelivery`; results arrive through `OnAudioFrame`/`OnVideoFrame` on the game thread. Copy event data if keeping it after the callback. Transport implementations must bound queued media, reject full submission queues with `ResourceExhausted`, and suppress delivery after disable/detach/shutdown. These CPU copies are a portable baseline; they do not promise zero-copy performance.

### Optional client devices and Unreal sinks

`SimurghRTCMedia` adds `USimurghRTCMediaOperation::GetDevices`, `CreateSource`, `BindVideo` and `BindAudio`. `GetDevices(Kind, OutDevices)` queries a cached microphone/camera list without requesting permission. IDs are opaque and may change between runs; labels may be empty until access is granted. The result includes Unknown/Available/Unavailable permission status. M0 returns an empty list with `NotImplemented`, rather than pretending no devices exist.

`CreateSource` supports microphone, camera, render target, screen and generated media. Device access requires an explicit application request and platform permission; denial returns `PermissionDenied`, never a bypass. `BindVideo` takes a render target and `BindAudio` an audio component. These sinks are client-only; headless applications use Core frames. A sink remains referenced until replacement, track teardown or session teardown. Binding a new sink of the same kind replaces the old binding. Platform/device behavior remains work for the media milestones.

The draft's common source/track operations moved to Core for headless use. Use `USimurghRTCOperation` for those controls and proxy fields. Replace pre-v1 Blueprint source/track control nodes with the Core versions and recompile. `USimurghRTCMediaOperation` remains the entry point for device creation and Unreal sinks.

## Capabilities, errors and extensions

`FSimurghRTCCapability` separates `Compiled`, `Platform`, `Service` and `Negotiated`, each Unknown/Available/Unavailable. `IsAvailable()` requires all four to be Available. Direct DataChannels report compiled/service availability when the adapter is included; platform and negotiation become Available after that session connects. Other features remain explicitly unavailable or unimplemented. `DeviceCapture` denotes the optional client capture module; generated/headless media is separate. Statistics have `bAvailable == false`; zeros are not measurements.

Errors distinguish invalid input/state, build exclusion, unavailable platforms, unsupported services, missing negotiation, authorization, timeout, cancellation, pending implementation, exhausted resources, denied device permission and internal failure. `None` means success. Invalid payloads are rejected before resource use. Errors do not include raw provider messages or credentials.

`SimurghRTCJanusExtensions` contains `JoinVideoRoom` and its string-based room options. It is a reflected placeholder, not an implemented VideoRoom service. Janus-only classes are physically omitted from disabled profiles. The Core provider enum remains present so shared configuration can report `BuildExcluded`.

Do not feature-guard reflected UCLASS/USTRUCT/UFUNCTION declarations. Stage the selected modules. The shared lifecycle Blueprint references Core only and uses the same graph with Janus enabled or disabled.
