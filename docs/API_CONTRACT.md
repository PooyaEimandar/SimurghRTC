# Shared API contract — M0 draft

This document describes the C++ and Blueprint API for **UE 5.8.3**. Types, async nodes, argument checks and cancellation handling are implemented. Connections, channels and shared media/room services are still pending; `Connect` returns `NotImplemented`.

Separate [native probes](NATIVE_PROBE.md) can create an idle peer and test HTTP/media integration. Those probes do not enable the shared session API. See [validation results](M0_VALIDATION.md) for what has been tested.

The API is still under review in [SR-001/SR-002](../ROADMAP.md#e01-api-and-build-contract). [Signaling examples](SIGNALING_V1.md) describe future provider messages; they stay private to the plugin and do not change the public API.

## Public surface

Applications depend on `SimurghRTCCore` and include `SimurghRTCSession.h` and `SimurghRTCOperation.h`. All common operations use Unreal-owned types, GUID handles and string identities. No libwebrtc headers, provider JSON or Janus numeric handles cross this boundary.

| Surface | C++ entry points | Blueprint equivalent |
|---|---|---|
| Session | `USimurghRTCSession::CreateSession(Owner)`, `GetSessionId`, `GetState`, `GetProvider`, `Shutdown` | Callable functions and pure queries |
| Connection | `USimurghRTCOperation::Connect(Session, Config)`, `Disconnect(Session)` | Async Connect / Disconnect nodes |
| Data channels | `OpenChannel(Session, Options)`, `CloseChannel(Session, Handle)` | Async Open Channel / Close Channel nodes |
| Data IO | `Session->SendText(Handle, Text)`, `SendBytes(Handle, Bytes)`, `GetChannelInfo(Handle, OutInfo)` | Callable nodes returning a structured error |
| Calls | `Call`, `AcceptCall`, `RejectCall`, `HangUp` | Matching async nodes |
| Text rooms | `JoinTextRoom`, `LeaveTextRoom`, `SendRoomText` | Matching async nodes |
| Diagnostics | `GetCapability(Provider, Feature)`, `GetStatistics()` | Pure queries exposing reflected structs |
| Events | `OnStateChanged`, `OnMessage`, `OnChannelChanged`, `OnTrackChanged`, `OnParticipantChanged`, `OnRoomText`, `OnIncomingCall` | Assignable multicast events |

`FSimurghRTCSessionConfig` holds provider (`DirectUnreal` or `Janus`), purpose, signaling endpoint, opaque local identity, access token, ICE servers and timeout. Direct uses HTTP(S); Janus also permits WS(S). Insecure signaling requires explicit development opt-in. Credentials are transient and are not copied into errors or logs; completion releases the operation's config copy. `Transient` does not secure a credential embedded by application code or a Blueprint literal.

Provider selection occurs at Connect. An active connection must be disconnected before changing provider; no migration of an existing PeerConnection is promised. Direct Call targets the Unreal server participant; Janus VideoCall targets another registered participant. Text rooms represent application services with explicit participant identities, not automatic multicast DataChannels.

Channel options separate ordering from reliability. `Reliable` requires a zero bound; `MaxRetransmits` uses a retransmission count and `MaxLifetime` uses milliseconds, both in 0–65535. Labels are bounded by UTF-8 byte length, not Unreal character count. Handles belong to one session and must never be fabricated or reused after close. M1 will implement their registry and negotiated limits. `MaximumMessageBytes == 0` means unknown, not unlimited. Future send success will mean accepted into a bounded local queue, not delivered to a peer; M0 rejects every send because no open channels exist.

## Async lifecycle and ownership

All public calls and callbacks run on the game thread. C++ factories return a dormant operation: bind completion, retain it, then call `Activate()`. Blueprint async nodes bind their output pins and activate automatically. Activated operations are held by the session until completion and registered with its game instance when one exists. Retain the session in a `UPROPERTY` or Blueprint variable; an Outer alone is not the application's owning reference.

Each operation has a GUID and one final `FSimurghRTCOperationResult`. Its error carries the operation and session IDs. `GetResult()` reports `InvalidState` until `IsFinished()` is true. Activation is idempotent and normal completion is deferred to a later core-ticker tick. Explicit `Cancel()` may complete immediately on the game thread, including before activation; bind listeners first.

For a live session, exactly one of **On Success**, **On Failure**, or **On Cancelled** fires. C++ may bind `OnCompletedNative` to receive the same result; this fires before the Blueprint multicast. Finishing or cancelling an already finished operation has no effect. Reentrant local shutdown suppresses further Blueprint callbacks into that owner.

`Disconnect()` closes local session state and cancels other pending operations. The facade can be connected again. `Shutdown()` is final local teardown: it marks pending operations cancelled, suppresses callbacks, and closes the facade permanently. Call it from an actor's EndPlay, a widget's teardown, or the application's equivalent lifecycle hook. World cleanup and UObject destruction also perform silent shutdown. After shutdown, create a new session. No future worker callback may retain an unsafe raw UObject pointer; transport callbacks must marshal through this ownership boundary.

Current Connect behavior is `Idle/Closed/Failed → Connecting → Failed`, returning `NotImplemented`. Local Disconnect succeeds without making a network claim. Other operations validate arguments, build availability and relevant state, then report an explicit failure. No success path creates fake channel/source/track handles. Source/track ownership, device discovery, headless PCM/frame sinks, timeouts and negotiation semantics still require the native feasibility probes before this draft is frozen.

### C++ flow

Store `Session` and any dormant `PendingOperation` as `UPROPERTY(Transient) TObjectPtr<...>` members on the application owner. Bind the native event with the same UObject owner so destroyed listeners are skipped:

```cpp
Session = USimurghRTCSession::CreateSession(this);
FSimurghRTCSessionConfig Config;
Config.Provider = ESimurghRTCProvider::DirectUnreal;
Config.Endpoint = TEXT("https://rtc.example/session");
PendingOperation = USimurghRTCOperation::Connect(Session, Config);
PendingOperation->OnCompletedNative.AddUObject(this, &UMyClient::HandleConnectResult);
PendingOperation->Activate();
// Handler signature: void HandleConnectResult(const FSimurghRTCOperationResult& Result);
// M0 reports Result.Error.Code == ESimurghRTCErrorCode::NotImplemented.
// EndPlay / owner teardown: Session->Shutdown();
```

### Blueprint flow sketches

1. Create Session with Self as owner; store the returned session.
2. Make Session Config, select provider/endpoint and call Connect.
3. On Success: request the desired channel/call/room operation. This branch will become reachable when transport is implemented.
4. On Failure: break Result → Error; display a user-facing error and retain IDs for diagnostics. M0 takes this branch with `NotImplemented` for a valid connection request.
5. Store the exposed Operation proxy to Cancel. On Cancelled handles cancellation separately from failure. Owner EndPlay calls Shutdown and expects no further completion events.

These are graph sketches; executable Blueprint sample assets and graph/cook parity tests remain outstanding. UHT/reflection tests alone do not prove Blueprint graph behavior.

## Capabilities and errors

`FSimurghRTCCapability` separates `Compiled`, `Platform`, `Service` and `Negotiated` support, each Unknown/Available/Unavailable. `IsAvailable()` requires all four to be Available. In M0, compilation availability reflects the staged modules, platform/negotiation remain Unknown, and the unimplemented service is Unavailable. A compiled class does not establish operational support.

Errors distinguish `InvalidArgument`, `InvalidState`, `BuildExcluded`, `PlatformUnavailable`, `ServiceUnsupported`, `NotNegotiated`, `Unauthorized`, `Timeout`, `Cancelled`, `NotImplemented` and `InternalError`. `None` represents success. The latter runtime-specific errors reserve semantics for later implementation; the current shell does not pretend to exercise them. Diagnostics expose no raw provider payloads. Statistics are explicitly unavailable rather than reporting zeros as measurements.

## Optional reflected APIs

`SimurghRTCMedia` declares `USimurghRTCMediaOperation` nodes for CreateSource, AttachSource, ReplaceSource, SetSourceMuted, StopSource, DetachTrack, BindVideo and BindAudio. Source kinds cover microphone, camera, render target, screen, generated audio and generated video. Unreal sink references stay in this client media module. SR-004 now proves local codec and UE integration paths through the separate [media probe](MEDIA_PROBE.md). These public signatures still await SR-001 review and production media integration; their shared-service implementations remain placeholders.

`SimurghRTCJanusExtensions` declares `USimurghRTCJanusOperation::JoinVideoRoom` and a string-based room options struct as the optional-reflection probe. It does not implement VideoRoom. Janus-only classes live entirely in this optional module; portable application graphs use only Core types. The provider enum remains in Core so a shared configuration can report `BuildExcluded` when Janus is absent.

Do not wrap reflected UCLASS/USTRUCT/UFUNCTION declarations in custom feature macros. Stage a profile that physically omits the module. Native consumers that add the optional dependency may use `#if SIMURGHRTC_WITH_JANUS` for ordinary C++ call sites; runtime capability checks are still required.
