# SimurghRTC mobile clients

**Status: packaged lifecycle, local-media and C++/Blueprint Direct-data checks pass on Android and the iOS simulator. Broader mobile acceptance is in progress.** See [Mobile data](MOBILE_DATA.md) for HTTPS/TURN, reconnect, background/resume, channel options, limits and backpressure coverage. [Native backend](NATIVE_BACKEND.md) describes the isolated M155 builds; [M0 results](M0_VALIDATION.md) records earlier package evidence. Physical iOS acceptance and Shipping size comparisons remain open.

## Targets and dependency artifacts

Mobile clients use the same C++ and Blueprint API for Direct Unreal and Janus. Desktop, Android and iOS use separately built, pinned WebRTC libraries behind the same private C interface. Mobile targets are clients only.

| Target | Dependency build host | Planned artifact and integration | Required evidence |
|---|---|---|---|
| Android `arm64-v8a` | Linux with pinned WebRTC and UE-compatible Android toolchains | ARM64 native static/shared library plus required Java/JNI helpers; package helpers in an AAR where appropriate | Standard UE packaged app creates and tears down a PeerConnection on a physical Android device |
| iOS device `arm64` | macOS with pinned Xcode/iOS SDK | ARM64 static library/framework with required Objective-C++ helpers; distribute as an XCFramework when useful | Signed standard UE packaged app creates and tears down a PeerConnection on a physical iPhone/iPad |
| Simulator/emulator | Separate development configuration where supported | Separate artifacts and symbols; exclude simulator slices from device app payloads | Development convenience only; does not replace device evidence |

Final linkage and packaging are selected by [SR-006 and SR-007](../ROADMAP.md#e02-native-feasibility-and-test-fixtures). Audit UE's module availability and dependencies first, then select the mobile libraries and private adapter explicitly without engine-source patches. Never link both engine-bundled and separately built libwebrtc into one application target.

Build the dependencies once per pinned revision, patches, platform, architecture, toolchain and configuration, then reuse them in plugin/application builds. Plugin consumers receive versioned artifacts; they do not need a WebRTC source checkout for routine packaging. Retain build scripts, a dependency manifest, checksums and third-party notices, and make artifacts reproducible from a clean build host. Keep build outputs out of source control and distribute permitted binaries through versioned release artifacts.

Use matching bridge sources and reviewed WebRTC revisions across the plugin's platform artifacts. Keep their symbols isolated from Unreal's own libraries, and validate wire interoperability against the desktop server and Janus. Pin matching headers and libraries. Verify C++ runtime/ABI settings, dependent crypto libraries, native symbol collisions, minimum OS versions and Android native-library page-size requirements for the selected UE/toolchain. Record security updates and rebuild dependencies through the same qualification process.

Upstream references: [Android build workflow](https://webrtc.googlesource.com/src/+/main/docs/native-code/android/), [iOS build workflow](https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/ios/README.md), [build options](https://webrtc.googlesource.com/src/+/main/webrtc.gni). Exact tool versions and flags must be recorded for our pinned revision rather than copied from an unrelated release.

## ARM64 optimization policy

- Produce release dependencies for Android `arm64-v8a` and iOS device `arm64`. Keep other architecture/debug builds out of the delivered device app.
- Disable dependency tests, examples and developer tools in release artifact builds. Inspect the final link/package to verify what actually ships; disabling a build target alone is not evidence of app-size savings.
- Select the smallest codec set that passes the declared Direct Unreal and Janus media contracts. Start with a validated software fallback and assess platform hardware codecs separately. Publish enabled encoders/decoders, negotiated formats and unsupported combinations for each artifact.
- Evaluate compiler size optimization, supported link-time optimization and dead-code elimination against the same functional and performance baseline. Preserve JNI entry points and required reflected/native symbols. Keep matching Android native symbols and iOS dSYMs as separate diagnostic artifacts.
- Treat hardware-only or reduced-codec builds as optional configurations with explicit device/service limits. Do not remove fallback or advanced-demo requirements silently to meet a size target. Janus codec compatibility and SFU layer support require actual negotiation tests.
- Keep optional effects, segmentation models and demo assets separate from the core RTC measurement. Full, Direct-only, Janus-only and Data-only are client feature profiles; evaluate size for each. A Data-only mobile dependency may omit media where supported, but must retain the required secure DataChannel stack.

## Mobile integration and runtime acceptance

[SR-107](../ROADMAP.md#e04-unreal-server-and-client-integration) validates mobile signaling and DataChannels; [SR-208](../ROADMAP.md#e06-media-lifecycle-and-unreal-playback) validates mobile media. Both use packaged Unreal clients and the common C++/Blueprint API.

Required device scenarios include:

- Camera/microphone permissions, denial and revocation; camera selection/switching, orientation and capture teardown.
- Audio focus/session interruptions, speaker/headset/Bluetooth route changes, mute and recovery, with a defined echo-cancellation reference and no unintended double processing.
- Local preview, remote video textures, audio playback, generated/render-target sources, hardware codec selection and software fallback or explicit unsupported results.
- Foreground/background transitions, screen lock, resume and process termination. The data samples now pass app-switch tests by shutting down on deactivation and creating fresh sessions on reactivation. Screen lock and process termination remain open; do not promise unrestricted background camera, microphone or network activity.
- Wi-Fi/cellular changes, forced TURN, constrained networks and reconnect with fresh or reconciled state. Test bounded queues, cancellations and callbacks after their Blueprint/UObject owners are destroyed.
- Direct Unreal and applicable Janus services, including calls with desktop peers and mobile peers where the service supports them. Run the same common contract tests on both platforms and test Janus-disabled packaging.

Record device model, OS, engine and libwebrtc revisions, codecs, build profile, network route, latency, CPU, memory and thermal/battery observations. A simulator run or desktop test cannot close a mobile acceptance gate. Missing devices or runners remain **Unverified**. Screen capture, advanced codecs, E2EE and effects each require their own capability evidence; unavailable paths remain visible in the [demo matrix](DEMO_MATRIX.md).

## App size validation

[SR-006 and SR-007](../ROADMAP.md#e02-native-feasibility-and-test-fixtures) record initial build/package sizes. [SR-807](../ROADMAP.md#e18-build-validation-and-diagnosability) establishes repeatable optimized results and numeric regression budgets before release.

1. Build a minimal standard UE Shipping app without SimurghRTC and a matching app with RTC enabled. Hold the engine/project revision, maps, assets, toolchain, architecture, signing/export method and compression settings constant. Use a minimal conditional harness so the baseline has no missing plugin class/asset references. Record unavoidable content differences separately.
2. Repeat for Full, Direct-only, Janus-only and Data-only client profiles. Keep optional models/effects and showcase assets out of the core comparison, then report their additional cost separately. Record both absolute sizes and `RTC delta = enabled app size - baseline app size` for each metric.
3. For Android, retain the AAB/developer artifact size, but estimate delivery using device-specific APKs and [`bundletool get-size total`](https://developer.android.com/tools/bundletool) with a saved device specification. Record a clean installed-app footprint separately, with the measurement method and app-data/cache policy. A universal APK or the AAB archive is not the per-device download result.
4. For iOS, archive/export the matching Shipping apps and retain the app-thinning report's compressed and uncompressed sizes for the same device variant. Label these as delivery/install estimates; record a clean installed-device measurement separately when available. The XCFramework ZIP and development archive are developer artifacts, not user download or installed-size measurements.
5. Inspect link maps and package contents for duplicate WebRTC/dependency copies, unintended architectures, symbols, codecs and optional assets. Attribute changes to native code, Java/resources or UE content rather than treating every byte as libwebrtc.
6. Record the complete build manifest, device/export specification, tool versions, commands, checksums and raw reports. Set per-platform/profile limits for added download and installed/uncompressed size after the baseline measurements. CI flags missing results or budget regressions; release qualification requires passing limits or a documented budget revision supported by functional and performance evidence.

| Report field | Required value |
|---|---|
| Build identity | UE/project/libwebrtc revisions, patches, compiler/SDK, build flags, codec set, profile and ARM64 target |
| Measurement identity | Device/OS or device specification, package/export method, metric, compression and app-data/cache policy |
| Absolute and incremental size | Baseline bytes, RTC-enabled bytes and delta bytes; distinguish MB from MiB |
| Separate developer outputs | Dependency archive, SDK package, native symbols/dSYMs and simulator artifacts |
| Release decision | Numeric budget, observed regression, pass/fail and any documented budget change |

No fixed MB claim is made before these measurements. Published vendor SDK figures are context only: they use different codecs, revisions, link settings and test applications. Library/archive size is also not a RAM, CPU or battery measurement. Repeat the applicable size and runtime checks whenever a dependency revision, codec set, toolchain or packaging configuration changes.
