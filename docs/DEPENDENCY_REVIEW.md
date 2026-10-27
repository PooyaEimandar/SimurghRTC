# Native dependency review

**SR-003 reviewed on 2026-10-19 for Unreal Engine 5.8.3.** The existing native integration is suitable for controlled feasibility tests. It is **not approved as a production security or distribution baseline**. SR-008 now owns the dependency updates and missing provenance; SR-902 owns the final notice bundle.

**SR-008 follow-up:** see [current fixes and migration status](DEPENDENCY_SECURITY.md). The tables below describe the original SR-003 inputs; its inventory snapshot is preserved.

The headless Mac Server, Samsung and iOS simulator results remain valid for the exact binaries tested. Completing this review does not close independent-host builds, physical iPhone testing or Shipping app-size validation. See [M0 results](M0_VALIDATION.md).

## What we actually link

The [inventory snapshot](reviews/SR003-inventory.json) records hashes, embedded version strings, license inputs and mobile archive checks. [MobileWebRTC.lock.json](../Config/MobileWebRTC.lock.json) contains the full source and toolchain pins.

| Component | Reviewed input | Finding |
|---|---|---|
| Desktop WebRTC | Epic's `5414` / M109 Release archive | Mac universal archive: 41,870,008 bytes, SHA-256 `172e7fdd53b10e78b5e06a268fca9f8ee85492e24f04fea800b65c35c07966cf`. Exact upstream commit and Epic patch history remain unknown |
| Mobile WebRTC | `4e8a5ac68e8a4ae0588f54f2fdb8cbd1eb5fa50d` | Separate Android, iOS device and simulator archives; headers and patches travel with each artifact |
| OpenSSL | Unreal's 1.1.1t archives and matching platform headers | Confirmed by embedded version strings. No vendor security-backport record was found |
| Opus | Unreal's `opus-1.1` module | Mac says `libopus 1.1`; device/simulator say `libopus 1.1-beta`; Android says `libopus unknown-fixed`. Folder names are insufficient provenance |
| Speex resampler | Additional Mac/Android input from `libOpus` | Separate static archive and BSD notice in its source/header. Exact source-to-binary revision is not recorded |
| VPX | Unreal 1.14.1; simulator rebuilt from the pinned 1.14.1 tarball | Used for VP8/VP9. The older VPX checkout fetched by WebRTC is not the linked implementation |
| Mobile SRTP / YUV | libsrtp `5b7c744e…`; libyuv `fe9ced6e…` | Exact commits are pinned. libsrtp uses external OpenSSL. Internal README/version labels differ from checkout IDs, so use the lock for identity |
| Other mobile native code | Abseil, CRC32C, PFFFT, RNNoise and WebRTC's small audio/utility libraries | Included by the native GN graph and its generated notices. Abseil/PFFFT/RNNoise are pinned through the Chromium `third_party` commit |
| Simulator HTTP/2 support | nghttp2 1.64.0 | A separate sample compatibility library, not part of libwebrtc. It also needs a security update |

Mobile notices list WebRTC, Abseil, base64, CRC32C, FFT, G.711, G.722, libsrtp, libyuv, Ooura, PFFFT, RNNoise, sigslot and `spl_sqrt_floor`. Android adds NDK/SDK notices. JSONCPP, PortAudio and SocketRocket notices cover exported headers; their presence does not mean those libraries run in the app. Likewise, fetched AOM, dav1d, JPEG, ICU and test/build repositories are not automatically runtime dependencies.

### Codecs, audio and private APIs

The mobile build enables software VP8/VP9, builtin audio codecs and audio processing (APM). Its declared audio set is Opus, ISAC, G.722, PCMU, PCMA, multiopus and L16. Factory capability lists can also include repair/auxiliary formats. H.264, AV1, internal audio devices and Java/Objective-C SDK wrappers are disabled. No hardware codec, microphone, camera or network-change behavior is established by these probes.

The data-only factory omits the media engine, ADM and APM, but still links the same media-capable archive. It is not a codec-stripped dependency. APM construction also does not establish acoustic echo cancellation quality or correct mobile audio routing.

WebRTC types stay behind the native module's private implementation. The adapter uses M109's `AudioDeviceModuleDefault` for its dummy audio device and owns the network, worker and signaling threads. Factory/peer teardown precedes thread joins and the final SSL cleanup. There is no dependency on Pixel Streaming's private APIs and no plugin-required engine patch. Keep the engine 5.8.3/`WEBRTC_5414` guards; a WebRTC or engine update requires an adapter review, not just a changed version string.

### Patch review

The current lock contains 13 patches. They select the native factory/APM/codecs, use Unreal's external crypto/codec headers, adapt old code to the selected compilers, remove unused Java/test/tool targets, use the pinned GN license generator and restore the standard Android C++ vtable layout. Patch 0002 changes libsrtp's crypto dependency from BoringSSL to Unreal's OpenSSL. Patch 0013 fixes the actual Android virtual-call crash recorded in [M0 results](M0_VALIDATION.md#packaged-mobile-probe-work).

These are compatibility/build changes; none is recorded as an upstream security backport. The earlier iOS device artifact contains 12 patches and its older build lock. The Android and simulator repeat artifacts contain 13. The inventory preserves this distinction instead of relabeling the older artifact.

## License and distribution review

The repository's Apache-2.0 license covers our code. It does not replace engine or dependency licenses. Epic points to the engine's `Engine/Source/ThirdParty/Licenses` directory for additional third-party terms; redistributing engine code also requires checking the applicable engine agreement. [Epic's terms, section 7(b)](https://www.unrealengine.com/eula/unreal).

| Input | Terms found locally | Distribution action |
|---|---|---|
| WebRTC, libsrtp, libyuv, CRC32C, RNNoise | BSD-style licenses | Retain copyright, conditions and disclaimer; preserve WebRTC/libyuv patent grants |
| Abseil | Apache-2.0 | Include license and applicable notices; retain modification/source attribution where required |
| PFFFT, FFT/Ooura, G.711/G.722, base64, sigslot and sqrt helper | Component-specific permissive/public-domain texts | Preserve the actual generated text rather than applying a blanket BSD label |
| OpenSSL 1.1.1t | Legacy OpenSSL **and** SSLeay licenses | Include both texts and their acknowledgments; review advertising/name restrictions. Do not substitute the OpenSSL 3 license |
| Opus and Speex | BSD-style copyright notices; Opus references patent grants | Include both components' notices and retain the Opus patent references |
| libvpx | BSD license and separate patent grant | Include license, patent grant and notices applicable to the selected implementation |
| Exported JSONCPP / PortAudio / SocketRocket headers | Their supplied license texts | Keep notices with the header distribution even when the runtime code is excluded |
| Android C++ runtime / system SDKs | NDK notice and platform terms | Preserve relevant runtime attribution; SDK/build-tool inclusion in a report is not proof that it ships |
| Simulator nghttp2 | MIT | Include its own `LICENSE.txt` when distributing the support library/sample |

The existing mobile `Licenses/` directory covers the generated WebRTC graph and exported headers. It does **not** include the external Unreal OpenSSL/Opus/Speex notices. Simulator VPX has a separate license file, but its patent grant is not exported. The Speex notice is available in `libOpus/opus-1.1/include/speex_resampler.h`; the engine VPX source tarball contains `LICENSE` and `PATENTS`. Desktop WebRTC's supplied top-level notices do not establish a complete inventory of Epic's embedded third-party code.

**SR-902 distribution gate:** assemble the applicable notices from exact source inputs, account for engine-linked libraries, and verify they are delivered with each plugin artifact and final app. Include patent files and source/patch references, keep diagnostic symbols separate, and review any codec/patent or redistribution conditions before publishing. A manifest containing a nonempty notice file does not satisfy this gate. No complete final-app notice bundle or legal clearance is claimed here.

## Security review

This is a dated source/configuration and advisory review, not an exhaustive vulnerability scan. Version matches identify candidates for investigation; they do not prove that a particular function is reachable or that a vendor binary lacks a backport.

| Finding | Evidence and disposition |
|---|---|
| OpenSSL baseline is unsupported by public upstream maintenance | 1.1.1 reached public EOL on 2023-09-11. Later fixes, including CVE-2023-0464/0465 and 2023-2650, postdate 1.1.1t. Their impact depends on enabled policy/display paths; no exploit in this adapter was demonstrated. Obtain a supported, identified dependency set or documented vendor backports. [EOL notice](https://openssl-corporation.org/blog/2023-06-15-1.1.1-EOL-Reminder.html), [advisories](https://openssl-library.org/news/vulnerabilities-1.1.1/) |
| M109 has no maintained security-fix inventory in this project | WebRTC advisories after this pin include CVE-2023-7024, CVE-2024-5493 and CVE-2026-9111/9119/9120. Chrome release numbers alone do not prove applicability to standalone M109. Source-level fix mapping and Epic patch provenance remain required. [2023 advisory](https://chromereleases.googleblog.com/2023/12/stable-channel-update-for-desktop_20.html?hl=ru), [2024 advisory](https://chromereleases.googleblog.com/2024/05/stable-channel-update-for-desktop_30.html), [2026 advisory](https://chromereleases.googleblog.com/2026/05/stable-channel-update-for-desktop_0841193308.html) |
| VPX and other media dependencies need current qualification | Upstream VPX 1.14.1 includes the CVE-2024-5197 fix. A later libvpx advisory, CVE-2026-1861, still needs source-level mapping to our binaries. Unknown Opus/Speex provenance prevents a reliable version-based assessment; the pinned libsrtp/libyuv/Abseil/audio helpers are not thereby cleared either. [VPX release](https://chromium.googlesource.com/webm/libvpx/+/refs/tags/v1.14.1), [later advisory](https://chromereleases.googleblog.com/2026/02/stable-channel-update-for-desktop.html) |
| Simulator nghttp2 falls in an affected version range | CVE-2026-27135 affects versions through 1.68.0; fixed in 1.68.1. Our 1.64.0 support build does not define `NDEBUG`. The advisory requires particular enabled extension-frame/callback paths; those paths have not been demonstrated in the probe. Upgrade or backport and retest before using this support library with untrusted HTTP/2 traffic. [Maintainer advisory](https://github.com/nghttp2/nghttp2/security/advisories/GHSA-6933-cjhr-5qg6) |

At the SR-003 review revision, shared `Connect` returned `NotImplemented`; factory probes used no remote SDP, ICE or media, and codec tests used generated local input. M1 now exchanges SDP/ICE and DataChannel messages, including through the [shared session API](SHARED_SESSIONS.md). Its local tests do not qualify a release dependency baseline. DTLS/SRTP, SDP, ICE/STUN/TURN, SCTP and codec parsing remain within the dependency review scope. No encryption bypass was found in the adapter; the disabled network monitor is a mobile integration limitation, not a security fix.

**SR-008 security gate:** the SimurghRTC maintainer owns a supported source/patch inventory for every linked component, advisory applicability records, refreshed pins and repeat ABI/package checks. Resolve this before untrusted network acceptance or a supported release. Controlled local implementation can continue. Do not replace OpenSSL or WebRTC with an additional conflicting copy inside Unreal; qualify the whole linked dependency set.

## Mobile ABI decision

Keep **three separate ARM64 artifacts**. An iOS device archive cannot substitute for an ARM64 simulator archive.

| Target | Reviewed toolchain and ABI | Acceptance |
|---|---|---|
| Android `arm64-v8a` | Linux x86-64 build container, NDK r27c `27.2.12479018`, Clang 18.0.3, API 26; standard Itanium vtables; Unreal-compatible static libc++ and local symbol binding | Whole-archive link passed; corrected Direct-only/Full packages passed on Galaxy S25 Ultra. Link probes and recorded final ELF segments have at least 16 KiB alignment |
| iOS device `arm64` | Xcode 27.0 `27A266a`, SDK 27.0, Apple Clang 21, minimum iOS 15; system libc++ | Archive/member and link checks passed. Physical iPhone acceptance is deferred; the older manifest lacks the later explicit `cpp_abi` field |
| iOS simulator `arm64` | Xcode 27.1 `27A9275`, SDK 27.1, Apple Clang 21, minimum iOS 15; system libc++; simulator-specific VPX and HTTP/2 support | Repeat builds and packaged Direct-only/Full lifecycle probes passed on iOS 27.0 simulator |

The validators check every payload hash and ARM64 archive member; reject device/simulator mixtures, unexpected binaries, fat/thin archives and unsupported bitcode/LTO objects; and require Android's explicit `itanium` ABI record. Link probes check one WebRTC implementation with external crypto/codecs. The adapter uses matching headers and keeps C++ allocation/lifetime ownership within its private implementation. These checks do not establish ABI stability across WebRTC releases.

Android must use a consistent C++ runtime across the final native graph. Static-runtime use needs particular care if additional shared libraries exchange C++ objects or allocation ownership. A 16 KiB-aligned archive link probe alone does not validate APK/AAB ZIP alignment or behavior on a 16 KiB device; those remain package/runtime gates under SR-006/SR-805. [Android C++ guidance](https://developer.android.com/ndk/guides/cpp-support), [page-size validation](https://developer.android.com/guide/practices/page-sizes).

Engine Release WebRTC has no supplied debug symbols. Mobile export retains separate unstripped `.symbols` archives; these are not final app dSYMs or an Android symbol upload. Keep them matched to the exact binary. Archive sizes are not download sizes or RTC's incremental app cost. SR-806 covers final diagnostics and SR-807 covers Shipping size budgets.

## Follow-up owners

| Issue | Owner | Required result |
|---|---|---|
| SR-008 | SimurghRTC maintainer | Identify Epic WebRTC/Opus/Speex provenance, qualify supported crypto/media/HTTP2 dependencies and record security-fix applicability; rerun ABI, repeat-build and package probes |
| SR-902 | SimurghRTC release maintainer | Complete and validate artifact/app notices, patent grants and redistribution scope before publishing |
| SR-006 / SR-007 | SimurghRTC mobile maintainer | Independent-host reproduction and remaining device/package gates; physical iPhone testing stays deferred |
| SR-805 / SR-806 / SR-807 | SimurghRTC release maintainer | Final profile contents, Android page-size packaging/runtime checks, usable symbols and measured app-size budgets |

These are responsibility roles within this project, not assignments to external engine/library vendors. Windows/Linux runtime behavior, mobile hardware media and network-change handling remain unverified.

## Reproduce the inventory

Run from the repository root with the accepted artifacts available locally:

```sh
python3 tools/audit_native_dependencies.py \
  --engine /absolute/path/to/UnrealEngine \
  --android LocalBuilds/Artifacts/Android-repeat-20261018-a \
  --ios LocalBuilds/Artifacts/IOS-arm64-release \
  --simulator LocalBuilds/Artifacts/IOSSimulator-repeat-20261018-a \
  --simulator-support LocalBuilds/SimulatorSupport/nghttp2-arm64-xcode271 \
  --output Intermediate/SR003/inventory.json
python3 -m unittest discover -s tools/tests -v
```

The command reuses the archive validators, checks external mobile library/header hashes against their locks, and records desktop libraries, rules and license inputs. It neither downloads advisories nor approves a release. Compare a new report with the reviewed snapshot, investigate every changed input and refresh the advisory review before approving a new baseline. This review does not rebuild or modify the accepted libraries or the shared engine.
