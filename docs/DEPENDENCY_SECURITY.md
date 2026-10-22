# Dependency security work (SR-008)

**In progress, 2026-10-22.** The simulator HTTP/2 dependency has been updated. The WebRTC, crypto and codec replacement is still being qualified. The current RTC dependency set is **not approved for untrusted network use or release**.

This work is limited to dependencies used by the Unreal plugin and its test apps: pinned builds, library compatibility, packaging and local lifecycle/media checks.

## What changed

The simulator probe now builds **nghttp2 1.70.0** from a checked source archive. The [lock](../Config/IOSSimulatorProbe.lock.json) records the archive, source headers and the existing Unreal 1.64.0 consumer headers separately. This replaces the vulnerable 1.64.0 support export without editing Unreal. The [upstream advisory](https://github.com/nghttp2/nghttp2/security/advisories/GHSA-6933-cjhr-5qg6) identifies CVE-2026-27135 and its fix; [1.70.0](https://github.com/nghttp2/nghttp2/releases/tag/v1.70.0) includes that fix.

A local C regression covers a valid exchange and two session-termination cases. The termination cases fail with the old library and pass with the new one. The tests compile against Unreal's existing headers and check the C API paths used by the fixture. They do not cover every HTTP/2 API or establish whether Unreal's normal HTTP client enables the affected callbacks.

Two fresh ARM64 simulator builds produce identical library bytes. Staging and Unreal build rules reject retired support exports, including an old export whose own checksums are internally consistent. Historical exports remain readable by the inventory tool and keep their original identities.

Packaged UE 5.8.3 Direct-only and Full probes also pass on the ARM64 iOS simulator. Each creates and closes nine PeerConnections and verifies app shutdown. Their link maps select the updated HTTP/2 library. These earlier HTTP/2 acceptance runs use the original WebRTC artifact; the later M155 runs are recorded separately below.

The inventory now also checks engine archives against Epic's `Commit.gitdeps.xml`. This establishes which distributed bytes are present. It does **not** establish the upstream source revision, patch history or security status of those binaries.

## Packaged M155 integration

The plugin now uses a private C interface to separately built M155, BoringSSL, Opus and VPX. Mac, Android, iOS device and iOS simulator ARM64 exports are built from pinned inputs. Only six C entry points remain externally visible.

Packaged Mac Direct-only and Android Direct-only/Full pass lifecycle, local codecs, render-target capture and silent audio-mixer checks. Both iOS simulator profiles pass lifecycle and local codecs without graphics/audio devices. Link audits select one isolated WebRTC archive per package. The iOS device library is built but not device-tested. The packaged Mac dedicated Server also passes builtin-media and data-only factory startup, real loopback HTTP responses and occupied-port cleanup in both modes. Its [snapshot](reviews/SR008-M155-server-validation.json) records the exact artifact and executable.

See the [build guide](NATIVE_BACKEND.md), [results](M0_VALIDATION.md#sr-008-packaged-m155-integration) and [snapshot](reviews/SR008-M155-validation.json). These tests establish local integration. Two fresh builds per target now reproduce the accepted Mac, Android and simulator library bytes, and all six native consumers pass. See the [repeat snapshot](reviews/SR008-M155-repeat-validation.json). Independent-host reproduction, the iOS device repeat build, source/advisory qualification and final notices remain open. Unreal's other crypto, HTTP and audio dependencies remain engine-owned.

## Original isolation candidate

The [candidate lock](../Config/NativeWebRTC.candidate.lock.json) pins WebRTC M155 to the revision referenced by Chromium 155.0.8059.85. It also pins the required source repositories, Clang, GN, Ninja and the Mac SDK. This records the original standalone experiment. The plugin now selects the [M155 C ABI backend](NATIVE_BACKEND.md); its packaged acceptance is recorded separately.

The Mac ARM64 test builds WebRTC with its own BoringSSL, Opus and VPX. It combines the native objects behind two private C entry points, then links them beside Unreal's crypto and codec archives. The symbol check and six factory/PeerConnection lifecycles pass. Both library sets report their own versions. Opus reports `libopus unknown`; its source identity comes from the pinned Chromium `third_party` tree, not that string.

Two separate Ninja build directories on the same Mac produce identical archives and isolated objects. The linker normalizes build-directory paths in its debug records. Both outputs pass the lifecycle test; independent-host reproduction is still open.

This original experiment proved isolation in a small Mac executable. It did not establish packaged Unreal or mobile behavior. See [M0 validation](M0_VALIDATION.md) for the later integration runs; earlier results retain their recorded WebRTC artifacts.

To repeat the candidate check, use Mac ARM64, Xcode 27.1 and Python 3.12 or newer:

```sh
python3 tools/build_native_candidate.py --engine "$UE_ROOT" \
  --work LocalBuilds/NativeCandidate --output Intermediate/NativeCandidate-a
python3 tools/build_native_candidate.py --engine "$UE_ROOT" \
  --work LocalBuilds/NativeCandidate --build-name MacCandidateRepeat \
  --output Intermediate/NativeCandidate-b
```

The first command fetches the pinned inputs into a new workspace. The second uses a separate Ninja output directory. Neither command installs the candidate into the plugin.

## Approved replacement design

On 2026-10-19, the maintainer selected **separately built, maintained dependencies for desktop and mobile**. This replaces ADR-005's requirement to use Epic's desktop WebRTC. The API v1 contract stays unchanged.

The replacement must:

- Pin WebRTC and its native dependencies to upstream commits. Record local patches and how known upstream issues are addressed.
- Build crypto and codecs from those sources instead of reusing Unreal's opaque OpenSSL/Opus/Speex archives.
- Keep WebRTC and dependency types private. Isolate internal symbols and allocation ownership so Unreal cannot accidentally call a different crypto or codec implementation.
- Preserve one selected WebRTC implementation per target, separate device/simulator ARM64 artifacts, standard Unreal builds and no engine source patches.
- Pass repeat builds, symbol/link audits, lifecycle and media probes before replacing the accepted feasibility artifacts.

A newer version number alone is not approval. This work qualifies the plugin's RTC dependencies and the simulator test-app support library. Unreal's other HTTP, audio and runtime libraries remain engine-owned; isolating RTC does not upgrade those components.

## Remaining work

| Work | Status |
|---|---|
| Simulator nghttp2 update | Regression, repeated archive builds and packaged Direct-only/Full simulator probes pass |
| Epic archive distribution identity | Verified against the engine manifest |
| Maintained WebRTC/crypto/codec replacement | Mac/Android/simulator integration and same-host fresh repeat builds passed; independent-host and iOS device repeat checks remain open |
| Known upstream issues in RTC dependencies | Open until the replacement sources and enabled features are checked |
| Replacement ABI, one-copy linkage and runtime acceptance | Local checks passed on Mac clients/Server, Samsung and simulator; physical iOS and Windows/Linux still need replacement-package validation |
| Final artifact/app notices and patent grants | Separate publication gate under SR-902 |

The [SR-003 review](DEPENDENCY_REVIEW.md) remains the historical baseline. It must not be relabeled as approval of replacement libraries. [M0 validation](M0_VALIDATION.md) records accepted runs and their limits.

## Reproduce the HTTP/2 checks

Use the pinned Python/Xcode combination described in [Mobile builds](MOBILE_BUILDS.md). Build twice into fresh directories:

```sh
python3.10 tools/ios_simulator_support.py --engine "$UE_ROOT" --output LocalBuilds/HTTP2-a
python3.10 tools/ios_simulator_support.py --engine "$UE_ROOT" --output LocalBuilds/HTTP2-b
cmp LocalBuilds/HTTP2-a/libnghttp2.a LocalBuilds/HTTP2-b/libnghttp2.a
```

For the regression, obtain the two source archives listed in the lock. The tool checks their hashes before extraction:

```sh
python3 tools/test_http2_security.py --engine "$UE_ROOT" \
  --baseline-archive /absolute/path/to/nghttp2-1.64.0.tar.gz \
  --current-archive /absolute/path/to/nghttp2-1.70.0.tar.gz \
  --output Intermediate/HTTP2-regression
python3 -m unittest discover -s tools/tests -v
```

The old library is built only as a local regression control. Rebuild and stage new simulator support before following the [packaged probe instructions](MOBILE_PROBE.md).
