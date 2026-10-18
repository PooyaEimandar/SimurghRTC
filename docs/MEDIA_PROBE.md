# Native media test — Unreal 5.8.3

This test checks local video/audio codecs, texture updates, render-target capture and Unreal audio-mixer consumption. Enable it with `-SimurghRTCMediaProbe`.

Codec tests work without graphics or audio devices. Texture and mixer tests need a real graphics backend and an initialized audio mixer. The probe does not make a network call or capture a camera/microphone.

SR-005 added fixed input files (`simurghrtc-media-v1`) and a generated C++ adapter. Run `python3 tools/media_fixtures.py --check` to check their hashes. See [Fixtures](FIXTURES.md) for formats and [validation results](M0_VALIDATION.md) for the distinction between the earlier packaged test and the newer Editor test.

## What is exercised

1. Generate twelve 64×64 BGRA frames with four colored quadrants and changing brightness. Convert to I420 with libyuv, encode/decode software VP8 through the linked libwebrtc factories, and receive decoded-frame callbacks. Require every RTP timestamp and dimension to match, and quadrant RGB error ≤12. Retain only the latest decoded frame.
2. Generate one second of 440 Hz stereo PCM16 at 48 kHz. Encode 10 ms inputs into fifty 20 ms Opus packets, decode each packet through libwebrtc, and validate sample count, RMS and phase-independent tone correlation ≥0.95. This is a local codec path, without RTP, jitter buffering, packet loss, SDP, ICE or PeerConnection media delivery.
3. Upload a solid sentinel and then the decoded video frame to `UTexture2D::UpdateTextureRegions`. Draw each through public `FCanvas` into a linear `PF_B8G8R8A8` `UTextureRenderTarget2D`. Capture with public `FRenderTarget::ReadPixels`, with gamma conversion disabled. Require quadrant RGB error ≤3 for both uploads to detect stale content, swapped channels and inverted orientation.
4. Queue the decoded PCM once into a probe `USoundWaveProcedural`, play through a real UE audio component with `PlayWhenSilent` and zero volume, and require all 192,000 bytes to be consumed outside the game thread within ten seconds. Instrumentation excludes underrun silence. No audible output is intended; the result proves mixer consumption, not speaker audibility or echo cancellation.

No Pixel Streaming implementation, engine private rendering header, or engine source patch is used. `RHI`, `RenderCore`, `AudioExtensions`, `Json` and `SimurghRTCNative` are private dependencies of the optional Media module. Server and data-only profiles still omit the Media module; the codec fixture itself lives in Native.

## Ownership and bounds

| Owner | Work and lifetime |
|---|---|
| Game thread | Creates/destroys UObjects, starts the bounded probe, schedules texture uploads, draws the canvas, requests readback, queues the PCM fixture and writes the report. Strong references keep probe resources alive. |
| Private codec worker | Creates, drives and releases software VP8/Opus codecs. Encode/decode callbacks execute synchronously for the pinned VP8 implementation. Codecs release before callback objects; the worker stops before returning the result. This is a blocking feasibility helper, not a production session API. |
| Render/RHI threads | Consume the copied upload buffer. The upload cleanup callback owns and deletes the bytes and region. Readback serializes each upload/draw/capture; teardown flushes outstanding render commands. |
| UE mixer/procedural worker | Consumes the procedural sound queue. The game thread queues once before Play and only reads atomic instrumentation afterward. Stop/destroy the component and fence audio commands before releasing probe references; normal UE audio ownership handles retiring mixer sources. |

One video upload/draw/readback is in flight. Each BGRA frame is 16,384 bytes; at most four application-owned BGRA buffers overlap in the capture phase (latest decoded frame, expected pixels, upload, readback), plus two GPU textures. Codec-internal allocations are not included in that bound. The PCM fixture and UE queue are each capped at 192,000 bytes; no ongoing producer or unbounded streaming queue exists. Encoded packets are checked against 65,536 video bytes and 4,096 audio bytes. Readback deliberately stalls the game thread and GPU at this tiny resolution. Production capture still needs an asynchronous bounded readback ring, drop policy, pacing and GPU/CPU cost measurements under SR-202.

The generated sources and offscreen texture capture require no camera, microphone or screen-recording permission. Audio output uses UE's normal device; hardware input is never opened. Android/iOS permissions, audio sessions, lifecycle and separately built codec artifacts remain unverified.

## Reproduce

Build the full sample Editor target as described in [Building](BUILDING.md), then run all seven tests headlessly (`Automation RunTests SimurghRTC.`). `SimurghRTC.Native.MediaCodecs` repeats the codec fixture to exercise teardown and fresh decoder state.

Run the full integration probe with a real RHI, not `-NullRHI` or `-nosound`:

```bash
python3 tools/run_media_probe.py \
  --executable "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  --kind editor-game --project "$PWD/Samples/ContractHost/ContractHost.uproject" \
  --output "$PWD/Intermediate/Reports/MediaEditor"
```

The harness adds `-game -RenderOffscreen`, starts a unique process/run ID, requires JSON evidence and a completion marker, and waits for clean process exit. The probe emits one compact `SIMURGHRTC_MEDIA_REPORT` JSON log line as well as attempting a report file. Packaged Mac platform-file policy rejects the external report path here; the harness reads captured stdout and saves the validated report locally. Duplicate, malformed or stale log reports fail validation. `--negative null-rhi` and `--negative no-audio` require specific failure reports, not an arbitrary crash. Use a fresh evidence directory for each invocation. On this Mac, Unreal may ignore `RequestExitWithStatus`'s requested nonzero status; negative validation therefore also checks the exact report and engine warning.

For a packaged Direct-only run, stage a fresh host outside any `Intermediate` parent (see the source-engine dependency-cache limitation in [Native Server probe](NATIVE_PROBE.md)):

```bash
python3 tools/prepare_profile.py --profile direct-only --output LocalBuilds/NativeMediaDirect
PROJECT="$PWD/LocalBuilds/NativeMediaDirect/ContractHost.uproject"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHost Mac Development -Project="$PROJECT" -architecture=arm64
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostEditor Mac Development -Project="$PROJECT" -architecture=arm64
"$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$PROJECT" -noP4 -platform=Mac -clientarchitecture=arm64 -editorarchitecture=arm64 \
  -clientconfig=Development -skipbuild -nocompileeditor -cook -stage -pak \
  -map=/Engine/Maps/Entry '-AdditionalCookerOptions=-SkipZenStore -NullRHI -nosound' -unattended -utf8output
python3 tools/run_media_probe.py \
  --executable "$PWD/LocalBuilds/NativeMediaDirect/Saved/StagedBuilds/Mac/ContractHost.app/Contents/MacOS/ContractHost" \
  --kind game --output "$PWD/Intermediate/Reports/MediaGame"
```

NullRHI/nosound are cooker options only. The runtime harness uses Metal and UE's mixer. Repeat the last command with a new output directory and `--negative null-rhi` or `--negative no-audio`. Packaged evidence must report `editor_build=false`.

On this host the Game C++ compile/link succeeded but recursive Xcode app finalization failed in **Touch UBT generated tiles**, as in the Server build. After confirming compile/link success, standalone finalization and normal receipt generation recovered without engine changes:

```bash
PROJECT_DIR="$(dirname "$PROJECT")"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" -Mode=ApplePostBuildSync \
  -Input="$PROJECT_DIR/Intermediate/Build/Mac/arm64/ContractHost/Development/PostBuildSync_ContractHost.json" \
  -XmlConfigCache="$PROJECT_DIR/Intermediate/Build/XmlConfigCache.bin"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHost Mac Development \
  -Project="$PROJECT" -architecture=arm64 -ActionTypeFilter=WriteMetadata
```

Use this recovery only for that finalization failure, never to bypass compile/link errors. The Editor build and standard UAT cook/stage do not need this recovery here. Observed evidence is recorded in [M0 validation](M0_VALIDATION.md).

## Limits

These probes establish codec callbacks, CPU frame copies, plugin-accessible texture/render-target operations and UE procedural playback consumption. They do not establish a call, negotiated codecs, a production media transport, real-time performance at useful resolutions, APM/AEC routing, physical device capture, hardware codecs, remote-track sinks, mobile support, or perceptual audio quality. M0 remains open for its other contract, dependency, fixture and mobile gates.
