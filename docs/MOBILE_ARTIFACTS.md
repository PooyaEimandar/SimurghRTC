# Prepare mobile WebRTC artifacts

**Historical M109 workflow.** The current plugin requires the [isolated M155 backend](NATIVE_BACKEND.md). The format and commands below preserve earlier build evidence; their exports cannot be used with the new adapter.

**Status: experimental build integration.** Android/iOS can select a separately built ARM64 library. Use the [pinned source-build workflow](MOBILE_BUILDS.md) to produce one, and check [validation results](M0_VALIDATION.md) for current evidence. Packaged physical-device validation remains open. Generated binaries are not committed to this repository.

This page describes the SR-006/SR-007 artifact format and staging checks. It lets us check, share and reuse a dependency without copying it into the engine.

## Use an existing artifact

Check it, then stage a fresh project:

```sh
python3 tools/mobile_artifacts.py check /absolute/path/to/android-artifact
python3 tools/prepare_profile.py --profile direct-only \
  --mobile-artifact /absolute/path/to/android-artifact \
  --output LocalBuilds/AndroidDirect
```

Use an iOS artifact the same way. Repeat `--mobile-artifact` to include one artifact for each platform. Full, Direct-only, Janus-only and Data-only all use this mechanism. The Server profile rejects mobile artifacts.

The staged plugin stores each dependency under `ThirdParty/WebRTC/<Android-or-IOS>/arm64`. Unreal selects the target's artifact and checks its identity and file hashes when evaluating build rules. Missing or incompatible artifacts stop the build with an explanation. The earlier desktop adapter used the engine library.

Simulator testing uses `ThirdParty/WebRTC/IOSSimulator/arm64` and `-architecture=iossimulator`. Its manifest identifies platform `IOSSimulator` and dependencies `unreal-openssl-opus-simulator-vpx`; it additionally contains `Lib/libvpx.a`, `Build/vpx-build.json` and the VPX license. Both archives must contain simulator objects. An iOS device build cannot select this artifact.

An artifact passing these checks has **not** passed ABI, link, codec, security or device acceptance. Android ELF objects identify the CPU, not the Android API level. Those checks need the actual build records and packaged probe. Other enabled plugins may introduce another WebRTC copy; inspect the final application's link map before accepting it.

## Package a dependency build

Prepare the directory below from a completed build. Keep debug symbols outside it.

```text
dependency-input/
  Include/                      Matching WebRTC and dependent headers
  Lib/libwebrtc.a                One Release ARM64 static archive
  Build/args.gn                 Exact GN arguments
  Build/source-lock.json        Source/dependency pins and patch list
  Build/recipe.txt              Commands, environment and tool versions
  Build/patches/                Applied patches, when needed
  Licenses/THIRD_PARTY_NOTICES.txt
```

Write the metadata described below, then run:

```sh
python3 tools/mobile_artifacts.py package \
  --input /absolute/path/to/dependency-input \
  --metadata /absolute/path/to/metadata.json \
  --output LocalBuilds/Artifacts/Android
```

The output must be new. The tool copies the input, adds `artifact.json` with a SHA-256 for every payload file, then validates it. A failed output remains available for inspection. It does not download or compile dependencies, infer missing provenance, or measure app download size.

The initial adapter uses the same 5414-era C++ API as the desktop probe. Its archive must use Unreal's **OpenSSL, Opus and VPX** dependencies and the target's libc++. A stock upstream archive with bundled BoringSSL/codec copies is incompatible. The exact source revision, necessary patches and toolchain compatibility still need qualification; the adapter name is not a source pin or a supported security baseline.

[SR-008](DEPENDENCY_SECURITY.md) is replacing this adapter with an isolated native dependency build on desktop and mobile. Its M155 candidate is not accepted by this artifact format; do not substitute it into an existing 5414-era export.

Android also requires the standard Itanium C++ ABI. Rebuild earlier artifacts that lack `toolchain.cpp_abi: "itanium"`; Chromium's relative vtables crash when called from Unreal. The pinned build now disables them and checks the resulting thread vtable before export.

Version 1 is limited to the factory and software-media probes: dummy audio device, network monitor disabled, no Java/AAR payload or hardware codec helpers. Camera, microphone, JNI/Objective-C integrations and platform lifecycle support remain separate mobile work. Both media and data-only profiles currently use the same media-capable dependency; no size saving is claimed.

<details>
<summary>Metadata and validation reference</summary>

Replace the descriptive values in this metadata template with real build facts. `source_revision` must be the full 40-character lowercase WebRTC Git commit.

```json
{
  "schema": 1,
  "artifact_id": "android-arm64-build-1",
  "platform": "Android",
  "architecture": "arm64",
  "engine_version": "5.8.3",
  "adapter": "webrtc-5414-ue5.8.3-v1",
  "configuration": "Release",
  "dependencies": "unreal-openssl-opus-vpx",
  "source_revision": "REPLACE_WITH_EXACT_GIT_COMMIT",
  "toolchain": {
    "host": "Linux",
    "compiler": "REPLACE_WITH_COMPILER_VERSION",
    "sdk": "REPLACE_WITH_NDK_VERSION",
    "minimum_os": "REPLACE_WITH_ANDROID_API_LEVEL",
    "cpp_runtime": "libc++",
    "cpp_abi": "itanium"
  },
  "codecs": {
    "audio": ["opus"],
    "video": ["VP8"]
  }
}
```

For iOS, use `platform: "IOS"`, `host: "Mac"`, an iOS SDK version and the deployment target. Record all enabled codecs; the initial probe requires Opus and VP8. The locally inspected UE 5.8.3 configuration names NDK `27.2.12479018` (r27c) as its main Android version and iOS 15.0 as its minimum OS. Metadata records these choices; it does not configure Unreal's SDK or app deployment target. Match the dependency and application settings before building.

`Build/source-lock.json` has `webrtc_revision` matching the metadata, a nonempty `dependencies` object mapping dependency names to exact Git commits or SHA-256 values, and a `patches` array of paths under `Build/patches/`. An empty patch array means no patches were applied. Include depot_tools/build tools and transitive source pins; the validator checks their format, not whether the list is complete or those sources produced the archive.

The input must include the full matching headers, including `api/create_peerconnection_factory.h`, `rtc_base/thread.h`, `third_party/abseil-cpp/absl/base/config.h` and `third_party/libyuv/include/libyuv.h`. Record generated headers and compile definitions in the recipe too.

The Python tool rejects missing, changed or extra files; unsafe paths, symlinks and case collisions; unexpected binary payloads; empty/thin archives; non-ARM64 objects; and objects built for the wrong platform. Device and simulator archives cannot be interchanged. Ordinary GNU and BSD archives are supported. LLVM bitcode/LTO and universal archives are intentionally unsupported in this first format. The optimization work can extend it after actual linker/device validation.

UBT rechecks platform, architecture, adapter, engine, dependency selection, required files and the complete SHA-256 inventory. File inputs are registered as external build dependencies. The archive object inspection runs in the packaging/staging tool. Neither check proves compatibility with another plugin's crypto symbols or validates Android's final shared-library page alignment.

</details>

## Verify changes to these tools

Run `python3 -m unittest discover -s tools/tests -v`. This needs no Unreal installation. The mobile fixtures are synthetic test inputs, not distributable libraries.

After building `ContractHostEditor`, exercise the validator in the actual UBT-generated rules DLL:

```sh
python3 tools/test_mobile_rules.py --engine /absolute/path/to/UE_5.8 \
  --rules Samples/ContractHost/Intermediate/Build/BuildRules/ContractHostModuleRules.dll \
  --output Intermediate/MobileRulesCheck
```

Use a new output directory. To also check simulator support and rejection of retired exports, add `--simulator-support /absolute/path/to/current-support-export` after rebuilding the rules DLL from the current sample.

Also rerun the [Unreal tests](BUILDING.md#2-build-and-run-unreal-tests) after changing native selection. The [build workflow](MOBILE_BUILDS.md) checks source/toolchain inputs and linkage; signed device probes and [app-size validation](MOBILE_CLIENTS.md#app-size-validation) remain separate gates. The [dependency review](DEPENDENCY_REVIEW.md) records the remaining security, provenance and notice requirements. A valid manifest does not approve a dependency for release.
