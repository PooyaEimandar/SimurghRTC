# Build mobile libwebrtc

Use this workflow to build the M0 native dependency for **Android ARM64** or **iOS device ARM64**. Each platform gets its own library and matching headers. The engine is read only; no engine patch is needed.

For local iOS testing, `--platform IOSSimulator` produces a separate ARM64 simulator artifact with its own Xcode pin. It also builds portable-C VPX from Unreal's checked source archive, because the engine's VPX binary is device-only. See the [simulator probe steps](MOBILE_PROBE.md#test-with-the-ios-simulator). Simulator results do not establish physical-device behavior or delivery size.

The build includes the native PeerConnection factory, data channels, audio processing, builtin audio codecs and software VP8/VP9. It excludes Java/Objective-C SDK wrappers, hardware capture, H.264 and AV1. Device permissions, hardware codecs and lifecycle integration come later.

See [validation results](M0_VALIDATION.md) for what has actually passed. A linked library does not establish packaged Unreal or physical-device support.

## What you need

Run commands from the repository root. Keep at least **30 GiB free per build workspace**, plus space for Unreal. Set `UE_ROOT` to the UE 5.8.3 directory containing `Engine`.

| Target | Pinned tools | Minimum target |
|---|---|---|
| Android | Linux x86-64 container, Python 3.13.5, NDK r27c (`27.2.12479018`), Clang 18.0.3 | Android API 26 |
| iOS | Apple Silicon Mac, Python 3.10.21, Xcode 27.0 (`27A266a`), SDK 27.0 (`24A430`), Apple Clang 21.0.0 | iOS 15.0, device ARM64 |
| iOS simulator | Apple Silicon Mac, Python 3.10.21, Xcode 27.1 (`27A9275`), SDK 27.1 (`24A94232`), Apple Clang 21.0.0 | iOS 15.0, simulator ARM64 |

The script checks these versions and the engine dependency hashes. Install the matching tools instead of bypassing a mismatch. It does not download Xcode or change your active Xcode installation.

The exact WebRTC commit, dependency commits, tool downloads, header/library hashes, flags and patches live in [the build lock](../Config/MobileWebRTC.lock.json). This uses WebRTC 5414 to match the current adapter. It is a compatibility baseline; the SR-003 security and distribution review is still open.

## Build iOS

```sh
UE_ROOT=/absolute/path/to/UnrealEngine
PYTHON=/absolute/path/to/python3.10

"$PYTHON" tools/mobile_build.py fetch --platform IOS \
  --work LocalBuilds/MobileIOS
"$PYTHON" tools/mobile_build.py build --platform IOS \
  --work LocalBuilds/MobileIOS --engine "$UE_ROOT" --python "$PYTHON" \
  --output LocalBuilds/Artifacts/IOS-arm64
```

Use `configure` instead of `build` to stop after generating Ninja files. `plan` prints the inputs without fetching or compiling anything.

For the simulator, use `--platform IOSSimulator` in both commands and a separate output such as `LocalBuilds/Artifacts/IOSSimulator-arm64`. The simulator pin follows the installed Xcode 27.1; the earlier device build keeps its 27.0 pin and evidence.

## Build Android

Docker provides the pinned Linux host. On Apple Silicon, it runs the x86-64 tools through emulation, so compilation takes longer.

```sh
UE_ROOT=/absolute/path/to/UnrealEngine
docker build --platform linux/amd64 -t simurghrtc-mobile-build:5414-v1 \
  -f Config/MobileWebRTC/Dockerfile Config/MobileWebRTC
docker volume create simurghrtc-mobile-android-5414

mobile_android() {
  docker run --rm --platform linux/amd64 \
    --mount "type=bind,src=$PWD,dst=/workspace" \
    --mount "type=bind,src=$UE_ROOT,dst=/engine,readonly" \
    --mount type=volume,src=simurghrtc-mobile-android-5414,dst=/build \
    simurghrtc-mobile-build:5414-v1 "$@"
}

mobile_android fetch --platform Android --work /build
mobile_android build --platform Android --work /build \
  --engine /engine --output LocalBuilds/Artifacts/Android-arm64
```

The container pins its Debian base by digest and packages by dated snapshot. GN, Ninja and the NDK have checked download hashes. The NDK's compiler and cached files are checked before use. The exported build records include the container package list.

Keep the Android workspace in the Linux volume. The NDK contains case-sensitive filenames that cannot be extracted on a typical Mac filesystem. Artifacts are exported to the repository's `LocalBuilds/Artifacts` directory.

## Use the result

Each output contains a stripped `Lib/libwebrtc.a`, headers, notices, source/patch records, GN arguments, observed tool versions and a link-check report. The adjacent `.symbols` directory keeps the unstripped archive. Final application dSYMs are produced when linking the iOS app.

Before export, the build links **every archive object** against the pinned Unreal OpenSSL, Opus and VPX libraries. The Android check uses Unreal's static C++ runtime and local symbol binding, and requires 16 KiB load-segment alignment in its test shared library. The final Unreal app still needs its own link-map, dependency, page-alignment and device checks.

Android uses the standard Itanium C++ ABI, matching Unreal. A recorded patch disables Chromium's relative vtables, and the export check verifies the pinned `rtc::Thread` vtable layout. Earlier Android exports without this fix must be rebuilt.

Stage an artifact into a fresh sample project:

```sh
python3 tools/prepare_profile.py --profile direct-only \
  --mobile-artifact LocalBuilds/Artifacts/Android-arm64 \
  --mobile-artifact LocalBuilds/Artifacts/IOS-arm64 \
  --output LocalBuilds/MobileDirect
```

See [artifact setup](MOBILE_ARTIFACTS.md) for format checks and [mobile clients](MOBILE_CLIENTS.md) for app-size measurement. Archive size is not download or installed app size.

## Check a repeat build

Build twice with `--fresh`, distinct `--build-name` values and new artifact paths. The tool refuses a nonempty Ninja directory and records whether compilation started from empty output. Keep the lock and build script unchanged between runs.

For example, after fetching simulator sources and setting `UE_ROOT` and `PYTHON`:

```sh
for run in a b; do
  "$PYTHON" tools/mobile_build.py build --platform IOSSimulator \
    --work LocalBuilds/MobileIOS --engine "$UE_ROOT" --python "$PYTHON" \
    --fresh --build-name "IOSSimulator-repeat-$run" \
    --output "LocalBuilds/Artifacts/IOSSimulator-repeat-$run" || exit 1
done
python3 tools/mobile_build.py compare --require-fresh \
  --first LocalBuilds/Artifacts/IOSSimulator-repeat-a \
  --second LocalBuilds/Artifacts/IOSSimulator-repeat-b
```

For Android, run the same two builds through `mobile_android`, using `--platform Android --work /build --engine /engine` and Android output names. The comparison runs on the host.

The comparison validates both manifests, then checks library bytes (including simulator VPX), headers, notices, patches, source/build locks, GN flags, observed tools and link inputs. Installation paths, host descriptions and the linked test binary's UUID can differ. The report records both manifest hashes; whole manifests can differ because they contain paths and diagnostic records.

Use `--require-fresh` for repeat-build acceptance. Without it, older artifacts can still be compared, but missing fresh-output records remain unverified. A resumed build is incremental and cannot pass this gate. Use a new output name after fixing a failed build when collecting repeat evidence.

These checks establish repeatability on the tested host. To check independent reproduction, fetch and build in a new workspace on another host with the same pinned tools. Save the comparison JSON with the build records; pinned inputs alone do not prove matching output.

## When a build stops

Use a new or script-created workspace. The tool never adopts a nonempty directory, changes a checkout to another revision, or overwrites an artifact. It rejects modified sources, unexpected files, patch drift, mismatched engine inputs and corrupted tool downloads.

Compiler details are in `<work>/src/out/<build-name>/compile.log`; configuration and link checks have their own logs beside it. A failed compile can be resumed without `--fresh`. If only toolchain pins change, a verified source checkout can be reused with a new output name; source revision changes need a new workspace and `fetch`. Do not edit downloaded sources without recording and reviewing a patch in the lock.

<details>
<summary>Why this build differs from the upstream SDK bundle</summary>

The native Unreal adapter supplies its own headless audio device and will connect Unreal media sources separately. The M0 target therefore builds the factory, software media and data dependencies without the platform SDK wrappers. It uses Unreal's OpenSSL, Opus and VPX headers and links their existing libraries; it does not embed duplicate codec or crypto implementations.

Recorded patches select this target, exclude unused Java/test dependencies, preserve native Android stack traces, route SRTP to external OpenSSL, and fix compiler compatibility for CRC32C and legacy stats conversions. License generation uses the verified GN executable directly. A checked GN overlay points CPU-feature detection at NDK r27c.

The builtin audio factory includes Opus, iSAC, G.722 and G.711. It also supports L16 and multichannel Opus without advertising them by default. The manifest lists these compiled codec families; negotiated formats and RTP auxiliary formats still need device capability checks. Debug information stays in the diagnostic archive; release archives are stripped. LTO and universal outputs are disabled. The explicit `IOSSimulator` artifact is kept separate from device artifacts.

Source: [WebRTC repository](https://webrtc.googlesource.com/src), [Android NDK r27c release](https://github.com/android/ndk/releases/tag/r27c). Exact fetched inputs and patch hashes are in the lock, rather than floating branch names.

</details>
