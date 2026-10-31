# Native audio transport

SR-201 starts M2 with generated PCM sent through real WebRTC audio tracks. Two peers negotiate Opus, exchange encrypted audio over local ICE, and receive decoded PCM. The path runs without a microphone, speaker, renderer or Unreal audio device.

This is an internal transport foundation. Shared session audio operations, Blueprint audio nodes, Direct HTTP media negotiation, microphone capture and Unreal speaker playback are still pending. M1's remaining acceptance gaps stay tracked in the [roadmap](../ROADMAP.md).

## What works

The native runtime can configure one local audio track before the first SDP exchange, copy PCM into its send queue, receive one remote track, and mute or resume the outgoing audio. DataChannels can use the same PeerConnection.

| Setting | Current behavior |
|---|---|
| Input | Interleaved float PCM in [-1, 1], mono or stereo |
| Sample rates | 8,000, 16,000, 32,000, 44,100 or 48,000 Hz |
| Submission size | Exactly 10 ms, using the format chosen at configuration |
| Resampling | WebRTC's sinc resampler converts input to 48 kHz before encoding |
| Input queue | At most 20 blocks (200 ms); a full queue returns `ResourceExhausted` |
| Output queue | At most 20 decoded blocks; overflow drops the oldest block and increments a counter |
| Clock | A private thread consumes one block every 10 ms; missing input produces silence |
| Mute | Clears queued input and sends silence; incoming audio continues |
| Shutdown | Stops the clock, removes the remote sink, closes the peer and releases its threads |

Accepted input is copied before return. Input timestamps must increase; a rejected submission does not consume its timestamp. Timestamps identify the application's sample sequence; they do not schedule future playback. Decoded timestamps are receiver-local PCM time, not remote capture time. Read the actual decoded channel count rather than assuming stereo was negotiated.

The native queue payloads are bounded to 38,400 bytes for input and 76,800 bytes for output, plus records, scratch buffers and resampler state. These figures exclude WebRTC's internal codec and jitter-buffer allocations. A slow consumer loses old audio rather than accumulating playback delay.

The generated source bypasses microphone audio processing. The linked APM remains available, but this path does not claim echo cancellation or noise suppression. Feeding the actual Unreal playback reference into AEC is separate SR-201 work.

## Build and run

Rebuild each selected platform library with [Native backend](NATIVE_BACKEND.md). The additive audio exports change the bridge pin, so earlier artifacts cannot be staged with the current sources.

Run the standalone native check:

```sh
python3 tools/run_native_audio_probe.py \
  --artifact "$NATIVE_ARTIFACT" --output Intermediate/NativeAudio
```

For Android or the iOS simulator, add `--device "$DEVICE_ID"`. An iOS device library accepts `--link-only`, which does not run an app or establish physical-device acceptance.

The fixture connects five pairs, covering all input sample rates and both input channel counts. It checks different tones in each direction, copied input, malformed frames, increasing timestamps, queue limits, receive overflow, mute/resume and teardown. It measures tone energy per decoded 10 ms block, allowing phase changes from jitter-buffer timing adjustments. The correlation threshold remains 0.9; separate checks require sufficient samples, signal level and silence while muted. These checks establish transport behavior, not perceptual audio quality.

After staging and packaging the sample with the new Mac library, run the Unreal check:

```sh
python3 tools/run_audio_probe.py \
  --kind server --executable "$SERVER_EXE" \
  --native-artifact "$MAC_ARTIFACT" --output Intermediate/UnrealAudio
```

The headless Unreal probe connects two local pairs on a dedicated worker. It checks 48 kHz mono and 44.1 kHz stereo input, decoded tones, mute/resume and four peer closures, while measuring game-thread ticks. `SimurghRTC.Native.AudioTransport` runs the same check in Unreal automation. Use a fresh output directory for every run.

## Integration boundary

`FNativeRuntime::ConfigureAudio`, `PushAudio`, `PollAudio`, `AudioState` and `MuteAudio` are internal synchronous methods. Use them on the runtime's owning worker thread. No WebRTC C++ objects cross the private C ABI, and no audio thread calls Unreal objects.

The public API still allows up to 100 ms per audio submission, but its source registry and media transport are not implemented yet. The internal 10 ms interface is a building block for that implementation, not a change to the public contract. Track replacement, renegotiation, multiple tracks, device changes, actual speaker playback and cross-device audio remain open.

Build identities, test results and unfinished acceptance items are kept in the [validation record](reviews/SR201-native-audio-validation.json). Earlier M0/M1 evidence describes its own recorded libraries; it does not validate these new audio exports.

## Validation status

On **2026-10-31**, the final native fixture passed all five audio pairs on Mac ARM64, the Galaxy S25 Ultra and the ARM64 iOS simulator: **15 pairs in total**. The packaged Unreal 5.8.3 Mac Server passed two pairs and closed all four peers, with no audio hardware. All 32 Unreal tests and 227 Python tests passed, along with the Server package audit and both HTTP factory regressions.

All four ARM64 libraries built. Native lifecycle and DataChannel regressions passed on Mac, the Galaxy and the simulator; iOS device audio and lifecycle consumers passed compile/link checks only. The Galaxy's final measurement rerun passed after reconnection. Temporary Android executables and all five disposable simulators were removed.
