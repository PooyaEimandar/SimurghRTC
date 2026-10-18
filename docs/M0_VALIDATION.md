# M0 status and test results

Updated **2026-10-18**. M0 is still in progress. Local media feasibility (SR-004) and repeatable fixtures (SR-005) are complete on the available host.

The plugin has working test probes, but the shared `Connect` API still returns `NotImplemented`. No Unreal RTC call or network media flow has been demonstrated yet.

## What passed

Tests ran on a Mac with Apple M2 Max and UE 5.8.3. Janus/TURN ran in Linux ARM64 containers through Docker Desktop.

| Check | Result | What it establishes |
|---|---|---|
| Python checks | 104 passed | Pinned inputs, artifacts, fixtures and mobile report validation, including stale results, wrong profiles, missing cleanup and non-ARM64 packages |
| Compiled UBT artifact validator | 30 current fixture cases passed; 2 earlier device exports passed | Valid Android/iOS metadata and rejection of missing, changed, extra or incompatible inputs |
| Android/iOS ARM64 dependencies | Built and linked; original repeat comparisons passed | Corrected Android ABI and separate iOS simulator exports are recorded below |
| Packaged Android lifecycle | Direct-only and Full passed on Samsung S25 Ultra | Each run created and closed nine peers; dependency identity and app cleanup verified |
| Packaged iOS simulator lifecycle | Direct-only and Full passed on ARM64, iOS 27.0 | Same nine-peer checks, matching launch PID and dependency identity, verified app shutdown |
| Full Unreal automation | 7 passed; no test warnings or failures | API lifecycle, native factory/peer setup and repeated local codec tests |
| Packaged Server probe | Passed, including data-only and occupied-port cases | Native factory and HTTPServer work without graphics or hardware audio |
| Packaged Direct-only Game media probe | Passed, including missing-graphics/audio cases | Local codecs, texture upload, render-target capture and audio-mixer consumption |
| Janus/TURN service tests | Passed twice, including a fresh restart | Janus authentication/session cleanup and a 29-byte binary message relayed in both directions through TURN |

The packaged media result predates the versioned media adapter added in SR-005. That adapter was compiled and tested in the Full headless Editor; the packaged Game was not rebuilt for SR-005.

These results do not establish Windows, native Linux Unreal or full Android/iOS support. Mobile evidence currently covers idle PeerConnection lifecycle; iOS uses the simulator with graphics and hardware audio disabled. Janus WebSockets loaded, but its message flow was not tested. Local service tests used HTTP/VideoRoom and Docker-internal UDP TURN traffic.

## What remains in M0

| Issue | Remaining work |
|---|---|
| SR-001 | Review and freeze the API; finish device/headless contracts and working C++/Blueprint flows |
| SR-002 | Finish executable Blueprint flows and remaining profile/package coverage |
| SR-003 | Complete dependency, license, security and mobile ABI review |
| SR-006 / SR-007 | Repeat current dependencies on independent hosts. Physical iPhone validation is deferred |

SR-005 is complete as a fixture task. Later SR-102/SR-301 work must run its message contracts against real Unreal providers; SR-305 covers provider parity. A Python model passing does not mean those providers are implemented.

## Reproduce the checks

- [Building](BUILDING.md): Python and Unreal automation.
- [Server probe](NATIVE_PROBE.md): packaged headless Server.
- [Media probe](MEDIA_PROBE.md): codecs, textures and audio integration.
- [Fixtures](FIXTURES.md): fixed input files and local Janus/TURN.
- [Mobile artifacts](MOBILE_ARTIFACTS.md): dependency packaging and build-rule checks.
- [Mobile builds](MOBILE_BUILDS.md): pinned sources/toolchains, separate ARM64 libraries and repeat-build checks.
- [Mobile probe](MOBILE_PROBE.md): package and run on Android or the iOS simulator.

Generated logs and reports stay under `Intermediate/` and are not committed. Start with `Intermediate/MobileProbes/current-results.json` for the current mobile runs.

The final simulator rebuild uses the supplied source engine at revision `2cafb2d9194e503358a1fbeced79e621f2620abc`. Its tracked files were clean when recorded. This shared checkout advanced during other work; `engine-provenance.json` records the revision, and this task introduced no engine source edits.

## Mobile source-build checks

All three targets build from pinned WebRTC 5414 sources and checked toolchains. Device and simulator libraries are separate. See [Mobile builds](MOBILE_BUILDS.md) for commands and the expanded record below for hashes and earlier repeat builds.

| Current artifact under `LocalBuilds/Artifacts/` | Stripped WebRTC archive | Status |
|---|---:|---|
| `Android-arm64-itanium` | 42,103,278 bytes | Standard C++ ABI; packaged Samsung probes pass |
| `IOS-arm64-release` | 25,297,696 bytes | Device build/link and repeat comparison pass; physical run deferred |
| `IOSSimulator-arm64-current` | 25,273,184 bytes | Packaged Direct-only and Full simulator lifecycle probes pass |

These archive sizes are not app sizes. Repeat builds of the corrected Android and new simulator revisions, plus independent-host reproduction, remain open.

## Packaged mobile probe work

The [mobile probe guide](MOBILE_PROBE.md) stages Direct-only and Full projects with checked ARM64 artifacts. The opt-in native probe exercises three lifecycle cycles: nine factories and peers, explicit stop, overlapping owners, restart, duplicate-call rejection and destructor cleanup. Every report identifies the dependency manifest compiled into the app.

The Mac Editor dry run passed all lifecycle checks. Its evidence is `Intermediate/MobileProbes/DesktopClient/result.json`; it uses the engine's desktop WebRTC and is not mobile acceptance. The 104 Python tests include rejection of stale reports, mismatched dependencies/profiles, incomplete cleanup, simulator evidence and APKs containing another architecture.

The sample Editor target has explicit `-SimurghRTCAndroidCook` and `-SimurghRTCIOSCook` options to build the required mobile cooking modules. Android and iOS simulator cooking and packaging passed. Build each project's Editor before cooking it, and avoid overlapping UBT builds; the [guide](MOBILE_PROBE.md) explains the shared-build constraints.

Both Android profiles passed on Samsung SM-S938B (Galaxy S25 Ultra), Android 16, ARM64, with 4 KiB system pages. Each run completed three cycles, nine factories/peers, six explicit stops and three destructor cleanups; the harness then verified the app had stopped. The Full result has `with_janus: true`; this checks module inclusion, not a Janus network connection. Both final link maps select one staged WebRTC archive and preserve 16 KiB ELF load alignment.

| Android Development package | APK bytes | Packaged `libUnreal.so` bytes | Evidence directory under `Intermediate/MobileProbes/` |
|---|---:|---:|---|
| Direct-only | 103,183,903 | 171,897,584 | `AndroidDirectItanium/` |
| Full | 103,188,083 | 171,903,088 | `AndroidFullItanium/` |

These are ARM64-only, cooked Development APKs (about 98.4 MiB each), not Shipping delivery or RTC-only size deltas. Each evidence directory contains `result.json`, the correlated device log and command log. Build/package logs and `android-*-itanium-link-audit.json` are alongside them.

The corrected Android archive is 42,103,278 bytes, SHA-256 `950bae6ef80ee71d0509d235f7eb0e5dfa1aa03d964cb8f3004df572e781b5d3`. Both apps report artifact manifest `f35b32bafa9a0c14bd1ab9a4121d09de9fbff2483fbd093270db8b55cde026cc`. Fresh-output repeat comparisons for this corrected revision and independent-host reproduction remain open.

The Direct-only iOS native executable compiled and linked successfully after scoping a Clang warning suppression to the pinned Abseil headers. Its link response/map select one iOS WebRTC archive, with iOS 15.0 minimum and SDK 27.0. Evidence is `ios-direct-build-fixed.log` and `ios-direct-link-audit.json`. Physical iPhone testing is deferred at the user’s request. Current iOS work targets the installed simulator; no older Xcode setup is planned. An unsigned device executable is not a simulator or device runtime pass.

Both simulator profiles passed on the installed iPhone 17 simulator, iOS 27.0 (`24A434`), ARM64. Each run completed three cycles, nine factories/peers, six explicit stops and three destructor cleanups. Reports match the launch PID, run UUID and simulator artifact manifest `a0dd1843b77eb2288dec27c96d3ff438f2913bfd9dc094cdc551313b82fb746d`. Both apps were stopped and their processes confirmed absent.

| iOS simulator Development package | Bundle file bytes | Executable bytes | Evidence directory under `Intermediate/MobileProbes/` |
|---|---:|---:|---|
| Direct-only | 283,675,760 | 251,680,960 | `IOSSimulatorDirectValidated/` |
| Full | 283,698,000 | 251,702,800 | `IOSSimulatorFullValidated/` |

These are simulator bundle measurements, not iPhone delivery sizes or Shipping RTC deltas. Each evidence directory contains the saved native report, launch/command logs and `result.json`. The `ios-simulator-*-final-link-audit.json` files bind the final packaged executable hashes to maps selecting only the simulator WebRTC, VPX and HTTP/2 archives, with Janus off/on as expected.

The simulator uses separately built libwebrtc and portable-C VPX, plus pinned `nghttp2` 1.64.0 because Unreal’s bundled HTTP/2 archive is device-only. The sample enables Unreal's scene lifecycle and supplies project-local metadata for its headless `-nullrhi -sm5` startup. All 30 compiled UBT validator cases passed (`SimulatorRules/result.json`). Tests reject device/simulator object mismatches and prevent simulator reports from counting as physical-device results. No engine source edits were needed.

Network media, provider interoperability, suspend/resume and Shipping RTC-off/on size comparisons remain separate work.

The host updated from Xcode 27.0 to 27.1 during this run. Dependency artifacts retain their pinned 27.0 provenance. Both final simulator executables and packages use 27.1 (`27A9275`), SDK 27.1 (`24A94232`). Dependency pins remain unchanged; runtime evidence records the host and bundle toolchains separately.

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

The [native probe audit](NATIVE_PROBE.md#build-selection-and-provenance) records the archive hash, local dependency versions, symbols and observed codecs. The Mac archives in the source and installed engines are identical. `SIMURGHRTC_WITH_NATIVE_TRANSPORT=0` still describes the unavailable shared session service, not factory linkage. Android/iOS engine WebRTC branches are deliberately not selected; their separate dependency builds are recorded above, while device acceptance remains SR-006/SR-007 work. No engine source changes were introduced.

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
| Janus-only / data-only profile engine builds | **Unverified** | Python staging is tested; runtime data-only factory mode is distinct from a packaged data-only profile |
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
