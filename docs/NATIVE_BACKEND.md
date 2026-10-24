# Isolated WebRTC backend

WebRTC **M155** is integrated into the Unreal 5.8.3 plugin. Packaged Mac/Android lifecycle and media probes pass; both iOS simulator profiles pass lifecycle and local codecs. The Mac dedicated Server passes headless factory, HTTP and cleanup checks. Fresh repeat builds pass on all four ARM64 targets, including iOS device. The [source review](NATIVE_SOURCE_REVIEW.md) resolves the known findings; independent-host reproduction and physical iOS acceptance remain open. See [M0 results](M0_VALIDATION.md) for the exact scope.

The backend builds its own WebRTC, BoringSSL, Opus and VPX. Unreal calls a small private C interface. Factory handles stay inside the backend; Unreal owns the buffers used to copy decoded video and PCM. No WebRTC C++ types or allocations cross that boundary.

The public C++ and Blueprint API is unchanged. M1 adds [asynchronous peers and SDP/ICE exchange](NATIVE_PEERS.md), with real local connection tests. Direct/Janus session providers remain unfinished.

The SR-101 bridge adds signaling functions and changes the artifact input pin. Rebuild all selected platform artifacts before staging current sources. Earlier M0 package and repeat-build records describe the earlier bridge; they remain historical evidence.

## Build Apple libraries

Use Apple Silicon, Python 3.12 or newer and Xcode 27.1. The script checks the pinned compiler downloads, source revisions and SDK versions. Mac, iOS device and iOS simulator each produce a separate ARM64 artifact.

```sh
python3 tools/build_native_backend.py --platform Mac \
  --work LocalBuilds/NativeBackend --output LocalBuilds/Artifacts/M155-Mac
python3 tools/build_native_backend.py --platform IOSSimulator \
  --work LocalBuilds/NativeBackend --output LocalBuilds/Artifacts/M155-IOSSimulator
python3 tools/build_native_backend.py --platform IOS \
  --work LocalBuilds/NativeBackend --output LocalBuilds/Artifacts/M155-IOS
```

Run these sequentially when sharing a source workspace. An interrupted compile can resume. Each export needs a new output directory. The [Apple lock](../Config/NativeWebRTC.lock.json) records the selected sources and tools.

## Build Android

Android uses a separate pinned Linux x86-64 builder, NDK r27c and M155's Clang/GN/Ninja tools. The [Android lock](../Config/NativeWebRTC.android.lock.json) records their checksums, source revisions and native-only build patches. The container includes C++ headers for upstream host code generators. Before export, the builder links a C consumer with Unreal's NDK r27c and checks 16 KiB load alignment. Compiler helpers required by libyuv stay inside the isolated backend.

```sh
docker build --platform linux/amd64 -t simurghrtc-native-m155:v1 \
  -f Config/NativeWebRTC/Dockerfile.android Config/NativeWebRTC
docker volume create simurghrtc-native-m155
docker run --rm --platform linux/amd64 \
  --mount "type=bind,src=$PWD,dst=/workspace" \
  --mount type=volume,src=simurghrtc-native-m155,dst=/build \
  simurghrtc-native-m155:v1 --work /build \
  --output /workspace/LocalBuilds/Artifacts/M155-Android --jobs 6
```

A fresh workspace downloads and verifies its own NDK. To reuse an existing verified r27c workspace, mount it read-only at `/ndk` and add `--ndk-cache /ndk`. Keep the source/NDK workspace in a Linux volume: the NDK needs a case-sensitive filesystem. The old M109 workspace is not modified.

## Check repeat builds

Use two new Ninja directories and two new exports. `--fresh` refuses to reuse compiled outputs. For example:

```sh
python3 tools/build_native_backend.py --platform Mac --work LocalBuilds/NativeBackend \
  --build-name MacRepeatA --fresh --output LocalBuilds/Artifacts/M155-Mac-a
python3 tools/build_native_backend.py --platform Mac --work LocalBuilds/NativeBackend \
  --build-name MacRepeatB --fresh --output LocalBuilds/Artifacts/M155-Mac-b
python3 tools/compare_native_builds.py \
  --left LocalBuilds/Artifacts/M155-Mac-a --right LocalBuilds/Artifacts/M155-Mac-b \
  --baseline LocalBuilds/Artifacts/M155-Mac --output Intermediate/M155-Mac-repeat.json
```

Use `IOS` or `IOSSimulator` and distinct build names for the Apple mobile targets. The Android builder accepts the same `--build-name` and `--fresh` options inside its container. Run builds sequentially when sharing a workspace. To resume an interrupted build, omit `--fresh`; that resumed export must not count as a fresh repeat run.

The comparison requires the shipped library, header, flags, patches and notice inputs to match byte for byte, including the accepted baseline. Build paths and debug-object hashes are retained separately. Artifact manifest hashes can differ because they include those build records. No library bytes are rewritten for comparison.

Run the C ABI fixture against each rebuilt export:

```sh
python3 tools/run_native_abi_probe.py --artifact LocalBuilds/Artifacts/M155-Mac-a \
  --output Intermediate/M155-Mac-a-runtime
```

For Android, add `--device ADB_SERIAL`; the runner uses NDK r27c and removes its temporary device executable afterward. For the simulator, add `--device SIMULATOR_UDID`. Each run checks six peer lifecycles, overlapping owners, invalid ABI inputs and two VP8/Opus fixture passes. It is a standalone native consumer, not a new Unreal package run.

For an iOS **device** artifact, compile and link the consumer without signing or running it:

```sh
python3 tools/run_native_abi_probe.py --artifact LocalBuilds/Artifacts/M155-IOS-a \
  --output Intermediate/M155-IOS-a-link --link-only
```

The report explicitly records `runtime_validation: not_run`. Run this for both device repeat exports. A successful link is not physical-device acceptance.

Separate output directories on one host establish a same-host repeat check. They do not establish independent-host reproduction or physical iPhone behavior.

## Stage an Unreal project

Every enabled target needs its own checked artifact. A mobile project also needs the Mac artifact for its Editor and cooking build:

```sh
python3 tools/prepare_mobile_probe.py --profile direct-only \
  --artifact LocalBuilds/Artifacts/M155-Mac \
  --artifact LocalBuilds/Artifacts/M155-IOSSimulator \
  --simulator-support LocalBuilds/SimulatorSupport/nghttp2-arm64 \
  --output LocalBuilds/M155SimulatorDirect --app-id org.simurghrtc.m155.direct
```

Use `--profile full` and a different output/app ID for Janus-enabled packaging. Android uses its own artifact and does not need simulator support. Follow [Mobile probe](MOBILE_PROBE.md) for Unreal build, package and device commands.

For a desktop project, use `tools/prepare_profile.py --native-artifact` with the Mac export. UBT checks the selected source lock, bridge inputs and all payload hashes. Old M109 exports are retained as historical evidence and are rejected by the new adapter.

## What the probes establish

The client probe creates and closes nine PeerConnections, then encodes and decodes twelve generated VP8 frames and one second of stereo Opus. It records the selected artifact and backend version. This checks lifecycle and local codecs; no remote peer, camera or microphone is used.

The separate [Unreal media probe](MEDIA_PROBE.md) checks texture upload, render-target readback and audio-mixer consumption. Pass `--native-artifact` to `tools/run_media_probe.py` to require the replacement library's identity in its report.

Nine C entry points are exported, including signaling, event polling, and an explicit loopback-only probe switch. Normal peer creation does not enable that switch. Internal RTC and codec symbols are local to the native object. The codec set remains software VP8/VP9 and builtin audio; H.264, AV1 and hardware capture are excluded from this M0 build. License inputs travel with each artifact; final publication notices remain SR-902 work.

## Changing pinned inputs

After changing the bridge or a source lock, run `python3 tools/native_artifacts.py` to refresh the checked-in UBT inputs, then rebuild the artifacts. The current bridge-input pin is shared by all four targets. Staging checks these inputs before Unreal compiles. The builders also reject source/configuration changes made during compilation.
