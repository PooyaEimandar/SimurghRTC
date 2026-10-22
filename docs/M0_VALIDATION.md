# M0 status and test results

Updated **2026-10-22**. M0 is still in progress. The isolated **WebRTC M155** backend runs inside packaged Unreal 5.8.3 clients on Mac, Android and the iOS simulator, plus a Mac dedicated Server. Android/iOS ARM64 libraries are built separately from pinned sources and tools. Fresh repeat builds match the accepted libraries on Mac, Android and the iOS simulator. Source/advisory qualification remains open under SR-008.

The plugin has working local test probes, but the shared `Connect` API still returns `NotImplemented`. Real connections and network media are M1/M2 work.

## Current M155 results

| Check | Result | Scope |
|---|---|---|
| Native artifacts | Mac, Android, iOS device and iOS simulator ARM64 built | Separate crypto/codecs behind six private C entry points |
| Packaged Mac Direct-only | Lifecycle and media passed | Nine peers; local VP8/Opus, Metal capture and silent mixer playback |
| Packaged Android Direct-only and Full | Lifecycle and media passed on Samsung S25 Ultra | Nine peers per profile; generated video, two OpenGL captures and complete PCM consumption |
| Packaged iOS simulator Direct-only and Full | Lifecycle and local codecs passed | ARM64 iPhone 17 simulator, iOS 27.0; graphics and audio devices disabled |
| Native linkage | Passed for those five client packages and the Mac dedicated Server | One isolated WebRTC archive; Janus only in Full; Android ARM64-only with 16 KiB ELF load alignment |
| Fresh repeat builds | Passed on Mac, Android and iOS simulator | Two new outputs per target; identical libraries, headers and notice inputs; native consumers passed |
| Python checks | 144 passed | Artifact integrity, repeat comparison, fixtures, report correlation and failure rejection |
| Compiled Unreal build-rule checks | 71 passed | Metadata, payloads, ABI inputs and Android NDK consumer proof |
| Full Unreal automation | 11 passed, no test warnings or failures | Shared API/lifecycle and native factory/codec probes |
| Headless HTTPServer | Packaged Mac dedicated Server passed | Builtin-media and data-only factories, real HTTP responses, occupied-port failures and cleanup |

Each packaged media run encodes/decodes twelve generated VP8 frames and 96,000 PCM samples through Opus. Android and Mac additionally capture two render targets and consume 192,000 PCM bytes in Unreal's mixer, with callbacks off the game thread. Android capture error is zero. No camera, microphone, remote peer or network media is tested.

The iOS device library is built but has no signed-device acceptance. Physical iPhone testing remains deferred at the user's request. Windows/Linux replacement backends and Shipping RTC-on/off size measurements are also unverified. Package sizes below are Development measurements.

The earlier API v1, Blueprint examples, four-profile packaging, dedicated Server and Janus/TURN fixture results remain recorded in the historical sections below. Their original dependency identities are preserved.

## What remains in M0

| Issue | Remaining work |
|---|---|
| SR-008 | Finish source/advisory qualification; see [dependency progress](DEPENDENCY_SECURITY.md) |
| SR-006 / SR-007 | Reproduce replacement artifacts on independent hosts and repeat the iOS device library build. Physical iPhone validation is deferred |

SR-902 also blocks publication until complete dependency notices and patent files are delivered with artifacts and apps. Owners and exact findings are in the [dependency review](DEPENDENCY_REVIEW.md#follow-up-owners).

SR-005 is complete as a fixture task. Later SR-102/SR-301 work must run its message contracts against real Unreal providers; SR-305 covers provider parity. A Python model passing does not mean those providers are implemented.

## Reproduce the checks

- [Building](BUILDING.md): Python and Unreal automation.
- [Dependency review](DEPENDENCY_REVIEW.md): SR-003 findings and the repeatable inventory check.
- [Lifecycle examples](LIFECYCLE_EXAMPLES.md): API v1 C++ and Blueprint examples in Editor and packaged games.
- [Server probe](NATIVE_PROBE.md): packaged headless Server.
- [Media probe](MEDIA_PROBE.md): codecs, textures and audio integration.
- [Fixtures](FIXTURES.md): fixed input files and local Janus/TURN.
- [Mobile artifacts](MOBILE_ARTIFACTS.md): dependency packaging and build-rule checks.
- [Mobile builds](MOBILE_BUILDS.md): pinned sources/toolchains, separate ARM64 libraries and repeat-build checks.
- [Mobile probe](MOBILE_PROBE.md): package and run on Android or the iOS simulator.

Generated logs and reports stay under `Intermediate/`. The compact [SR-003 inventory snapshot](reviews/SR003-inventory.json) is retained with the review; its rerun is `Intermediate/SR003/inventory.json`. Start with `Intermediate/SR002/result.json` for profile/package acceptance, `Intermediate/SR001/result.json` for the earlier API v1 acceptance, `Intermediate/MobileProbes/current-results.json` for packaged mobile runs and the [M155 repeat snapshot](reviews/SR008-M155-repeat-validation.json) for the latest dependency repeat checks.

The earlier Xcode 27.1 simulator packages use source engine revision `82051d771c6227c2d5d10737c46d5e5d5a837c0b`. `Intermediate/MobileProbes/Xcode271/build-commands.json` records commands, tool versions and engine state before and after each step. Concurrent work changed HTML5-only files in the shared engine checkout; those paths are retained in the evidence. This task made no engine source edits.

## SR-008: packaged M155 dedicated Server

**Passed on 2026-10-22:** a packaged UE 5.8.3 Mac ARM64 Server creates an M155 factory and idle PeerConnection without rendering or hardware audio. Both factory modes pass HTTP readiness and shutdown; both also pass the occupied-port cleanup test.

| Scenario | Result |
|---|---|
| Builtin-media factory | HTTP 200 from the launched PID/run ID; Opus and video codecs advertised; clean shutdown |
| Data-only factory | HTTP 200; no media factories, dummy audio device or audio processing; clean shutdown |
| Occupied port, each mode | Explicit bind failure, no readiness announcement, native cleanup |
| Package/link audit | ARM64 only; one isolated WebRTC archive; no Editor or Janus dependency; valid code signature |

The harness now requires the selected artifact hash in HTTP health and the native startup log. The latter also verifies identity when the port cannot bind. Each result records the executable hash. Unknown routes and the wrong HTTP method are rejected, and positive runs leave the listener closed after exit.

The source engine is `dc88db5c3775843bdebcebfa473ceb22c4d5109f`; its working tree stayed clean. The selected library comes from the accepted Mac repeat build. The Development executable is 283,378,224 bytes; this is not a Shipping size or an RTC size delta. All **144 Python tests** pass.

Compilation/linking succeeded; the existing recursive Xcode finalization failure required Unreal's standalone finalization and receipt-generation steps. Cooking passed with zero errors and four known ACL-settings warnings. The minimal host also logs an unused gameplay NetDriver startup error; these checks cover the separate RTC factory and HTTP listener. Dawn's deployment-version warnings mean older macOS compatibility remains unverified.

See the [Server instructions](NATIVE_PROBE.md) and [retained snapshot](reviews/SR008-M155-server-validation.json). Commands, compile/cook logs, link inputs and four runtime reports are under `Intermediate/SR008Server20261022/`. This closes the replacement dedicated Server check under SR-008; source/advisory qualification remains open. No remote connection or network media is tested.

## SR-008: packaged M155 integration

The [native backend guide](NATIVE_BACKEND.md) describes the current build and staging workflow. The [integration snapshot](reviews/SR008-M155-validation.json) retains artifact, package and evidence hashes. Detailed logs are under `Intermediate/SR008Integration/`.

| Evidence | Location |
|---|---|
| Mac package | `MacLifecycle/`, `MacMediaAccepted/`, `mac-link-audit.json` |
| Android final packages | `AndroidDirectLifecycleFinal/`, `AndroidFullLifecycleFinal/`, `AndroidDirectMediaComplete/`, `AndroidFullMediaComplete/` |
| Android package/link audits | `android-direct-link-audit.json`, `android-full-link-audit.json` |
| iOS simulator packages | `SimulatorDirect/`, `SimulatorFull/`, both `simulator-*-link-audit.json` files |
| Rules, Python and Unreal tests | `RulesWithNDK/result.json`, `python-tests-final4.log`, `UnrealTests/index.json` |
| Headless Editor HTTPServer | `EditorHTTP/result.json` |

Android required two integration fixes: its compiler-runtime helpers must be included inside the isolated backend so Unreal's older NDK can link it, and OpenGL capture needs a vertical UV correction. The harness now keeps Android logging enabled and reconstructs bounded media-report chunks, rejecting incomplete, duplicate or mismatched reports. Earlier failures remain in the evidence directory.

Both final Android packages pass lifecycle and media checks against the same APK hashes. Each audit ties the packaged ELF build ID to the linked binary. The Samsung runs with 4 KiB system pages; 16 KiB ELF alignment is checked statically, not on a 16 KiB device. Simulator link maps resolve HTTP/2 to the pinned nghttp2 1.70.0 support library.

| Development package | Direct-only | Full |
|---|---:|---:|
| Android APK | 105,609,617 bytes | 105,611,024 bytes |
| Android `libUnreal.so` (uncompressed) | 177,164,464 bytes | 177,174,192 bytes |
| iOS simulator bundle files | 284,459,367 bytes | 284,465,191 bytes |
| iOS simulator executable | 252,280,272 bytes | 252,285,712 bytes |

These are complete sample-app sizes, not the incremental cost of RTC or Shipping download sizes. The selected archives are 17,820,448 bytes (Mac), 28,104,458 (Android), 17,259,440 (iOS device) and 17,331,008 (simulator).

These runs use source engine `3c5c6b098ec411254cb662437676de5548d929be`, Xcode 27.1 and Android NDK r27c. The engine checkout was clean; no engine sources were edited. Apple package runs precede the Android-specific capture/report fixes. The final six-entry-point backend now passes the separate fresh repeat checks below; earlier two-entry-point candidate results remain historical.

## SR-008: final M155 repeat builds

**Passed on 2026-10-19:** two fresh builds per target reproduce the accepted six-entry-point backend. The shipped library, C header, GN flags, patches, source lock and notice inputs match byte for byte. Each build starts in a separate empty Ninja directory and reuses the verified source checkout and pinned tools.

| Target | Archive bytes | Comparison | Native consumer runs |
|---|---:|---|---|
| Mac ARM64 | 17,820,448 | Both match the accepted library | Both passed |
| Android ARM64 | 28,104,458 | Both match the accepted library | Both passed on Samsung S25 Ultra, Android 16 |
| iOS simulator ARM64 | 17,331,008 | Both match the accepted library | Both passed on iPhone 17 simulator, iOS 27.0 |

Every consumer checks six peer lifecycles, overlapping data/media owners, invalid ABI inputs and two local VP8/Opus runs. Export checks find exactly six C symbols. Android consumers link with NDK r27c, have 16 KiB ELF load alignment, and are removed from the phone after execution. The Python suite passes **142 tests**.

Build records retain output paths and intermediate hashes, so artifact manifest hashes can differ. The comparison does not rewrite library bytes. A macOS tool-reinstallation failure was fixed by installing executables through fresh files; both accepted Mac runs use that fix.

This is a same-host repeat check, not independent-host reproduction. These runs use standalone native consumers; the earlier Unreal packages were not rebuilt or rerun here. Their selected library and header bytes match these exports. Physical iOS acceptance and an iOS device artifact repeat build remain open.

See the [retained snapshot](reviews/SR008-M155-repeat-validation.json) and [repeat instructions](NATIVE_BACKEND.md#check-repeat-builds). Full commands, comparisons, compiler logs and runtime reports are under `Intermediate/SR008Repeat20261019/`.

## Earlier SR-008 dependency update and isolation checks

**Historical intermediate results, 2026-10-19.** The simulator support library is now nghttp2 1.70.0. Its local regression passes against Unreal's existing 1.64.0 headers. Two fresh simulator builds produce identical archive, license and source-lock bytes; build metadata retains its temporary paths and therefore differs. The Python suite passes 123 tests, and the actual UBT rules DLL passes 36 cases, including retired-export rejection.

Both Direct-only and Full simulator packages pass with the new support library. Each run creates nine factories and peers, performs six explicit and three destructor teardowns, and verifies app shutdown. Each link map contains one WebRTC archive, one VPX archive and the selected nghttp2 archive; Janus is present only in Full. Both use Xcode/SDK 27.1 on the ARM64 iPhone 17 simulator running iOS 27.0. These packages retain the earlier WebRTC artifact; this is acceptance of the HTTP/2 update, not of the M155 replacement.

The separate M155 Mac ARM64 candidate builds WebRTC, BoringSSL and codecs from pinned sources. Only its two C entry points remain externally visible. Linking beside Unreal's crypto/codec archives and running six data/media-factory PeerConnection lifecycles succeeds. It does not exchange media or create a network session, and at that stage it had not replaced the plugin backend.

The candidate was built in two Ninja output directories on the same host and verified source checkout. Both the native archive and the isolated object match byte for byte after normalizing linker debug paths. Each output passes six local peer lifecycles. Reports are `M155Mac-accepted-a/result.json`, `M155Mac-accepted-b/result.json` and `native-repeat.json` under the SR-008 log directory; acceptance here covers only the standalone Mac test.

The [validation snapshot](reviews/SR008-validation.json) records exact inputs, hashes and results. The [updated inventory](reviews/SR008-inventory.json) verifies 19 engine archives against Epic's distribution manifest. That check identifies the distributed files; it does not recover their upstream patch history. The original SR-003 snapshot remains unchanged. See [dependency progress](DEPENDENCY_SECURITY.md) for reproduction commands and the remaining integration work.

Logs are under `Intermediate/SR008/`: `commands.json`, `Direct/result.json`, `Full/result.json`, both profile link audits, `Rules/result.json`, `python-tests-final.log` and the candidate build reports. The engine was at `c95c0654d8a28aff46b97644e27a58a81cc7879f` when this work started. Concurrent engine changes are recorded in the snapshot; this task made no engine source changes. Development package sizes are recorded as file bytes, not Shipping download sizes or RTC size deltas.

## SR-003: native dependency and ABI review

**Complete with release blockers, 2026-10-19.** The [review](DEPENDENCY_REVIEW.md) covers desktop/mobile dependencies, codec/APM scope, private interfaces, debug symbols, license inputs, security advisories and ABI constraints. The inventory command validated the corrected Android artifact, earlier iOS device artifact and Xcode 27.1 simulator artifact, including their external engine libraries. The full Python suite passed **120 tests**.

The main findings are unsupported public-upstream maintenance for OpenSSL 1.1.1t, missing Epic WebRTC/codec provenance, advisories requiring source-level disposition, a simulator nghttp2 version in an affected range, and incomplete distribution notices. SR-008 owns dependency qualification; SR-902 owns the final notice bundle. Existing Server/Samsung/simulator feasibility evidence is retained. No Unreal binaries, dependency pins or engine sources changed, and no new device/runtime acceptance is claimed.

The older device artifact correctly reports a different build lock and 12 patches. Android/simulator repeat artifacts match the current lock with 13 patches. Embedded Opus strings are `1.1` on Mac, `1.1-beta` on device/simulator and `unknown-fixed` on Android; the inventory does not infer a release version from the folder name.

## SR-002: build profiles and optional content

**Passed on 2026-10-19:** Full, Direct-only, Janus-only and data-only compile, cook and run as Mac ARM64 packages on UE 5.8.3. The same shared C++ and Blueprint examples run in each profile.

| Profile | Unreal tests | Providers tested in Editor and packaged Game | Optional Janus Blueprint |
|---|---:|---|---|
| Full | 11 | DirectUnreal and Janus | Executed |
| Direct-only | 11 | DirectUnreal | Excluded |
| Janus-only | 11 | Janus | Executed |
| data-only | 10 | DirectUnreal and Janus | Excluded |

All **43 Unreal tests** and **12 runtime runs** passed. Each runtime report checks the selected modules, reflected media/extension classes, optional asset, lifecycle outcomes and clean process exit. The Janus graph uses its actual async node and options struct, then verifies one expected invalid-input failure. Network transport remains unimplemented.

Staging excludes Janus sample content when its extension module is absent, including the Server profile. Capabilities now use an explicit extension-module flag. The data-only build retains both providers while excluding client media and Janus room extensions; it still links the engine's media-capable native archive. This is not a codec-pruned or Shipping size result.

Package audits match each executable to its runtime hash and native UUID, check exact plugin modules in the link map and reflected modules in UHT inputs, and require one libwebrtc archive. The Editor generator is absent from every Game. **117 Python tests** passed; the new Janus graph compiled with zero Blueprint warnings/errors. Ordinary Editor/cooker startup still reports the known missing optional ACL assets.

Reproduce these checks using [Lifecycle examples](LIFECYCLE_EXAMPLES.md). Evidence is in `Intermediate/SR002/result.json`, `commands.json`, per-profile automation/runtime reports, package audits and saved link maps/UHT manifests. These runs used Xcode 27.1 and Mac SDK 27.0. Concurrent work advanced the shared engine from `365b52acc1836aebbad937d565f2f8f2ed9240f1` to `c95c0654d8a28aff46b97644e27a58a81cc7879f`, including HTML5 work and a CoreAudio hardware sample-rate fix. Commands retain the engine revision and working-tree state before and after each step; `engine-concurrent-changes.patch` preserves that diff. The probes use `-nosound` and do not exercise hardware-audio startup. This task made no engine source edits. Earlier Server and mobile results remain separate; no new physical-device or mobile Blueprint acceptance is claimed.

## SR-001: API v1 and lifecycle examples

**Passed on 2026-10-19:** the shared contract is frozen as version 1 for UE 5.8.3. The review defines provider topology, session/source ownership, cancellation, structured errors, device discovery and headless PCM/BGRA frame boundaries. Shared source controls now live in Core. Public types expose no libwebrtc or Janus implementation details; [API v1](API_CONTRACT.md) includes migration and compatibility rules.

The executable examples use `AContractLifecycleCpp` and the actual `BP_RtcLifecycle` graph. The graph compiles with zero Blueprint warnings/errors, prints the shared error message, and calls Shutdown from EndPlay. Its parent records outcomes; it does not run the Blueprint's operation chain.

| Profile | Editor game | Packaged Mac ARM64 game |
|---|---|---|
| Full | DirectUnreal and Janus passed | DirectUnreal and Janus passed |
| Direct-only | DirectUnreal passed | DirectUnreal passed |

Each of these six runs executes both examples, checks operation/session IDs and ordered outcomes, verifies silent shutdown and actor destruction, waits for late callbacks, and requires process exit. Disconnect supplies local success; Connect deliberately reports `NotImplemented`. No network session or call is claimed.

Both profiles passed all **11 Unreal automation tests**, including pending null-session failures surviving garbage collection, disconnect/reuse, generated enum sentinel rejection, PCM/BGRA validation and honest device/source placeholders. **112 Python tests** passed. The same Blueprint asset was cooked into both packages; link maps include Janus only in Full and exclude the Editor-only generator from both games.

Evidence is under `Intermediate/SR001/`: `result.json`, `commands.json`, `FullFinalAutomation/`, `DirectFinalAutomation/` and the six `FullFinal*` / `DirectFinal*` runtime report directories. Reports retain run IDs, PIDs, executable/source hashes, native/package UUIDs and link maps. Earlier development attempts are preserved separately from the final accepted runs.

The accepted runs used source engine `365b52acc1836aebbad937d565f2f8f2ed9240f1`, Xcode 27.1 and the engine's desktop WebRTC archive. Concurrent HTML5 work changed the shared engine checkout and rebuilt its binaries; command records retain those observations. This task made no engine source edits. Ordinary Editor startup also reports missing optional ACL compression assets; the Blueprint compile and automation test results have no warnings. These Mac results do not add mobile Blueprint acceptance, live transport, hardware capture or Shipping-size evidence.

## Mobile source-build checks

All three targets build from pinned WebRTC 5414 sources and checked toolchains. Device and simulator libraries are separate. See [Mobile builds](MOBILE_BUILDS.md) for commands and the expanded record below for hashes and earlier repeat builds.

| Earlier artifact under `LocalBuilds/Artifacts/` | Stripped WebRTC archive | Status |
|---|---:|---|
| `Android-arm64-itanium` | 42,103,278 bytes | Standard C++ ABI; packaged Samsung probes pass |
| `IOS-arm64-release` | 25,297,696 bytes | Device build/link and repeat comparison pass; physical run deferred |
| `IOSSimulator-arm64-current` | 25,273,184 bytes | Packaged Direct-only and Full simulator lifecycle probes pass |

These archive sizes are not app sizes. Android runtime evidence still uses the earlier corrected artifact. The latest simulator runs use the Xcode 27.1 repeat-build artifact described below; earlier simulator reports are preserved separately.

## Mobile repeat-build validation

**Passed on 2026-10-18:** two fresh builds each of corrected Android ARM64 and iOS simulator ARM64. All four exports passed archive validation and whole-archive linking.

| Target | Compared files | Stripped libraries | Result |
|---|---:|---|---|
| Android | 2,939 | WebRTC: 42,103,278 bytes | Identical bytes and build inputs; standard C++ ABI and 16 KiB link-probe alignment checked |
| iOS simulator | 2,941 | WebRTC: 25,273,184 bytes; VPX: 1,784,592 bytes | Identical bytes and build inputs; ARM64 simulator objects checked |

Each run starts with an empty Ninja output directory and reuses its platform's verified source checkout. The comparison checks exported libraries, headers, notices, patches, source/build locks, GN flags, tool versions and whole-archive link inputs. This is same-host repeatability; independent-host reproduction remains open.

Exports are under `LocalBuilds/Artifacts/`, named `Android-repeat-20261018-a` / `-b` and `IOSSimulator-repeat-20261018-a` / `-b`. Their matching `.symbols` directories retain diagnostic WebRTC archives. Android keeps NDK r27c, Clang 18.0.3 and API 26. Its library, headers and notices also match the earlier corrected Samsung-tested artifact; only build provenance changed.

Simulator builds now pin the installed Xcode 27.1 (`27A9275`) and SDK 27.1 (`24A94232`), with Apple Clang 21.0.0. Headers and notices match the earlier simulator export. All 1,246 WebRTC and 160 VPX objects differ from the originals only in their recorded SDK version (27.0 to 27.1); `simulator-sdk-delta.json` records that audit. The `IOSSimulator-repeat-20261018-a` export also passed the packaged Unreal checks below in both profiles. The `-b` export supplies repeat-build comparison evidence. Earlier runtime reports and the iOS device build's Xcode/SDK 27.0 pins are preserved.

Evidence is under `Intermediate/MobileBuilds/Repeat20261018/`: `result.json`, `Android-comparison.json`, `IOSSimulator-comparison.json`, per-build command records/logs and `python-tests.log` (108 passed). The new checks reject reused output directories, changed tools/flags and missing fresh-build evidence. Full compiler logs remain in each workspace's `src/out/<build-name>/` directory.

<details>
<summary>Repeat-build library checksums</summary>

| Library | SHA-256 |
|---|---|
| Android `libwebrtc.a` | `950bae6ef80ee71d0509d235f7eb0e5dfa1aa03d964cb8f3004df572e781b5d3` |
| Simulator `libwebrtc.a` | `53216b093aeaf0127c874ac16f31ca8846a3eb0dd0fdab47057ca1e1b1e4f354` |
| Simulator `libvpx.a` | `919f7d108a8b78fca36475f3d9077661d8060b00ef1f147a1685dd52ec6c2c20` |

Build lock SHA-256: `5089c53c0ea5463f6e2e8320f40a7b5a7b010d894c57981eb410a76c642adbf1`.
Build runner SHA-256: `63b261a6ebb829c7a285e370f39d57ce42dcd92c12c874a32f3a82bf79454fd5`.
The comparison reports record both manifest hashes. Manifests can differ because they also record output paths and diagnostic link results.

</details>

## Packaged mobile probe work

The [mobile probe guide](MOBILE_PROBE.md) stages Direct-only and Full projects with checked ARM64 artifacts. The opt-in native probe exercises three lifecycle cycles: nine factories and peers, explicit stop, overlapping owners, restart, duplicate-call rejection and destructor cleanup. Every report identifies the dependency manifest compiled into the app.

The Mac Editor dry run passed all lifecycle checks. Its evidence is `Intermediate/MobileProbes/DesktopClient/result.json`; it uses the engine's desktop WebRTC and is not mobile acceptance. That run's 104 Python tests include rejection of stale reports, mismatched dependencies/profiles, incomplete cleanup, simulator evidence and APKs containing another architecture.

The sample Editor target has explicit `-SimurghRTCAndroidCook` and `-SimurghRTCIOSCook` options to build the required mobile cooking modules. Android and iOS simulator cooking and packaging passed. Build each project's Editor before cooking it, and avoid overlapping UBT builds; the [guide](MOBILE_PROBE.md) explains the shared-build constraints.

Both Android profiles passed on Samsung SM-S938B (Galaxy S25 Ultra), Android 16, ARM64, with 4 KiB system pages. Each run completed three cycles, nine factories/peers, six explicit stops and three destructor cleanups; the harness then verified the app had stopped. The Full result has `with_janus: true`; this checks module inclusion, not a Janus network connection. Both final link maps select one staged WebRTC archive and preserve 16 KiB ELF load alignment.

| Android Development package | APK bytes | Packaged `libUnreal.so` bytes | Evidence directory under `Intermediate/MobileProbes/` |
|---|---:|---:|---|
| Direct-only | 103,183,903 | 171,897,584 | `AndroidDirectItanium/` |
| Full | 103,188,083 | 171,903,088 | `AndroidFullItanium/` |

These are ARM64-only, cooked Development APKs (about 98.4 MiB each), not Shipping delivery or RTC-only size deltas. Each evidence directory contains `result.json`, the correlated device log and command log. Build/package logs and `android-*-itanium-link-audit.json` are alongside them.

The corrected Android archive is 42,103,278 bytes, SHA-256 `950bae6ef80ee71d0509d235f7eb0e5dfa1aa03d964cb8f3004df572e781b5d3`. Both apps report artifact manifest `f35b32bafa9a0c14bd1ab9a4121d09de9fbff2483fbd093270db8b55cde026cc`. The later repeat builds reproduce these library bytes; independent-host reproduction remains open.

The Direct-only iOS native executable compiled and linked successfully after scoping a Clang warning suppression to the pinned Abseil headers. Its link response/map select one iOS WebRTC archive, with iOS 15.0 minimum and SDK 27.0. Evidence is `ios-direct-build-fixed.log` and `ios-direct-link-audit.json`. Physical iPhone testing is deferred at the user’s request. Current iOS work targets the installed simulator; no older Xcode setup is planned. An unsigned device executable is not a simulator or device runtime pass.

On 2026-10-18, both simulator profiles passed using `LocalBuilds/Artifacts/IOSSimulator-repeat-20261018-a`. The dependencies and packaged UE 5.8.3 apps use Xcode 27.1 (`27A9275`) and SDK 27.1 (`24A94232`). They ran on the installed iPhone 17 simulator, iOS 27.0 (`24A434`), ARM64.

Each run completed three cycles: nine factories/peers, six explicit stops and three destructor cleanups. Duplicate calls, overlapping owners and restart after stop passed. Reports match the launch PID, run UUID and artifact manifest `765657e8fa7cf55d4337654f5de39ba9f17fc8188c0e3c317d721408df1f85ff`. Both apps were stopped and their processes confirmed absent.

| iOS simulator Development package | Bundle file bytes | Executable bytes | Evidence directory under `Intermediate/MobileProbes/` |
|---|---:|---:|---|
| Direct-only | 283,675,788 | 251,680,976 | `Xcode271/Direct/` |
| Full | 283,698,028 | 251,702,816 | `Xcode271/Full/` |

These are simulator bundle measurements, not iPhone delivery sizes or Shipping RTC deltas. Each evidence directory contains the native report, launch/command logs and `result.json`. The summary is `Intermediate/MobileProbes/Xcode271/result.json`; that directory also holds build/package logs, linker maps/responses and per-profile link audits. The packaged and native executable UUIDs match, tying each app to its map. Both maps select exactly one staged WebRTC, VPX and HTTP/2 archive, with Janus absent in Direct-only and present in Full. All 108 Python checks passed in this run.

These earlier simulator runs used separately built libwebrtc and portable-C VPX, plus pinned `nghttp2` 1.64.0 because Unreal’s bundled HTTP/2 archive is device-only. That HTTP/2 support export is retired by SR-008. It was built with Xcode/SDK 27.1 in `LocalBuilds/SimulatorSupport/nghttp2-arm64-xcode271`; its manifest and archive hashes remain in the historical link audits. The sample enables Unreal's scene lifecycle and supplies project-local metadata for its headless `-nullrhi -sm5` startup. The earlier 30 compiled UBT validator cases passed (`SimulatorRules/result.json`). Tests reject device/simulator object mismatches and prevent simulator reports from counting as physical-device results. No engine source edits were needed.

Network media, provider interoperability, suspend/resume and Shipping RTC-off/on size comparisons remain separate work.

Earlier simulator runs used Xcode/SDK 27.0 dependencies with 27.1 packaging. Their reports remain in `IOSSimulatorDirectValidated/` and `IOSSimulatorFullValidated/`, under `Intermediate/MobileProbes/`, with the original manifest `a0dd1843b77eb2288dec27c96d3ff438f2913bfd9dc094cdc551313b82fb746d`. Their engine revision was `2cafb2d9194e503358a1fbeced79e621f2620abc`, with clean tracked files when recorded in `engine-provenance.json`. The old aggregate is preserved as `Xcode271/previous-mobile-results.json`.

## Detailed validation record

<details>
<summary>Expand engine versions, measurements, earlier runs and troubleshooting</summary>

The earlier mobile-artifact integration check rebuilt the Full Mac Editor and reran all seven Unreal tests successfully, with no test warnings or failures. The 19 validator cases executed the class in the actual UBT-generated rules DLL, using explicitly synthetic dependency inputs. Those earlier checks did not include a packaged Unreal runtime result.

Small C archives compiled with NDK `27.2.12479018` and Xcode were also inspected: Android AArch64 and iOS device ARM64 objects passed; an ARM64 iOS simulator object was rejected. These are archive-parser tests, **not libwebrtc builds**. The installed engine lacks its optional iOS build components. A source-engine iOS check was cancelled while waiting behind another active build; its platform build path was unverified at that point.

Earlier artifact-format evidence is under `Intermediate/MobileArtifacts/`: `python-tests.log`, `unreal-build.log`, `Rules/result.json` and `ObjectProbes/result.json`. The Unreal report is `Intermediate/Reports/MobileArtifactDesktop/index.json`. Source-build evidence is recorded separately below. Packaged Unreal linkage, device behavior and app size were still open at that point; current results are above.

Generated logs and reports are local files under `Intermediate/` and are not committed. For the earlier SR-005 run, start with:

| Local file | Contents |
|---|---|
| `Intermediate/SR005/python-tests.log` | 47 Python test results |
| `Intermediate/Reports/SR005Native/index.json` | 7 Unreal test results |
| `Intermediate/Reports/SR005ServicesIPv4/result.json` | First complete service pass |
| `Intermediate/Reports/SR005ServicesRestart/result.json` | Fresh-restart service pass |

Check structured results and cleanup evidence as well as exit codes. Known build workarounds, earlier failures, exact versions, hashes and log paths are preserved below.

## Earlier mobile source-build checks

The [build workflow](MOBILE_BUILDS.md) pins WebRTC `4e8a5ac68e8a4ae0588f54f2fdb8cbd1eb5fa50d`, its source dependencies, GN/Ninja downloads, platform toolchains and matching Unreal dependency inputs. The original 12 patches were checked against the pinned files. A thirteenth patch fixes the Android C++ ABI mismatch found by the physical-device probe below. No engine source was changed.

| Target | Build and link result | Stripped archive | Separate diagnostic archive |
|---|---|---|---|
| Android ARM64 | 1,250 objects; whole-archive link and repeat comparison passed; all four probe load segments aligned to 16 KiB | 42,533,454 bytes (40.6 MiB) | 118,581,830 bytes (113.1 MiB) |
| iOS device ARM64 | 1,246 objects; whole-archive link passed; two output directories matched byte for byte | 25,297,696 bytes (24.1 MiB) | 60,121,120 bytes (57.3 MiB) |

Android used the pinned Linux x86-64 container under Docker on Apple Silicon, NDK r27c (`27.2.12479018`), Clang 18.0.3 and API 26. Its archive SHA-256 is `3bbd8758351921218180be185388b623037884bb28e408d42539c91e7636ae66`. The link probe uses Unreal's static C++ runtime and local symbol-binding flags; these resolved the initial VPX shared-library relocation errors.

The iOS run used Xcode 27.0 (`27A266a`), iPhoneOS SDK 27.0 (`24A430`), Apple Clang 21.0.0 and deployment target 15.0. Its archive SHA-256 is `6865dd8162410a4a680f538fb1ebc9767c4f5821a6ce980678af43235cb245ad`. Neither WebRTC archive defines the checked Opus, VPX or OpenSSL symbol prefixes; those implementations remain external engine dependencies.

The original artifacts are under `LocalBuilds/Artifacts/`: `Android-arm64-release`, `IOS-arm64-release` and their `-repeat` counterparts. The Android exports are superseded: they used incompatible relative vtables and are now rejected by staging/build rules. Both platforms match across 2,938 compared files. Each artifact contains its build lock, toolchain observations, source lock, notices and `Build/link-probe.json`.

Both exports also compiled and linked without source-checkout include paths. They were staged together into the Direct-only project `LocalBuilds/MobileBuiltDirect`, and the compiled Unreal artifact validator accepted both. This exercises artifact selection and integrity, not an Unreal app build.

Evidence is under `Intermediate/MobileBuilds/`: `android-repeat.json`, `ios-repeat.json`, `rules-result.json`, `android-consumer.log`, `ios-consumer.log`, `python-tests.log` and `patch-validation.json`. `result.json` collects the final artifact identities and checks. Full compiler logs remain in each workspace's `src/out` directory: the Android Docker volume `simurghrtc-mobile-android-5414`, and `LocalBuilds/MobileSources/Checkout` for iOS.

These are dependency build/link checks on one host, with separate Ninja output directories and a shared verified source checkout. Fresh-host reproduction remains open. The dependency link probe itself is not executed on a device; Unreal app checks are recorded below. Static archive bytes are not app download or installed size. SR-006/SR-007 stay in progress.

Android's Direct-only native build and initial APK assembly passed after enabling Unreal's `AndroidPermission` helper in the minimal host. The linker response and map resolve exactly one libwebrtc archive, from the staged mobile artifact. No Janus module is linked, and every final ELF load segment is aligned to at least 16 KiB. Evidence is `Intermediate/MobileProbes/android-direct-build.log` and `android-direct-link-audit.json`.

The first packaged Samsung run crashed in `FNativeRuntime::Start`, when Unreal called `rtc::Thread::BlockingCall`. Chromium had built Android with 32-bit relative vtable entries; Unreal expects standard 64-bit entries. Patch `0013-android-itanium-abi.patch` disables the incompatible layout. The build now checks the pinned thread vtable before export, and Python/UBT validators reject Android artifacts without the matching ABI record. The corrected dependency built successfully, and both packaged Samsung profiles now pass. The failed-run evidence is `Intermediate/MobileProbes/AndroidDirectValidated/result.json` and `process.log`.

The connected Samsung is an SM-S938B. The connected iPhone X is an iPhone10,6 on iOS 16.7.16; USB trust, Developer Mode and an Apple Development signing identity are now available. Xcode 27 reports this device as unsupported, and no legacy iOS 16 developer disk image is installed. A successful compile or Editor dry run does not close the physical-device gate. Network media, provider interoperability, suspend/resume and Shipping RTC-off/on size comparisons remain separate work.

## Local environment

| Item | Observed value |
|---|---|
| Installed engine | UE **5.8.3**, changelist **58210709**, branch `++UE5+Release-5.8` |
| Source engine | UE **5.8.3**, changelist **0**, branch `UE5`; Git `1a6598b42ab311f9d9766e10404e54c82ce1a0a1` |
| Engine compatibility version | **5.8.0**, compatible changelist **55116800**; used by the `.uplugin` descriptor |
| Build targets | `ContractHost`, `ContractHostEditor` and `ContractHostServer`, Mac, Development, ARM64 |
| Host | Apple M2 Max, macOS 27.0.1 (26A434), as reported by the editor |
| Toolchain | Xcode 27.0 (27A266a), Mac SDK 27.0; UBT reports Clang 21.1.6, compiler banner Apple Clang 21.0.0 (clang-2100.3.34.2) |
| Minimum macOS target | 14.0, as selected by the installed UE toolchain |
| Native RTC library linked by this plugin | Engine WebRTC **5414 / M109**, Release; explicit OpenSSL/Opus/VPX links |

The [SR-003 inventory](reviews/SR003-inventory.json) retains the earlier archive hashes, local dependency versions and codec observations. The Mac archives in the source and installed engines are identical. `SIMURGHRTC_WITH_NATIVE_TRANSPORT=0` still describes the unavailable shared session service, not factory linkage. Android/iOS engine WebRTC branches are deliberately not selected; their separate dependency builds are recorded above, while device acceptance remains SR-006/SR-007 work. No engine source changes were introduced.

## Earlier evidence

| Check | Result | Scope |
|---|---|---|
| Python offline tests | **Passed: 47 tests** | Prior 16 profile/probe checks plus 16 signaling cases, 10 media/service fixture checks and five TURN packet checks |
| Full UHT and Editor compile | **Passed with native linkage** | All seven modules compile/link on the local Mac ARM64 installation |
| Direct-only UHT and Editor compile | **Passed with native/media integration** | Direct-only Editor and Game compile/link with Janus source omitted |
| Full contract + native automation | **Passed: 7 tests, 0 warnings/failures in test results** | Five contract cases, factory/peer lifetimes and repeated generated VP8/Opus codec fixture under NullRHI/nosound |
| Direct-only runtime contract automation | **Passed: 5 tests, 0 warnings/failures** | Same tests with Janus reflected classes absent |
| Full / Direct-only minimal host cook | **Passed before native integration: 0 errors, 4 warnings each** | `/Engine/Maps/Entry`; no shared or Janus-only Blueprint sample assets exist yet |
| Full headless Editor HTTP probes | **Passed: media, data-only and occupied-port cases** | Real HTTP health and invalid-route/method requests, factory/peer readiness and cleanup; explicitly `compiled_server_target=false` |
| Source Server / cooking Editor C++ compile and link | **Passed; app finalization required a workaround** | Mac ARM64, Server profile; standalone UBT finalization and metadata generation succeeded, with no engine source edits |
| Source Server cook / staging | **Passed: 0 errors, 4 cook warnings** | Standard UAT BuildCookRun staged the ARM64 Server app with cooked Entry map and Pak/IoStore content |
| Staged dedicated Server HTTP probes | **Passed: media, data-only and occupied-port cases** | `compiled_server_target=true`, native factory/peer ready, Null RHI, no hardware audio, HTTP routing and native/listener cleanup |
| Janus-only / data-only profile engine builds | **Now passed; see SR-002 above** | Earlier staging-only evidence is superseded by current Editor and packaged Mac results |
| Full Editor local media probe | **Passed on Metal** | VP8/Opus, two texture uploads/captures and UE procedural mixer consumption |
| Direct-only Game cook / staging | **Passed: 0 errors, 4 cook warnings** | Installed UE 5.8.3 ARM64 Game; app finalization workaround below |
| Packaged Game local media probe | **Passed: positive, NullRHI and no-audio cases** | `editor_build=false`; codec/capture/mixer feasibility only, no RTC call or executable Blueprint demo acceptance |
| Canonical media v1 / Full Editor automation | **Passed: 7 tests, no test warnings/failures** | Generated adapter compiled; raw-input hashes verified; native VP8/Opus fixture exercised twice under NullRHI/nosound |
| Pinned Janus / coturn fixtures | **Passed on Docker Linux ARM64** | Janus authentication, VideoRoom control/correlation/cleanup; TURN auth negatives, allocation, permission, exact 29-byte relay round trip and release |
| Windows/Linux Unreal apps | **Unverified** | No runtime host evidence; current Android and iOS simulator results are recorded above |

Commands are in [Building](BUILDING.md), [Native Server probe](NATIVE_PROBE.md) and [Native media probe](MEDIA_PROBE.md). Local generated evidence lives under `Intermediate/` and is intentionally not source-controlled. Engine output and test reports must be checked for actual test counts/failures; a process exit code alone is insufficient.

The Full UHT manifest contains Core, Media and JanusExtensions reflected modules; the Direct-only manifest contains only Core and Media. That earlier contract-only Direct run used `Intermediate/Profiles/DirectValidated`; current media packaging uses `LocalBuilds/NativeMediaDirect`. Automation reports are `Intermediate/Reports/Full/index.json` and `Intermediate/Reports/Direct/index.json`; both report five successes and zero failed/not-run/in-progress tests. Build logs are `Intermediate/ContractHost-Build.log` and `Intermediate/DirectValidated-Build.log`.

Cook logs are `Intermediate/Full-Cook.log` and `Intermediate/Direct-Cook.log`. Both commandlets exited 0 and reported success with four warnings: the minimal host disables default engine plugins, while engine animation defaults still refer to the disabled ACL plugin's bone/curve compression settings assets. The empty host has no animation assets; animation samples must configure these dependencies before their packaging acceptance. Full used the default Zen output; Direct-only used `-SkipZenStore` for local filesystem output. The documented command uses local output to avoid the initial Zen-store discovery delay observed on this machine.

The first runtime attempt exposed the distinction between engine patch and plugin compatibility versions: setting descriptor `EngineVersion` to `5.8.3` made the unattended editor skip optional modules. It also caught an abstract UObject used as a test fixture. The descriptor now uses the engine's `5.8.0` compatibility version and the fixture uses a concrete GameInstance. Neither fix modifies engine files.

## Native integration evidence

`Intermediate/M0-Native-Editor-Build.log` records the final installed-engine native compile/link. `Intermediate/Reports/NativeFullFixed/index.json` records six successes with zero failures, warnings, skipped or incomplete tests. `Intermediate/M0-Native-Automation-Fixed.log` contains the runtime test log. A concurrent Editor startup caused an unrelated AutoSDK trace-log backup exception in console output; all six engine automation cases still completed successfully. The documented workflow runs Editor processes sequentially.

Editor fallback probe evidence is `Intermediate/Reports/NativeEditorProbe/result.json`, `NativeEditorDataProbe/result.json` and `NativeEditorOccupiedProbe/result.json`. All passed. The media run created dummy ADM/APM and advertised Opus plus VP8/VP9; data-only reported no media dependencies/codecs. Neither claims a connected RTC session.

The first integration runs caught and fixed three real boundaries: precompiled external modules needed explicit dependency links; the engine archive lacked the default audio-device factory symbol; and M109's capability query dereferenced a missing media engine in data-only mode. The final lifecycle test covers the corrected data-only branch. The initial installed Server build was rejected because that distribution lacks Server target support (`Intermediate/M0-Server-Build.log`). The supplied source engine is used for dedicated Server acceptance.

## Dedicated Server evidence

**Historical M109 baseline.** Current M155 Server acceptance is recorded above.

The tested isolated Server profile is `Intermediate/Profiles/NativeServerFinal`. Future builds should use `LocalBuilds/` as documented below. The actual staged executable is:

```text
Intermediate/Profiles/NativeServerFinal/Saved/StagedBuilds/MacServer/ContractHostServer.app/Contents/MacOS/ContractHostServer
```

`file` identifies an ARM64 Mach-O executable. The link response contains exactly one engine `libwebrtc.a` and no UnrealEditor inputs; `otool -L` also contains no UnrealEditor dependency. Audits are `Intermediate/M0-Server-Link-Audit.json` and `Intermediate/M0-Server-Artifact-Audit.json`. The development executable SHA-256 is `cf8cfc830b3aa27f62ef1d07d2107ace416725c8d418de53677923935f988acc`. Its size is not a Shipping app-size baseline or RTC size delta.

The following separate build stages were verified; a successful metadata-only invocation is not being counted as C++ compilation:

| Stage | Evidence / outcome |
|---|---|
| Native C++ compile/link | `M0-Source-Server-Final-Build.log`, `M0-Source-Server-NoUBA-Build.log`, `M0-Source-Server-Editor-Build.log`; compile/link succeeded, then recursive Xcode app finalization failed |
| Standalone Unreal app finalization | `M0-Server-Standalone-PostBuild.log` and `M0-Editor-Standalone-PostBuild.log`; `ApplePostBuildSync` succeeded for both apps |
| UBT target/module receipts | `M0-Server-Metadata.log` and `M0-Editor-Metadata.log`; `-ActionTypeFilter=WriteMetadata` succeeded after compilation/linking/finalization |
| Cook and stage | `M0-Source-Server-Package.log`; UAT exited 0, `BUILD SUCCESSFUL`; four ACL settings warnings, matching the empty-host limitation above |
| Media factory runtime | `Reports/NativeServerMediaValidated/result.json`; passed, dummy ADM/APM present, Opus and VP8/VP9 advertised, peer ready |
| Data-only factory runtime | `Reports/NativeServerDataValidated/result.json`; passed, no ADM/APM/media codecs, peer ready |
| Occupied-port runtime | `Reports/NativeServerOccupiedValidated/result.json`; passed, bind failure, no readiness, native cleanup |

All paths in that table are beneath `Intermediate/`. Both positive Server runs returned HTTP 200 JSON from the launched PID/run ID, reported UE `5.8.3-0+UE5` and `compiled_server_target=true`, rejected unknown routes and POST to the GET health route, exited 0, and left the probe port closed. Runtime logs explicitly report Null RHI and that the Audio Device Manager did not initialize. The idle peers never connected to a remote endpoint.

Observed toolchain/host limitations are retained:

- The source checkout's recursive Xcode pre-action failed through UBT even with `-NoUBA`. Standalone UBT `ApplePostBuildSync` succeeded. The reproduction guide includes this recovery and normal metadata generation; engine sources were not patched.
- This checkout's dependency cache finds the first `Intermediate` path component. Nesting a staged project beneath it caused repeated full rebuilds. A redundant retry was interrupted and the raw Server executable restored from its successfully finalized app before receipt generation. Future commands use ignored `LocalBuilds/` paths.
- The supplied engine's Dawn archive emitted macOS 27.0 versus target 14.0 linker warnings. This is validation on the current macOS 27.0.1 host, not proof of deployment compatibility with older macOS versions.
- The minimal host disables default engine plugins. It logs a missing `OnlineSubsystemUtils.IpNetDriver` and failure to open Unreal's gameplay listener. RTC factory/HTTP checks still passed; this host does not validate Unreal gameplay replication. Configure that sample dependency before multiplayer gameplay acceptance.
- Packaged Mac Server output goes to captured stdout here, without the requested separate ABSLOG file. The first harness run failed that file assumption despite receiving valid Server health; the corrected harness verifies `process.log` and the three final runs passed. Mac also ignores requested graceful exit statuses; the occupied-port test checks explicit failure/cleanup evidence rather than trusting exit 0.

## Native media evidence (2026-10-18)

The native fixture uses the same engine WebRTC archive audited above, with software **VP8/libvpx** and **Opus**. The Full headless automation report `Intermediate/Reports/MediaCodecs/index.json` records seven successes and zero failures, warnings, skipped or incomplete tests; log `Intermediate/M0-Media-Automation.log`. `SimurghRTC.Native.MediaCodecs` runs the fixed codec fixture twice and verifies fresh decoder/callback lifetimes without RHI or audio devices.

The complete GPU/audio probe passed in Full Editor (`Intermediate/Reports/MediaEditor/result.json`) and in the cooked/staged Direct-only Game (`Intermediate/Reports/MediaGameValidated/result.json`). The packaged report confirms installed engine `5.8.3-58210709+++UE5+Release-5.8`, `editor_build=false`, Metal and no network transport claim:

| Measurement | Packaged result |
|---|---|
| Generated / encoded / decoded video | 12 / 12 / 12 frames, 64×64, ordered 90 kHz timestamps |
| VP8 quadrant RGB error | Maximum 3 (limit 12) |
| Texture upload / render-target capture | Two distinct frames; maximum RGB error **0** (limit 3) |
| Application frame transfer | 16,384 bytes per BGRA frame; one transfer in flight |
| Encoded packets | Maximum 139 video bytes and 161 audio bytes in this fixture |
| Decoded audio | 50 Opus packets, 48 kHz stereo, 96,000 interleaved PCM16 samples |
| Tone validation | Correlation 0.999877; RMS 5559.67 for generated amplitude 8000 |
| UE procedural mixer | All **192,000 bytes** consumed in 95 callbacks, outside the game thread; silent output |

`Intermediate/Reports/MediaGameNullRHI/result.json` and `MediaGameNoAudio/result.json` both passed their negative expectations: explicit missing-rendering or missing-audio failure, probe reference cleanup, and process exit. No device requests or simulated capture pass replace these failures.

The final executable is `LocalBuilds/NativeMediaDirect/Saved/StagedBuilds/Mac/ContractHost.app/Contents/MacOS/ContractHost`. `Intermediate/M0-Media-Game-Artifact-Audit.json` records ARM64, no UnrealEditor dynamic dependency, exactly one libwebrtc archive in the link response, no Janus module in the link response/source, and exact correspondence between staged and repository plugin sources. Its SHA-256 is `47788e5dc031954ddb1b29fbb8ca1eda3e22a241898879ce864714aeb9045327`. Its Development executable size is 273,906,768 bytes; this is not a Shipping package baseline or the incremental cost of RTC.

Build evidence: `Intermediate/M0-Media-Editor-Report-Build.log`, `M0-Media-Direct-Editor-Report-Build.log`, and `M0-Media-Game-Report-Build.log`. The Game compile/link succeeded, then the known Xcode **Touch UBT generated tiles** pre-action failed. Standalone finalization (`M0-Media-Game-Report-PostBuild.log`) and normal receipt generation (`M0-Media-Game-Report-Metadata.log`) succeeded. Initial cook/stage (`M0-Media-Game-Package.log`) reported zero errors/four known ACL warnings; final binary restaging (`M0-Media-Game-Report-Stage.log`) succeeded using the same cooked content. The build also warns about an absent optional MetalShaderConverter include directory. No engine sources were edited.

The first packaged media attempt (`Intermediate/Reports/MediaGame/result.json`) failed because UE's packaged platform-file policy rejected the requested absolute report path. The final runner emits compact JSON to the runtime log and the harness validates/saves captured stdout; a regression test rejects absent, duplicate or malformed reports. Packaged positive and negative runs above use this fallback. UE also reports a CoreAudio sample-rate-query warning and hardware device `Unknown` on this host. Mixer callbacks and consumption were observed; physical speaker output, device quality and AEC were not tested. The earlier compile attempts exposed UE's atomic-counter API spelling and the explicit `AudioExtensions` link required by the procedural-wave subclass; both were fixed before acceptance.

[Native media probe](MEDIA_PROBE.md) documents the fixed allocation/queue bounds, game/codec/render/audio ownership and permission requirements. Capture uses public `UpdateTextureRegions`, `FCanvas` and `ReadPixels`, with no private rendering headers or Pixel Streaming implementation. The small synchronous readback and blocking codec helper are feasibility tools, not production real-time media paths.

## SR-005 fixture evidence (2026-10-18)

The authoritative service input pins are in `Fixtures/services/sources.lock.json`: Janus multistream **v1.4.2** at `0a24110ae55a172c4293749b763dbb66a138f9ec`, coturn **4.18.0** at `23c6c1d32a3d2b21a56cee65d3203bcc4ea82d2b`, archive SHA-256 values, a Debian multi-architecture image digest and the `20261016T000000Z` package snapshot. Docker CLI 29.7.2, Engine 29.7.2 and Compose v5.5.1 ran Linux ARM64 containers through Docker Desktop on this Mac.

| Evidence | Observed result |
|---|---|
| `Intermediate/SR005/python-tests.log` | 47 offline tests passed: prior profile/probe checks plus Direct/Janus contracts, media manifests, credential/configuration handling and STUN/TURN packet validation |
| `Intermediate/Fixtures/SR005MediaV1/manifest.json` | Materialized PCM/WAV/BGRA bytes match all committed v1 hashes; generated C++ adapter matches |
| `Intermediate/SR005/unreal-build.log` | Installed UE 5.8.3 Full Editor compiled/linked the v1 native fixture adapter successfully |
| `Intermediate/Reports/SR005Native/index.json` | Seven Unreal automation successes, zero failures/warnings/skips/in-progress; codec fixture runs twice under NullRHI/nosound |
| `Intermediate/Reports/SR005ServicesIPv4/result.json` | First complete pinned-service pass: Janus version/plugin/transport checks, auth/control/cleanup; coturn version/auth/relay/release |
| `Intermediate/Reports/SR005ServicesRestart/result.json` | Same checks passed after `down` removed the containers/network and `up` created a fresh stack |

Both service reports identify image `sha256:0ca00e7a237a5e0b5bda82b053475d69130924c25bdc3c0863dc95ccbd79280b`, the baked lock and each configuration/probe source hash. Each evidence directory also contains a secret-free process log and exact Debian package inventory. Running service image IDs were verified against the smoke image. The TURN test rejected missing, wrong and expired credentials, authenticated allocation/permission responses, relayed an exact **29-byte binary payload in each direction**, and confirmed lifetime-zero release. The Janus test verified unauthenticated rejection, configured room 9001, an ACK followed by a correlated VideoRoom joined event, trickle completion acknowledgment, detach/destroy and stale-session rejection.

Early checks failed visibly and were retained under `SR005ServicesInitial`, `SR005ServicesRuntimeFix` and `SR005ServicesConfigFix`. Fixes were limited to the fixture setup: executable mode on coturn's installed binary, the pinned release's `dtls=false`/`cli=false` options, bounded file descriptors and explicit per-container IPv4 binding. Janus rejected `ws_ip=0.0.0.0`; interface-only WebSocket binding stalled startup on this Docker host. IPv4 binding allowed both transports to initialize. The package build also required matching security/updates snapshot repositories and individual JavaScript-disable configure flags. No upstream or engine source was patched.

Expected diagnostics remain: Janus warns about private addresses without external STUN and unavailable optional RNNoise; AudioBridge's unused external RTP address detection falls back to loopback. coturn warns about absent TLS certificate files while TLS/DTLS are disabled. Auth-negative and stale-session tests deliberately generate service error logs. These do not establish Internet, TLS, AudioBridge media or device behavior.

Scope remains explicit: HTTP/VideoRoom control and Docker-internal UDP TURN traffic are live; WebSockets is loaded/advertised, with message flows untested. No native Unreal client negotiated a Janus PeerConnection in this task. Direct signaling is an offline draft oracle, not an implemented endpoint. This turn did not rebuild the SR-004 packaged Game or rerun Metal/audio playback; those historical results above precede the canonical v1 adapter. Current v1 Unreal verification is the Full headless Editor automation run. Mobile, Windows and native Linux Unreal builds remain unverified. Reproduction and credential/network boundaries are in [Fixtures](FIXTURES.md); wire examples are in [Signaling v1](SIGNALING_V1.md).

</details>
