# Shared audio

Direct sessions can send microphone audio or generated PCM and receive decoded audio through the same C++ and Blueprint API. Audio travels over Opus media tracks; Direct HTTP sets up the connection, and DataChannels remain available alongside audio.

This first shared path accepts **48 kHz mono float PCM**, with up to 100 ms in each submission. Received audio can also play through an Unreal audio component. Mac microphone capture is implemented; mobile capture, echo cancellation and video remain separate work. The lower-level [native audio path](NATIVE_AUDIO.md) supports more input formats; the shared adapter returns `ServiceUnsupported` for those formats for now.

## Connect and attach a source

Use `SimurghRTCCore`, `SimurghRTCSession.h` and `SimurghRTCOperation.h`. Retain the session and pending operations in `UPROPERTY` fields. Calls and callbacks run on the game thread.

1. Create a session and call `CreateGeneratedSource(Session, Audio)`. Save `Result.Source` on success. This can happen before Connect.
2. Connect to your Direct endpoint with `Config.Purpose = ESimurghRTCPurpose::Call`.
3. After Connect succeeds, call `AttachSource(Session, Source)`. Save `Result.Track`.
4. Submit PCM through `Session->PushAudioFrame(Source, Frame)`.

C++ operations are dormant: bind `OnCompletedNative`, then call `Activate()`. Blueprint async nodes activate automatically and expose the same result fields. See [Shared sessions](SHARED_SESSIONS.md) for the connection and operation pattern.

The `Call` purpose reserves one bidirectional Opus track during the initial offer/answer. Attachment selects the source feeding that track; it does not renegotiate SDP. The server must admit that purpose. Call invitations, ringing and `Call`/`AcceptCall` operations remain unimplemented.

A frame contains copied, interleaved float samples in `[-1, 1]`:

```cpp
FSimurghRTCAudioFrame Frame;
Frame.SampleRate = 48000;
Frame.Channels = 1;
Frame.TimestampMicroseconds = NextTimestamp;
Frame.Samples = GeneratedSamples; // 1–4800 finite samples.
const FSimurghRTCError Error = Session->PushAudioFrame(Source, Frame);
```

Use increasing timestamps for each source. A successful submission copies all samples into a bounded queue. A full queue returns `ResourceExhausted` without taking any samples or consuming the timestamp; retry later. An unattached, stopped or muted source returns `InvalidState`.

The native worker groups accepted submissions into 10 ms blocks. Smaller submissions can combine into a block; mute, detach, replacement and shutdown discard any partial block. Submit at the audio rate, rather than sending a large burst every game tick. When input runs out, the transport sends silence.

## Use a microphone on Mac

Add `SimurghRTCMedia` to your client module and include `SimurghRTCMediaOperation.h`. Create the source with the same async operation pattern:

```cpp
FSimurghRTCSourceOptions Options;
Options.Kind = ESimurghRTCSourceKind::Microphone;
// Empty DeviceId selects the system default input.
auto* Capture = USimurghRTCMediaOperation::CreateSource(Session, Options);
// Retain Capture, bind its completion, then Activate().
// On success: save Result.Source and AttachSource after Connect succeeds.
```

`CreateSource` requests macOS microphone permission and opens Unreal's public AudioCapture stream on a worker. It completes only when capture starts. Permission denial returns `PermissionDenied`; a missing device or unsupported format returns `PlatformUnavailable`. Cancellation, timeout and owner loss discard the pending request, so a later permission response cannot publish a source or open a device. The application must include `NSMicrophoneUsageDescription` in its Info.plist and the audio-input entitlement if sandboxed. The sample includes both in `Build/Mac/Resources`; `prepare_profile.py` copies them into staged projects.

`GetDevices(Microphone, Devices)` returns a snapshot without requesting permission or opening a stream. Pass a returned opaque ID in `Options.DeviceId` to select that input. IDs may change; an empty ID uses the current system default. Labels remain generic until access is granted. Other platforms return `PlatformUnavailable` for capture for now; camera discovery remains unimplemented.

Capture converts common input rates (8, 16, 32, 44.1 and 48 kHz) to **48 kHz mono float PCM**. It requests one input channel and handles interleaved callbacks by downmixing. Higher rates are rejected. A fixed 200 ms queue drops oldest samples on overflow. Samples are forwarded only while attached, connected and unmuted; mute and detach clear queued capture. Do not call `PushAudioFrame` on a microphone handle.

Muting keeps the device open, so the OS microphone indicator can remain on. `StopSource`, disconnect, failure and shutdown discard capture immediately and close the device on its worker. At most four workers can be open or closing per process. A device failure or three seconds without callbacks fails the session with a structured connection error; automatic device recovery and switching are not implemented yet. These limits do not guarantee end-to-end latency.

Use `BindAudio` on the receiving track for Unreal playback as below. This first path has **no echo cancellation or noise suppression**. Use headphones for audible trials; the packaged echo fixture plays at zero volume.

## Receive and control audio

Bind `OnTrackChanged`. When it adds a remote audio track, save its handle and call `SetDecodedFrameDelivery(Session, Track, true)`. Decoded frames then arrive through `OnAudioFrame` on the game thread. Delivery starts disabled. Copy a frame if you need it after the callback; its timestamp is local decoded sample time, not the sender's capture time.

| Operation | Effect |
|---|---|
| `SetSourceMuted` | Clear pending outgoing samples and send silence; incoming audio continues |
| `ReplaceSource` | Use another source while keeping the local track handle |
| `DetachTrack` on a local track | Stop sending its source; the source can be attached again |
| `DetachTrack` on a remote track | Remove local delivery until reconnect; incoming packets are still drained |
| `StopSource` | Invalidate the source and remove its attached track |
| `SetDecodedFrameDelivery(false)` | Stop frame callbacks and discard queued delivery |
| `Disconnect` | Close the transport and invalidate all source and track handles |
| `Shutdown` | Suppress callbacks immediately and release the session |

Each session has at most eight audio sources and one attached local audio track. Handles from another session or an earlier connection are rejected. Creating, muting and stopping an unattached source do not require a connection.

The shared send queue holds at most 200 ms of PCM, in addition to the native queue's 200 ms bound. Each receive queue holds at most twenty decoded blocks and drops the oldest when full. These bounds limit storage; they are not a measured end-to-end latency guarantee. Disabling callbacks keeps draining incoming audio.

Data-only clients exclude audio. Audio capability becomes negotiated for a connected Direct session with the audio track. It does not prove that a local playback device is available; `BindAudio` checks that separately. Device permission and audio processing are separate from transport capability.

## Play received audio

Client builds with `SimurghRTCMedia` can bind a remote audio track to a registered `UAudioComponent` in a live game world:

```cpp
auto* Bind = USimurghRTCMediaOperation::BindAudio(Session, RemoteTrack, AudioComponent);
Bind->OnCompletedNative.AddLambda([](const FSimurghRTCOperationResult& Result)
{
    // Check Result.IsSuccess(); Result.Track identifies the bound track.
});
Bind->Activate();
```

Retain pending operations as usual. The matching Blueprint `Bind Audio` node uses the same path. Success starts playback and keeps the component referenced until replacement or teardown. The component's volume, attenuation and spatial settings remain under your control. Binding replaces its `Sound`; cleanup stops and clears that sound. The previous sound is not restored. Auto-destruction is disabled while bound and your original setting is restored afterward; normal component and actor lifetime rules still apply.

Playback and `OnAudioFrame` callbacks are independent: you can leave decoded callbacks disabled and still hear the remote track. Binding another component replaces the old sink. Another session cannot take over an actively bound component. Stopping, destroying or unregistering the component ends its binding on the next session tick; call `BindAudio` again to restart. Remote-track detach, disconnect, failure, owner loss and shutdown release the sink too.

The playback buffer holds at most **200 ms** of 48 kHz mono PCM. Overflow drops the oldest samples; underflow produces silence. The mixer consumes signed 16-bit PCM on its own thread, without calling session objects. Pausing a component keeps the buffer bounded. Teardown discards queued PCM; audio already mixed into the device buffer may still drain briefly. These limits are not an end-to-end latency measurement.

An unregistered component, wrong track or missing audio device returns an error. Server and data-only profiles do not include this client playback adapter. The playback reference needed for echo cancellation, output-device changes and audible quality acceptance remain open.

## Headless Server

The Server host uses `FDirectServer` and supplies its normal authorization callback. For a connected `call` session, use `MuteAudio(SessionId, false)` to enable its initially muted generated source, then `PushAudio(SessionId, Frame)`. `PollAudio(SessionId, Frame)` returns copied decoded frames; `HasRemoteAudio` reports the remote audio track. These internal provider methods run on the game thread and use the same bounded native worker as the shared client API.

The host does not automatically echo or mix audio. It decides what to generate or consume. No microphone, speaker, RHI or Editor is needed.

## Run the packaged check

Build Mac ARM64 Server and media-enabled Game packages with the current native audio artifact, following [Building](BUILDING.md). Run once for each client profile:

```sh
python3 tools/run_shared_audio_probe.py \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --client-project "$CLIENT_PROJECT" --client-executable "$CLIENT_EXE" \
  --output Intermediate/SharedAudioProbe
```

The runner uses separate packaged processes and fresh local credentials. It checks different tones in each direction, outgoing mute with incoming audio continuing, source replacement, decoded delivery disable/resume, detach, stop and native cleanup. It also submits alternating 7 ms and 13 ms blocks. The Server receives 400 Hz, silence, then 600 Hz; the client receives 800 Hz throughout. Measurements exclude the first second of each phase to allow queued audio and codec state to settle.

Add `--microphone --playback` to explicitly request microphone access and echo captured PCM through the separate Server into the Unreal mixer. Allow the macOS permission dialog when prompted. The check measures signal energy, mute, replacement with a generated source, and capture cleanup; it saves counters and measurements, not raw microphone samples.

Add `--playback` alone to run the client with Metal and Unreal's real audio mixer. The fixture sets component volume to zero and checks mixer consumption, sink replacement, continued playback while PCM callbacks are disabled, component destruction/rebinding and disconnect cleanup. The Server remains headless. This validates the mixer path without judging audible speaker quality.

On 2026-10-31, packaged Mac ARM64 **Direct-only and Full** clients passed all four audio phases against a separate dedicated Server. Both processes exited cleanly, with no peers left and temporary credentials removed. All 34 Unreal and 232 Python tests passed, along with the existing data-session regression (16 sessions, eight connected at once). Package checks confirmed ARM64 binaries, valid signatures and one isolated WebRTC library per executable.

On 2026-11-03, both Mac client profiles also passed `--playback` with CoreAudio. Each run consumed the decoded tone through three successive audio components and passed all four playback lifecycle checks. The headless audio regression and package audits passed, as did all **36 Unreal and 237 Python tests**. Both processes exited cleanly in every run. See the [playback validation record](reviews/SR201-audio-playback-validation.json).

The microphone implementation also passes **39 Unreal and 241 Python tests**, including conversion, bounded buffering, cancellation and capture-source ownership. Fresh Full and Direct-only packages pass the generated-audio headless and CoreAudio playback regressions against the rebuilt Server. Package audits confirm capture modules are client-only. See the [microphone validation record](reviews/SR201-microphone-validation.json) for the exact scope and deferred hardware check.

These are C++ execution tests of the shared reflected API. They do not execute a new Blueprint audio graph or qualify audible speaker sound, mobile audio, Internet paths or perceptual quality. Real microphone-to-Server-to-playback validation remains unverified: macOS denied access during the first packaged attempt, and the user deferred further microphone testing. No capture stream was opened. The earlier [shared audio validation](reviews/SR201-shared-audio-validation.json) retains its original scope and results.
