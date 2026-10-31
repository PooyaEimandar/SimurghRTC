# Shared generated audio

Direct sessions can send generated PCM and receive decoded audio through the same C++ and Blueprint API. Audio travels over Opus media tracks; Direct HTTP sets up the connection, and DataChannels remain available alongside audio.

This first shared path accepts **48 kHz mono float PCM**, with up to 100 ms in each submission. Microphones, Unreal speaker playback, echo cancellation and video are still separate work. The lower-level [native audio path](NATIVE_AUDIO.md) supports more input formats; the shared adapter returns `ServiceUnsupported` for those formats for now.

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

Each session has at most eight generated sources and one attached local audio track. Handles from another session or an earlier connection are rejected. Creating, muting and stopping an unattached source do not require a connection.

The shared send queue holds at most 200 ms of PCM, in addition to the native queue's 200 ms bound. Each receive queue holds at most twenty decoded blocks and drops the oldest when full. These bounds limit storage; they are not a measured end-to-end latency guarantee. Disabling callbacks keeps draining incoming audio.

Data-only clients exclude audio. Audio capability becomes negotiated for a connected Direct session with the audio track. This does not claim microphone, speaker or audio-processing support.

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

On 2026-10-31, packaged Mac ARM64 **Direct-only and Full** clients passed all four audio phases against a separate dedicated Server. Both processes exited cleanly, with no peers left and temporary credentials removed. All 34 Unreal and 232 Python tests passed, along with the existing data-session regression (16 sessions, eight connected at once). Package checks confirmed ARM64 binaries, valid signatures and one isolated WebRTC library per executable.

This is a C++ execution test of the shared reflected API. It does not execute a new Blueprint audio graph or qualify speaker sound, microphone input, mobile audio, Internet paths or perceptual quality. Results and remaining work are recorded in [SR-201 shared audio validation](reviews/SR201-shared-audio-validation.json).
