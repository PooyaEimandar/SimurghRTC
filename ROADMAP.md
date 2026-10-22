# SimurghRTC — Unreal Engine WebRTC plugin roadmap

**M0 is in progress**, targeting Unreal Engine **5.8.3**. Updated 2026-10-22.

- **Done on the available Mac:** SR-001 API v1 and C++/Blueprint lifecycle examples, SR-002 build profiles, SR-003 dependency/ABI review and headless Server feasibility, SR-004 local media tests and SR-005 repeatable fixtures/service tests.
- **Integrated and locally tested:** the isolated M155 backend on Mac, Android and the iOS simulator. Android Direct-only/Full packages pass lifecycle, local codecs, capture and mixer checks; both simulator profiles pass lifecycle/local codecs. Fresh repeat builds also pass on those three targets. The packaged Mac dedicated Server passes factory, HTTP and cleanup checks in both modes. SR-006/SR-007/SR-008 remain open for independent-host reproduction, remaining iOS checks and dependency qualification.
- **Still to prove:** real connections, network media, signed physical iOS behavior and broader platform/release coverage. See [M0 validation](docs/M0_VALIDATION.md) for exact test scope; the current dependencies are not a qualified release baseline.

For a quick overview, read [delivery stages](#2-structure-and-delivery) and [milestone status](#8-status). Each issue below explains the work, its dependencies and how to verify completion. Test results and detailed build evidence are in [M0 validation](docs/M0_VALIDATION.md).

## 1. Goal and scope

Build a standalone plugin for standard Unreal Engine 5.8.3. Developers should use the same C++ API and Blueprint nodes to send WebRTC data, audio and video to either a native Unreal server or Janus. Use separately built, pinned libwebrtc dependencies for desktop clients/servers and Android/iOS clients, with optimized ARM64 mobile builds. SR-008 has replaced the Mac/mobile RTC backend; Windows/Linux integration and dependency qualification remain open. Include native desktop/mobile sample projects and require no engine-source patches.

The plugin includes the Unreal server endpoint, which must be enabled and configured. A normal Unreal dedicated server does not expose WebRTC automatically. Mobile targets are clients only; the Server profile remains on desktop/server hosts. Janus and STUN/TURN run separately; provide repeatable local configurations for testing.

This plan excludes gameplay replication transports, UDP gateways, WebTransport, raw QUIC, HLS, a custom SFU and hosted services. DataChannels carry application messages; Unreal replication and its NetDriver remain separate.

Here, a **provider** is the Direct Unreal or Janus server integration. Both use the same client API and native libwebrtc backend. **Signaling** sets up the connection; **DataChannels** carry messages; media tracks carry audio/video. Janus supplies the optional **SFU** that forwards media among room participants.

### Success criteria

| Target | Required evidence |
|---|---|
| Unreal client to Unreal server | Bidirectional text, binary data, audio and video between native Unreal clients and a native UE Server target using the plugin |
| Unreal client to Janus | Equivalent common operations against Janus, plus explicitly identified Janus extension services |
| One client API | Shared C++ demo logic and Blueprint graphs switch provider through configuration; no provider branches for common operations |
| Janus demo coverage | Every entry in [the demo matrix](docs/DEMO_MATRIX.md) has an implementation issue, prerequisites and an acceptance outcome |
| C++ and Blueprints | Public operations have matching success, failure, cancellation and lifecycle behavior in both interfaces |
| Standard engine | The plugin installs, compiles and packages on standard UE 5.8 without engine-source patches |
| Optional Janus | A Janus-disabled package contains no Janus provider module, extension assets or active Janus dependency |
| Android/iOS clients | Packaged ARM64 Unreal clients pass common data/media contracts against Direct Unreal and applicable Janus services on physical devices, using separately built libwebrtc |
| Mobile dependency builds | Pinned source, patches, toolchains, flags, codec sets, checksums and notices reproduce the selected artifacts; each target links one libwebrtc implementation |
| App size | Matching Shipping apps with RTC on/off record device-specific download and installed/uncompressed size deltas for each client profile; measured budgets and regression checks pass |

## 2. Structure and delivery

```text
M0 Foundation and feasibility
 └─ M1 Direct Unreal data → M2 Direct Unreal media
      └─ M3 Janus provider parity → M4 Shared demo suite
           ├─ M5 Janus SFU rooms
           ├─ M6 Janus service demos
           └─ M7 Advanced media demos
                ↓ release integration ↓
             M8 Production hardening → M9 Documentation and release
```

The diagram shows delivery stages. Follow the prerequisites and issue dependencies for the actual build order. M6 and M7 can run independently once their dependencies are ready. Build checks start in M0 and expand throughout development. Android/iOS dependency probes begin in M0, Direct data and media acceptance follows in M1/M2, and Janus/mobile demo conformance follows in M3/M4. Mobile optimization and app-size qualification close in M8; mobile is required planned scope, not a future-only target.

1. **Direct endpoint preview:** M0 to M2. Prove the most important integration risks first: a real UE Server target, native media integration, media on a headless endpoint, Android/iOS ARM64 integration and usable Blueprints.
2. **Shared provider preview:** M3 and M4. One application API works with Direct Unreal and Janus; portable demos demonstrate the contract.
3. **Janus services preview:** M5 and M6. SFU and service-specific functionality is optional and clearly separated from the portable API.
4. **Advanced media and release:** M7 to M9. Complete the requested demo scope, validate quality and distribute reproducible packages.

A preview describes what is included; it does not mean production-ready. Set dates and performance targets after the M0 measurements. A capability check or disabled UI does not complete an unsupported demo. Any smaller release must clearly state what it leaves out.

## 3. Working conventions

- **Documentation:** use plain language and short paragraphs. Start with the purpose, current status and practical steps. Explain unfamiliar terms, and keep detailed protocol rules and build evidence in reference sections.
- **Structure:** milestones group epics; epics group numbered issues. Each issue belongs to one epic and milestone, but can depend on work elsewhere.
- **Issue IDs:** keep the `SR-xxx` IDs stable. They identify this local plan and are not published GitHub issue numbers. Each issue appears once, under its epic.
- **Priority:** P0 is foundational work; P1 is required functionality or hardening; P2 is advanced examples and optional services. All remain in scope.
- **Dependencies:** links open the relevant epic. Find the issue by its ID.
- **Parallel work:** an epic need not be a separate runtime module. Work can proceed when its listed dependencies are ready.
- **C++ and Blueprints:** provide both for client features. Server internals may stay in C++; expose the session, data and media callbacks needed by samples.
- **Done:** finish and verify every issue before closing its epic, and every epic before closing its milestone. Each must also meet its completion criteria. A successful compile does not prove runtime behavior.
- **Test records:** record the engine/project/libwebrtc revisions, provider, target, build profile, toolchain, codec set and device/OS. Mark missing runners or devices Unverified. Check that Janus-only Blueprint assets do not break packages built without Janus, including Android/iOS.
- **Mobile artifacts and size:** follow [Mobile clients](docs/MOBILE_CLIENTS.md). Keep dependency archive size, delivery estimates and installed/uncompressed app size distinct; compare matching Shipping apps with RTC on/off. Establish numeric budgets from measured baselines, not vendor figures.

### Common definition of done

- The issue's acceptance criteria pass on the declared targets with evidence attached to its future tracking issue.
- Public async behavior, errors and cancellation are documented and equivalent between C++ and Blueprints.
- Common code has no provider-specific payloads or engine-private dependency leaks.
- New Janus-only functionality is absent from the Janus-disabled profile and discoverable through runtime capabilities when enabled.
- Relevant sample behavior is exercised in a packaged build, not only PIE; mobile acceptance requires physical Android/iOS devices.
- Mobile changes record the selected ARM64 artifact and update applicable size/capability evidence; smaller builds retain the declared data/media behavior.
- Dependencies and demo coverage are updated together; incomplete target coverage remains visible.

## 4. Milestones, epics, issues

SR-001 is **Complete (API v1 and executable lifecycle examples)**, SR-002 is **Complete (available Mac profile/package checks)** and SR-003 is **Complete (review with tracked release blockers)**; SR-006 and SR-007 are **In progress**; SR-004 is **Complete (available Mac feasibility)** and SR-005 is **Complete (versioned fixtures and local validation)**; SR-008 is **In progress**; all other issues remain **Planned**. Each issue explains the work and how to verify it. Priorities appear beside issue titles. The [demo matrix](docs/DEMO_MATRIX.md) tracks the requested examples.

### M0 Foundation and feasibility

A standalone plugin contract and proven native client/server build boundaries.

**Prerequisites:** None.

#### E01 API and build contract

**Complete on the available Mac.** [API v1](docs/API_CONTRACT.md) is frozen. Full, Direct-only, Janus-only and data-only pass UHT, compilation, cooking and packaged ARM64 lifecycle checks. Optional Janus Blueprint content is included only with its extension module. Mobile module selection is defined; broader mobile acceptance stays under SR-006/SR-007. See [validation results](docs/M0_VALIDATION.md#sr-002-build-profiles-and-optional-content).

##### SR-001 — Define the portable client contract (P0)

**Complete, 2026-10-19.** API v1 defines ownership, cancellation, errors, call topology, device discovery and headless CPU frames. Full and Direct-only examples execute success, failure, cancellation and EndPlay cleanup. These are local contract tests; shared network operations still return explicit implementation errors.

Define the shared API: sessions, calls, text rooms, channels, media sources and sinks, capabilities, errors and cancellation. Show how both Direct Unreal and Janus use native libwebrtc, and identify Janus-only operations.

**Done when:** reviewed C++ signatures and corresponding Blueprint node/event sketches cover success and failure flows; Direct-server and Janus call topologies are explicit; no public engine-WebRTC or Janus implementation types leak into the contract.

**Depends on:** None

##### SR-002 — Prove plugin modules and optional build profiles (P0)

**Complete, 2026-10-19 (available Mac).** All four client profiles pass Unreal automation and Editor/packaged lifecycle checks. Link maps and UHT inputs contain the selected modules, one libwebrtc archive and no Editor generator in Game. The optional Janus graph runs in Full/Janus-only and is absent from Direct-only/data-only; Server staging excludes it too. Earlier dedicated Server runtime evidence remains recorded under SR-003.

Create the plugin layout and one place to select modules, generate descriptors and set `SIMURGHRTC_WITH_JANUS`. Define Full, Direct-only, Janus-only and data-only client profiles for desktop and mobile, plus the desktop/server-host Server profile. Select one pinned libwebrtc implementation by target, independently of provider. The completed feasibility checks use engine libraries on desktop; SR-008 replaces this baseline with separately built dependencies on all targets.

**Done when:** UHT, compile and sample cook pass with Janus on/off in standard UE on the available Mac; reflected extension types/assets are excluded cleanly and dependency graphs contain only selected modules. Mobile linkage/startup is verified separately by SR-006/SR-007.

**Depends on:** [SR-001](#e01-api-and-build-contract)

**Epic complete when:** Reviewed public operations cover success, failure and cancellation; Janus-enabled and Janus-disabled UHT, compile and cook probes succeed on standard UE on the available Mac, with mobile module selection defined for the E02 probes.

#### E02 Native feasibility and test fixtures

**SR-005 complete:** fixed audio/video inputs, versioned signaling examples and local Janus/TURN tests are in place. The tests use pinned service versions and external credentials. See the [fixture guide](docs/FIXTURES.md) and [results](docs/M0_VALIDATION.md).

**SR-003 complete, 2026-10-19:** the [dependency review](docs/DEPENDENCY_REVIEW.md) records linked versions, licenses, security findings and mobile ABI constraints. A packaged Mac ARM64 Server creates a libwebrtc factory and runs the HTTP test without graphics or hardware audio. Security/provenance remediation is SR-008; final distribution notices are SR-902. Review completion does not approve the current libraries for release.

**SR-004 complete on the available Mac:** the packaged Direct-only Game passed local VP8/Opus, texture, render-target and audio-mixer tests. Missing graphics/audio were also tested. The M155 backend also passes Samsung local media checks under SR-008; network media remains later work. See the [media guide](docs/MEDIA_PROBE.md).

##### SR-003 — Audit native libwebrtc and Server target feasibility (P0)

**Complete with follow-ups.** Exact mobile pins and local binary/header/notice fingerprints are recorded. Epic's upstream WebRTC revision/patch list and some codec source-to-binary identities are unresolved and assigned to SR-008. The [review](docs/DEPENDENCY_REVIEW.md) gives the decision, evidence and owners; the [Server guide](docs/NATIVE_PROBE.md) covers the runtime probe.

Check the engine headers and libraries, target restrictions, codecs, audio processing (APM), debug symbols, private API dependencies and distribution licenses. Record the engine libwebrtc revision and the Android/iOS ABI, dependency and adapter constraints for separate builds. Prove that a native UE Server target can create a PeerConnection factory and use the standard UE HTTPServer.

**Done when:** the available Mac runs a minimal headless endpoint probe without an Editor/RHI/audio device; record standard UE engine revisions and library versions. Unsupported host architectures remain unverified. Any required engine patch is a design blocker, not an assumed prerequisite.

**Depends on:** [SR-002](#e01-api-and-build-contract)

##### SR-004 — Prove plugin-accessible native media hooks (P0)

Build a small native test that generates video, updates textures, receives decoded PCM and captures a UE render target through APIs available to plugins. Define which game, render and audio threads own the work, and which device permissions are required.

**Done when:** a packaged native probe runs on standard UE on the available Mac; a bounded frame-copy path and UE audio integration strategy are demonstrated. Record API/codec constraints; private rendering access or engine patches are design blockers.

**Depends on:** [SR-002](#e01-api-and-build-contract)

##### SR-005 — Establish reproducible fixtures and contract tests (P0)

Create a sample project and test harness with generated audio/video and versioned signaling examples. Pin the Janus multistream release or commit and selected plugins. Provide repeatable local Janus and TURN test setups that do not depend on a public demo server.

**Done when:** fixture versions/configuration are recorded, protocol tests run deterministically, test credentials are external, and actual engine/host runs are distinguished from planned matrix entries.

**Depends on:** [SR-001](#e01-api-and-build-contract), [SR-003](#e02-native-feasibility-and-test-fixtures), [SR-004](#e02-native-feasibility-and-test-fixtures)

##### SR-006 — Build and integrate Android ARM64 libwebrtc (P0)

**In progress, 2026-10-19.** The pinned M155 Linux build now produces an isolated ARM64 backend that links with NDK r27c. Packaged Direct-only and Full probes pass lifecycle, local VP8/Opus, OpenGL capture and silent mixer checks on the Galaxy S25 Ultra. Final ELF load segments are 16 KiB aligned. Two fresh builds reproduce the accepted library bytes, and both native consumers pass on the Samsung. Independent-host reproduction remains open. Earlier M109 repeat results remain historical; see [current results](docs/M0_VALIDATION.md#sr-008-packaged-m155-integration).

Create a reproducible Linux-hosted build of pinned upstream libwebrtc for `arm64-v8a`, with UE-compatible NDK/compiler settings and required Java/JNI helpers. Record flags, codecs, patches, checksums, symbols and notices; validate native-library page-size requirements.

**Done when:** a clean host reproduces the artifact and a packaged standard UE probe runs on a physical Android device with Janus on/off, creates/closes a PeerConnection and links one libwebrtc implementation without engine patches. Record initial package/size baselines and exact device/toolchain versions.

**Depends on:** [SR-002](#e01-api-and-build-contract), [SR-003](#e02-native-feasibility-and-test-fixtures)

##### SR-007 — Build and integrate iOS ARM64 libwebrtc (P0)

**In progress, 2026-10-19.** Separate M155 device and simulator ARM64 libraries build with Xcode 27.1. Packaged Direct-only and Full probes pass on the iOS 27.0 ARM64 simulator: nine peer lifecycles, generated VP8 and decoded Opus PCM per profile. Graphics/audio-device behavior is untested there. Two fresh simulator builds reproduce the accepted library bytes, and both native consumers pass. The iOS device repeat build, independent-host reproduction and signed physical-device acceptance remain open. Physical iPhone testing is deferred at the user's request. See [current results](docs/M0_VALIDATION.md#sr-008-packaged-m155-integration).

Create a reproducible macOS/Xcode build of pinned upstream libwebrtc for device `arm64`, with matching headers and required Objective-C++ helpers. Select static library/framework packaging and XCFramework distribution where useful; keep simulator outputs and dSYMs separate.

**Done when:** a clean host reproduces the dependency artifact and a signed standard UE probe runs on a physical iPhone/iPad with Janus on/off, creates/closes a PeerConnection and links one libwebrtc implementation without engine patches. Record initial package/size baselines, toolchain/deployment settings, checksums and notices.

**Depends on:** [SR-002](#e01-api-and-build-contract), [SR-003](#e02-native-feasibility-and-test-fixtures)

##### SR-008 — Qualify the native dependency security baseline (P0)

**In progress; owner: SimurghRTC maintainer.** The simulator HTTP/2 fix and Epic distribution checks are implemented. The isolated M155 backend is integrated through a private C interface. Mac/Android packages pass lifecycle and local media probes; both iOS simulator profiles pass lifecycle/codecs, and the device library builds. Fresh repeat builds pass on Mac, Android and the iOS simulator. The replacement Mac dedicated Server passes builtin-media/data-only factory, HTTP and occupied-port cleanup checks. Source/advisory qualification remains open; see [SR-008 progress](docs/DEPENDENCY_SECURITY.md). On 2026-10-19, the maintainer approved separately built, maintained dependencies for desktop and mobile. Replace the opaque WebRTC/crypto/codec inputs with pinned source builds and isolate their symbols from Unreal's own libraries. Map relevant advisories to source and enabled features before untrusted network acceptance or a supported release. Controlled local feature development can continue while this work is open.

**Done when:** every native RTC component selected by the plugin has traceable source and patches, relevant dependency findings have recorded resolutions, and refreshed pins pass repeat builds, mobile ABI checks, one-copy linkage and packaged lifecycle/media tests. Any change that requires an engine source patch remains a design blocker. Revalidate the RTC dependency set together; do not introduce conflicting crypto/runtime symbols. SR-902 separately verifies distribution notices.

**Depends on:** [SR-003](#e02-native-feasibility-and-test-fixtures)

**Epic complete when:** Headless Server and native media probes run on the available Mac; separately built Android/iOS ARM64 libraries run in packaged device probes. Dependency manifests, initial size baselines, generated media and controlled Janus/TURN fixtures are reproducible, and SR-008 qualifies the native dependency baseline. Missing mobile build hosts/devices remain Unverified and do not close the epic.

**Milestone complete when:** Freeze the API and prove standard UE builds, headless Server/libwebrtc linkage and plugin-accessible native media on the available Mac, plus separate Android/iOS ARM64 dependency builds and packaged device startup. Prove Janus-disabled packaging on desktop and both mobile targets, and close SR-008's dependency qualification. Assign every unresolved blocker an owner and issue before feature work starts.

### M1 Direct Unreal data

Native Unreal clients exchange application data with an Unreal server.

**Prerequisites:** M0.

#### E03 Connections and DataChannels

##### SR-101 — Implement native PeerConnection ownership (P0)

Implement the private libwebrtc backend for clients and servers. Own its factory and threads, handle SDP/ICE, and route callbacks without using destroyed objects.

**Done when:** connect, fail, cancel and close repeatedly without late UObject access; ICE arriving before the remote description is handled; teardown completes without blocking Unreal's game thread.

**Depends on:** [SR-003](#e02-native-feasibility-and-test-fixtures), [SR-005](#e02-native-feasibility-and-test-fixtures)

##### SR-102 — Implement Direct Unreal signaling (P0)

Define a versioned UE HTTPServer protocol for creating sessions, exchanging offers/answers, polling trickled ICE and closing sessions. Add a UE HTTP client with bounded requests and a session admission hook. Document the schema, authentication, errors and compatibility rules so other endpoints can implement it.

**Done when:** sequence/correlation IDs handle duplicate, delayed and out-of-order responses; timeout/cancel and rejected sessions are deterministic. Local HTTP works for development; documented HTTPS deployment keeps media on WebRTC.

**Depends on:** [SR-101](#e03-connections-and-datachannels)

##### SR-103 — Implement common DataChannels (P0)

Expose text/binary messages, channel lifecycle, ordering/reliability options, negotiated message limits and buffered-amount/backpressure events. Define who owns each byte buffer and what happens when a send cannot be accepted.

**Done when:** binary round trips preserve bytes; reliable ordering and supported partial-reliability options behave as configured; oversized messages and saturated queues fail explicitly without unbounded memory growth.

**Depends on:** [SR-101](#e03-connections-and-datachannels), [SR-001](#e01-api-and-build-contract)

**Epic complete when:** Session setup, cancellation and shutdown are deterministic; text/binary integrity, ordering, negotiated limits and backpressure pass contract tests.

#### E04 Unreal server and client integration

##### SR-104 — Implement the Unreal server endpoint (P0)

Add the plugin server runtime and a sample Server target. Give each client its own session, addressed data callbacks and echo handler. Add a small test that distributes text-room messages to the appropriate sessions.

**Done when:** multiple clients connect concurrently, receive only authorized messages and release server resources on departure. The server runs headless and does not replace Unreal replication or provide a UDP proxy.

**Depends on:** [SR-102](#e03-connections-and-datachannels), [SR-103](#e03-connections-and-datachannels)

##### SR-105 — Validate native client and server session integration (P0)

Connect packaged native clients to the reference Unreal Server through the shared API. Test concurrent sessions, text/binary traffic, cancellation, backpressure and reconnect. Keep ownership separate for each session.

**Done when:** packaged native clients exchange text and binary with [SR-104](#e04-unreal-server-and-client-integration); game-thread dispatch, UObject lifetime, cancellation and world/PIE teardown do not leak sessions or invoke destroyed objects. Reconnect creates fresh session state.

**Depends on:** [SR-102](#e03-connections-and-datachannels), [SR-103](#e03-connections-and-datachannels), [SR-104](#e04-unreal-server-and-client-integration)

##### SR-106 — Expose Blueprint data APIs and the first sample (P1)

Add Blueprint async connection nodes, channel/data events and structured errors, with matching C++ examples. Provide native data-client maps that connect to the reference server.

**Done when:** equivalent graphs and C++ code pass message/timeout/disconnect scenarios in packaged builds; graph owners can be destroyed during pending operations safely.

**Depends on:** [SR-104](#e04-unreal-server-and-client-integration), [SR-105](#e04-unreal-server-and-client-integration)

##### SR-107 — Validate Android/iOS Direct data clients (P0)

Integrate the separately built mobile libraries into packaged ARM64 data-client samples through the shared API. Exercise authenticated HTTPS signaling, direct/forced-TURN routes, binary/text traffic, ordering, limits, backpressure, cancellation and reconnect against the Unreal Server.

**Done when:** physical Android and iOS devices pass equivalent C++/Blueprint flows, safely release sessions during owner destruction and foreground/background transitions, and report network loss without blocking the game thread.

**Depends on:** [SR-006](#e02-native-feasibility-and-test-fixtures), [SR-007](#e02-native-feasibility-and-test-fixtures), [SR-106](#e04-unreal-server-and-client-integration)

**Epic complete when:** Multiple native clients, including packaged Android/iOS ARM64 clients, exchange isolated messages with the headless server; reconnect, owner destruction and Blueprint lifecycle tests pass on physical devices.

**Milestone complete when:** Desktop and physical Android/iOS ARM64 clients exchange text and binary data with the Unreal Server through the shared API. Verify concurrent-client isolation, channel reliability, mobile lifecycle handling and the Blueprint sample.

### M2 Direct Unreal media

Bidirectional audio/video and usable Unreal media components.

**Prerequisites:** M1.

#### E05 Native media sources and devices

##### SR-201 — Integrate native audio capture and playback (P0)

Connect UE audio sources and sinks to libwebrtc. Define sample formats, resampling and clock ownership. Use the available echo cancellation and noise processing, including the UE playback signal as the echo-cancellation reference.

**Done when:** a two-way audio call has intelligible sustained playback; mute, device changes, removal and shutdown work. Headless consumers can receive PCM without an output device; processing capabilities reflect actual linked support.

**Depends on:** [SR-101](#e03-connections-and-datachannels), [SR-004](#e02-native-feasibility-and-test-fixtures), [SR-005](#e02-native-feasibility-and-test-fixtures)

##### SR-202 — Integrate native video and codec factories (P0)

Add generated video, camera and UE render-target sources, decoded-frame sinks and codec negotiation. Establish a software fallback before adding platform hardware encoding or decoding.

**Done when:** bidirectional frames have correct orientation/color/timestamps, negotiate a measured baseline codec and handle resolution/source changes. No dependency on Pixel Streaming private types is required.

**Depends on:** [SR-101](#e03-connections-and-datachannels), [SR-004](#e02-native-feasibility-and-test-fixtures), [SR-005](#e02-native-feasibility-and-test-fixtures)

##### SR-203 — Implement native device permissions and selection (P0)

List and select microphones/cameras, handle OS permissions, and report device availability and removal. Keep platform handles private and physical capture optional in Server builds.

**Done when:** packaged native clients can select available capture devices and report permission denial, device removal and capture failure through structured events. Missing hardware does not break generated-source or headless Server tests; blocked permissions never hang the game thread.

**Depends on:** [SR-004](#e02-native-feasibility-and-test-fixtures), [SR-201](#e05-native-media-sources-and-devices), [SR-202](#e05-native-media-sources-and-devices)

**Epic complete when:** Native media has correct audio/video formats and timestamps; baseline codecs, device selection, denied permissions and device loss are validated.

#### E06 Media lifecycle and Unreal playback

##### SR-204 — Implement source and track lifecycle controls (P1)

Support attaching, detaching and replacing tracks, muting, pausing and renegotiation. Tracks must outlive individual widgets and support valid UI rebinding. Set explicit queue limits.

**Done when:** replacing a source or remote sink mid-call preserves the session where supported; forced renegotiation, permission loss and track removal have consistent C++/Blueprint results with no orphaned media devices.

**Depends on:** [SR-201](#e05-native-media-sources-and-devices), [SR-202](#e05-native-media-sources-and-devices), [SR-203](#e05-native-media-sources-and-devices)

##### SR-205 — Implement headless Unreal server media fixtures (P0)

Add generated audio/video and consumers for received PCM/frames to the Server profile. Provide a test that decodes and re-encodes received media within fixed resource limits, plus a receive-only inspection test.

**Done when:** a native UE Server target sends and receives actual media with native Unreal clients without physical devices, Editor or RHI. Record CPU/codec constraints; do not describe decoded echo as a packet relay or SFU.

**Depends on:** [SR-104](#e04-unreal-server-and-client-integration), [SR-201](#e05-native-media-sources-and-devices), [SR-202](#e05-native-media-sources-and-devices)

##### SR-206 — Implement Unreal media components and sinks (P1)

Provide video textures and UMG bindings, audio playback components and callbacks for headless consumers. Define ownership on the game, render and audio threads, including world teardown.

**Done when:** multiple independent remote tracks can be displayed/played; destroyed actors/widgets release references safely; spatial audio behavior and platform limitations are explicit and tested.

**Depends on:** [SR-204](#e06-media-lifecycle-and-unreal-playback)

##### SR-207 — Expose Blueprint media APIs and direct media sample (P1)

Expose Blueprint controls for creating media sources, binding tracks/sinks, muting, replacing sources and receiving device/error events. Include matching C++ examples.

**Done when:** the same sample plays two-way audio/video with the Unreal server in packaged native builds; C++ and Blueprint controls have equivalent results and close all resources when leaving the map.

**Depends on:** [SR-106](#e04-unreal-server-and-client-integration), [SR-205](#e06-media-lifecycle-and-unreal-playback), [SR-206](#e06-media-lifecycle-and-unreal-playback)

##### SR-208 — Integrate and validate Android/iOS media lifecycle (P0)

Add private mobile capture, playback and hardware-codec adapters using the selected ARM64 libraries. Handle camera/microphone permissions, camera switching/orientation, audio focus/session interruptions, speaker/headset/Bluetooth routing and foreground/background transitions. Define echo-cancellation ownership and test the baseline software fallback.

**Done when:** packaged clients on physical Android/iOS devices exchange two-way audio/video with the Unreal Server, render remote tracks and expose matching C++/Blueprint controls; denial, device/route changes, interruption, resume and teardown have bounded, documented outcomes. Record codec/device limits and preliminary CPU, memory and thermal/battery observations; no unrestricted background-call guarantee is implied.

**Depends on:** [SR-107](#e04-unreal-server-and-client-integration), [SR-207](#e06-media-lifecycle-and-unreal-playback)

**Epic complete when:** Desktop and Android/iOS direct media samples exchange real audio/video with the server; source replacement, multiple sinks, mobile permissions/interruptions and teardown work equivalently from C++ and Blueprints.

**Milestone complete when:** Two-way audio/video reaches desktop and physical Android/iOS client textures/playback components and headless server consumers. Verify source replacement, mute, mobile device/lifecycle handling and teardown through matching C++ and Blueprint controls.

### M3 Janus provider parity

Use the same client API for the common Direct Unreal and Janus services.

**Prerequisites:** M2.

#### E07 Janus signaling and service adapters

##### SR-301 — Implement Janus signaling and transport lifecycle (P0)

Implement Janus sessions, handles, transactions, keepalives, JSEP and trickle ICE in C++. Use UE WebSockets for signaling, with a UE HTTP fallback behind the same transport interface.

**Done when:** delayed acknowledgements/final events, errors, disconnects and session expiry are handled correctly; IDs retain precision; credentials are redacted; provider protocol state stays in C++ behind the common API.

**Depends on:** [SR-102](#e03-connections-and-datachannels), [SR-005](#e02-native-feasibility-and-test-fixtures)

##### SR-302 — Map Janus Echo to the common API (P0)

Map Janus Echo setup, media/data exchange and supported controls to the shared session, track and channel API. Report the channel capabilities the service actually supports.

**Done when:** native Unreal clients pass applicable Direct echo contracts unchanged; bitrate controls and unsupported options have defined results, with no provider JSON in demo code.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-207](#e06-media-lifecycle-and-unreal-playback)

##### SR-303 — Map Janus VideoCall to the common API (P1)

Map registration, calling, accept/reject, busy/errors, hangup and renegotiation to the shared call/session API. Keep endpoint-directory policy separate from WebRTC transport.

**Done when:** calls between native Unreal clients work through Janus; incoming/outgoing call events map consistently; failed registration and abrupt peer departure clean up state. Demo usernames are not treated as production authentication.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-207](#e06-media-lifecycle-and-unreal-playback)

##### SR-304 — Map Janus TextRoom to common text rooms (P1)

Implement the TextRoom DataChannel message protocol. Map room membership and text events to the same API used by the Direct server chat test.

**Done when:** join/leave, participant identity and group messaging agree across providers; private-message support is advertised only where implemented; generic binary/unreliable requests are not silently converted into TextRoom operations.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-103](#e03-connections-and-datachannels), [SR-104](#e04-unreal-server-and-client-integration)

**Epic complete when:** Native clients complete each service flow; asynchronous transaction handling, channel capabilities, call cleanup and room membership have defined behavior.

#### E08 Provider conformance and optional Janus builds

##### SR-305 — Run provider conformance and switching tests (P0)

Run the same test harness with each provider on desktop and physical Android/iOS ARM64 clients, including applicable desktop/mobile and mobile/mobile peer combinations. Select applicable tests by capability, and test a clean reconnect when the provider changes.

**Done when:** common operations keep the same public semantics and error categories; the same test code runs against both providers. Capabilities distinguish build exclusion, server support and negotiation failure; no live-migration guarantee is implied.

**Depends on:** [SR-302](#e07-janus-signaling-and-service-adapters), [SR-303](#e07-janus-signaling-and-service-adapters), [SR-304](#e07-janus-signaling-and-service-adapters), [SR-107](#e04-unreal-server-and-client-integration), [SR-208](#e06-media-lifecycle-and-unreal-playback)

##### SR-306 — Validate Janus Blueprint parity and exclusion (P1)

Test provider selection and Janus common services through the existing Blueprint API. Package Janus-enabled and Janus-disabled desktop and Android/iOS ARM64 builds with the correct reflected modules and content.

**Done when:** no common Blueprint graph contains Janus payload handling; Janus-disabled native packages load without missing classes/assets and report the provider as unavailable.

**Depends on:** [SR-305](#e08-provider-conformance-and-optional-janus-builds), [SR-002](#e01-api-and-build-contract)

**Epic complete when:** Applicable contract tests pass unchanged for both providers; switching reconnects cleanly and Janus-disabled packages contain no broken class or asset references.

**Milestone complete when:** Echo, two-party calls and text rooms pass the same applicable API tests with Direct Unreal and Janus. Builds without Janus contain no Janus type or asset references.

### M4 Shared demo suite

Build demos that use either provider, with matching C++ and Blueprint examples.

**Prerequisites:** M3.

#### E09 Shared communication and capture demos

##### SR-401 — Build the shared Echo and diagnostics demo (P1)

Build one Echo UI for text, binary data where supported, audio/video, bitrate, statistics and cleanup controls. Direct mode uses the UE server test endpoint; Janus mode uses Echo.

**Done when:** provider switching changes configuration only; tests distinguish server decode/re-encode latency from Janus packet echo. Both C++ and Blueprint examples work in packaged native clients.

**Depends on:** [SR-302](#e07-janus-signaling-and-service-adapters), [SR-305](#e08-provider-conformance-and-optional-janus-builds), [SR-205](#e06-media-lifecycle-and-unreal-playback)

##### SR-402 — Build the shared Call demo (P1)

Build calling, incoming-call notification, accept/reject, local preview, remote playback and hangup through the shared API. Show whether the other participant is the Direct server or a peer reached through Janus.

**Done when:** native Unreal client/server and Janus fixtures demonstrate bidirectional media and applicable data; the common UI/graphs contain no provider branches and recover cleanly from a remote hangup or rejected call.

**Depends on:** [SR-303](#e07-janus-signaling-and-service-adapters), [SR-305](#e08-provider-conformance-and-optional-janus-builds)

##### SR-403 — Build the shared Chat demo (P1)

Build room membership, participant display and text messaging through the shared text-room API. Add a separate raw-channel inspector for services that support generic DataChannels.

**Done when:** multiple native Unreal clients exchange authorized messages through either provider; binary tests remain distinct from TextRoom; disconnect/rejoin and stale participant removal are verified.

**Depends on:** [SR-304](#e07-janus-signaling-and-service-adapters), [SR-305](#e08-provider-conformance-and-optional-janus-builds)

##### SR-404 — Build device and capture demonstrations (P1)

Add device selection, UE render-target sources and point-to-point screen sharing to the shared capture sample. Add native screen-capture adapters where available and handle OS permissions.

**Done when:** the same source controls work against both providers; source replacement, device denial/removal and externally ended screen capture are handled. Unsupported targets do not claim capture support merely because WebRTC connects.

**Depends on:** [SR-204](#e06-media-lifecycle-and-unreal-playback), [SR-305](#e08-provider-conformance-and-optional-janus-builds)

**Epic complete when:** Provider selection changes configuration only; each demo handles its applicable connection, media, device and participant failure scenarios.

#### E10 Sample packaging and developer workflows

##### SR-405 — Package the shared demo launcher and lifecycle tests (P1)

Add reusable settings, safe credential entry, capability display and session diagnostics to the sample project. Keep sample UI assets separate from required runtime plugin content.

**Done when:** all shared maps run after packaging on declared desktop and physical Android/iOS targets; repeated map changes, mobile suspend/resume and PIE stop leave no tracks, sockets or pending callbacks alive. Mobile capture limits and advanced capabilities have explicit per-device results.

**Depends on:** [SR-401](#e09-shared-communication-and-capture-demos), [SR-402](#e09-shared-communication-and-capture-demos), [SR-403](#e09-shared-communication-and-capture-demos), [SR-404](#e09-shared-communication-and-capture-demos), [SR-306](#e08-provider-conformance-and-optional-janus-builds)

##### SR-406 — Document equivalent C++ and Blueprint workflows (P1)

Write runnable C++ snippets and annotated Blueprint walkthroughs for the same data, media, call and room flows. Include provider settings, errors and cancellation.

**Done when:** a developer can reproduce each shared demo without editing provider code, copying Janus JSON or depending on private engine headers; documented snippets are checked against the actual public API.

**Depends on:** [SR-405](#e10-sample-packaging-and-developer-workflows)

**Epic complete when:** Packaged maps survive repeated lifecycle transitions; a developer can follow the examples without provider JSON or private engine headers.

**Milestone complete when:** Shared Echo, Call, Chat and Capture demos select the provider through settings. Packaged native C++ and Blueprint examples exercise the same application behavior.

### M5 Janus SFU rooms

VideoRoom, multistream and broadcast integration.

**Prerequisites:** M3; M4 before demo acceptance.

#### E11 SFU sessions and subscription controls

##### SR-501 — Implement VideoRoom publishers and subscribers (P0)

Add typed VideoRoom join, publish, subscribe and leave operations. Give publishers and subscribers independent PeerConnections. Associate tracks with participants and support multistream subscriptions.

**Done when:** join/leave and track addition/removal work with multiple native Unreal participants; one subscriber connection can receive multiple negotiated tracks; publisher and subscriber lifetimes remain correct under concurrent events.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-204](#e06-media-lifecycle-and-unreal-playback), [SR-306](#e08-provider-conformance-and-optional-janus-builds)

##### SR-502 — Expose guarded Janus room extension APIs (P1)

Expose Janus subscription, layer and room controls in dedicated C++ and Blueprint modules selected by the Janus build profile. Reuse shared source, sink and session-statistics types.

**Done when:** extension calls reject Direct sessions explicitly; missing Janus plugins/permissions return structured errors; Janus-off builds contain no reflected extension references or extension assets.

**Depends on:** [SR-501](#e11-sfu-sessions-and-subscription-controls), [SR-002](#e01-api-and-build-contract)

##### SR-503 — Implement SFU adaptation and codec compatibility (P1)

Add supported simulcast/layer selection, renegotiation, codec matching and subscription bandwidth controls. Check native encoder support on every declared target before advertising a layer mode.

**Done when:** layer changes remain stable under bandwidth pressure; unsupported codec combinations fail clearly; tests do not assume Janus transcodes incompatible media formats.

**Depends on:** [SR-501](#e11-sfu-sessions-and-subscription-controls)

**Epic complete when:** Multiple negotiated tracks and independent connection lifetimes remain correct; layer selection and unsupported-codec errors are validated through Janus extensions.

#### E12 Room experiences and recovery

##### SR-504 — Build VideoRoom and webinar demos (P1)

Build standard and multistream video rooms, plus one-to-many screen sharing. Reuse shared capture/render components and optional Janus room controls.

**Done when:** demonstrate six native Unreal participants when test resources permit, with explicit measured limits; join/leave, mute and publisher loss update all views. Lower-resource previews document the smaller tested count.

**Depends on:** [SR-502](#e11-sfu-sessions-and-subscription-controls), [SR-503](#e11-sfu-sessions-and-subscription-controls), [SR-405](#e10-sample-packaging-and-developer-workflows)

##### SR-505 — Integrate room chat and SFU recovery (P1)

Add optional TextRoom chat and consistent room presence. Keep its session separate from VideoRoom media. Document any supported VideoRoom data forwarding separately.

**Done when:** loss/recreation of either service does not corrupt the other; identity mapping and subscription rebuild are deterministic; all client-facing controls have Blueprint equivalents.

**Depends on:** [SR-501](#e11-sfu-sessions-and-subscription-controls), [SR-304](#e07-janus-signaling-and-service-adapters), [SR-504](#e12-room-experiences-and-recovery)

**Epic complete when:** Measured multi-participant demos handle publisher loss, subscription rebuild and independent TextRoom failure without corrupting the other service.

**Milestone complete when:** VideoRoom, multistream subscriptions and broadcast demos work with shared media sinks and optional Janus extensions. Verify multiple participants, track lifecycle, layer changes and associated chat.

### M6 Janus service demos

Remaining Janus gateway, mixing, recording and administration demos.

**Prerequisites:** M3; M5 for the combined room/service release.

#### E13 Playback and audio room services

##### SR-601 — Add Streaming mountpoint demo and controls (P1)

Add typed controls to discover, watch, start and stop streaming mountpoints, using shared track playback. Include a Direct generated/live-source view to demonstrate the same receive path.

**Done when:** a controlled Janus streaming source plays in native Unreal clients; service management is Janus-only, while playback components are shared. External stream setup and negotiated codecs are documented.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-502](#e11-sfu-sessions-and-subscription-controls), [SR-206](#e06-media-lifecycle-and-unreal-playback)

##### SR-602 — Add AudioBridge mixing demo (P1)

Add AudioBridge membership and supported mixing controls through typed Janus C++/Blueprint APIs. Reuse shared audio sources and sinks.

**Done when:** multiple participants hear the configured mixed output; mute/leave/device loss and room failure are handled; the UI identifies server mixing and does not claim Direct-mode audio-room parity.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-502](#e11-sfu-sessions-and-subscription-controls), [SR-201](#e05-native-media-sources-and-devices), [SR-203](#e05-native-media-sources-and-devices)

##### SR-603 — Add Record and Play demo (P1)

Add recording, authorized recording-catalog access and playback through Janus Record/Play. Handle SDP offers sent by the service during playback.

**Done when:** a recorded audio/video fixture can be replayed by native Unreal clients; access and cleanup are tested. Record data-channel support only if verified against the pinned Janus version, rather than assuming all recording modes are equivalent.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-502](#e11-sfu-sessions-and-subscription-controls), [SR-207](#e06-media-lifecycle-and-unreal-playback)

**Epic complete when:** Controlled fixtures demonstrate streaming, mixed audio and recorded playout with authorization, device/session lifecycle and codec evidence.

#### E14 Gateway services and operator tooling

##### SR-604 — Add SIP Gateway demo (P2)

Add typed SIP registration, call, accept and hangup controls, with relevant errors, through Janus. Use an isolated test SIP service and provide C++/Blueprint sample controls.

**Done when:** complete a call against the controlled fixture; rejection, busy and credential failures are actionable. The plugin does not bundle a SIP server or expose privileged credentials in distributable sample assets.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-502](#e11-sfu-sessions-and-subscription-controls), [SR-207](#e06-media-lifecycle-and-unreal-playback)

##### SR-605 — Add NoSIP interoperability demo (P2)

Add NoSIP session controls and instructions for an external SDP/RTP test endpoint. Reuse media rendering/playback and document the external signaling explicitly.

**Done when:** native Unreal clients exchange media with the controlled external peer through Janus; incompatible SDP and endpoint shutdown fail clearly.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-502](#e11-sfu-sessions-and-subscription-controls), [SR-207](#e06-media-lifecycle-and-unreal-playback)

##### SR-606 — Add development Admin and Monitor tooling (P2)

Add an optional development/operator module for selected Janus status and monitoring operations, using shared correlation IDs. Document operator authentication and keep credentials outside game content.

**Done when:** inspect the controlled Janus fixture; administrative code/assets are excluded from ordinary Shipping client packages. Supply equivalent C++ and operator/editor Blueprint examples in the optional development module, with no admin credentials in player builds.

**Depends on:** [SR-301](#e07-janus-signaling-and-service-adapters), [SR-502](#e11-sfu-sessions-and-subscription-controls)

**Epic complete when:** Controlled SIP/RTP peers exchange media with clear error handling; operator tools inspect Janus while administration code and credentials stay out of player packages.

**Milestone complete when:** Streaming, AudioBridge, Record/Play, SIP, NoSIP and development-only Admin/Monitor each have a typed extension API, an external test setup and a working demo. Client-facing controls also have Blueprint access.

### M7 Advanced media demos

Processing, background effects, multichannel audio and E2EE examples.

**Prerequisites:** M4 and M5.

#### E15 Media transforms and effects

##### SR-701 — Define bounded audio and video transform hooks (P1)

Define native video-frame and audio processing hooks. Specify timestamps, formats, thread ownership, latency limits and queue limits. Keep effect dependencies outside the core API.

**Done when:** a deterministic test transform runs before publication on both providers; dropped frames and stalled processing do not block the game thread; effect enable/disable is exposed consistently to C++ and Blueprints.

**Depends on:** [SR-204](#e06-media-lifecycle-and-unreal-playback), [SR-206](#e06-media-lifecycle-and-unreal-playback), [SR-305](#e08-provider-conformance-and-optional-janus-builds)

##### SR-702 — Add audio processing demo (P2)

Adapt the Janus Web Audio Processing demo into a simple effect on native UE audio sources. Document where processing occurs relative to echo cancellation.

**Done when:** the processed signal is heard at the remote endpoint through both providers; switching effects and devices is stable; the Unreal sample documents its native processing API and measured behavior.

**Depends on:** [SR-701](#e15-media-transforms-and-effects), [SR-201](#e05-native-media-sources-and-devices), [SR-203](#e05-native-media-sources-and-devices)

##### SR-703 — Add virtual background demo (P2)

Add optional camera segmentation and background compositing. Keep model/effect downloads, licenses and runtime costs separate from the base plugin.

**Done when:** a supported native path publishes the composited result through the common API; measure processing cost and fallback behavior; missing models or unsupported devices return an explicit capability state.

**Depends on:** [SR-701](#e15-media-transforms-and-effects), [SR-404](#e09-shared-communication-and-capture-demos)

**Epic complete when:** Actual processed media reaches the remote endpoint; effect changes, stalled processing and unavailable models/devices have measured, bounded behavior.

#### E16 Advanced codec and encryption capabilities

##### SR-704 — Add multichannel Opus demo (P2)

Check the selected desktop/mobile codec factories and native SDP/audio paths for multichannel Opus. On supported paths, negotiate channel layouts and add a generated surround-audio test source.

**Done when:** validate channel mapping and actual output on a declared supported path with both providers where possible. Unsupported codec-build/platform cases remain open gaps or require an explicit design revision; ordinary stereo success does not close this issue.

**Depends on:** [SR-201](#e05-native-media-sources-and-devices), [SR-203](#e05-native-media-sources-and-devices), [SR-305](#e08-provider-conformance-and-optional-janus-builds)

##### SR-705 — Design and implement optional media E2EE (P2)

Check encoded-frame encryption hooks in the qualified, separately built libwebrtc for the declared client and Server targets. Before implementation, define an interoperable authenticated frame format, key provisioning outside Janus, key rotation, replay handling and a threat model.

**Done when:** key-holding endpoints exchange media while Janus cannot decode it; wrong keys and rotation are tested; cryptographic review is recorded. DTLS-SRTP alone and insecure demo ciphers do not count as E2EE. Missing hooks remain explicit target limitations.

**Depends on:** [SR-701](#e15-media-transforms-and-effects), [SR-501](#e11-sfu-sessions-and-subscription-controls)

##### SR-706 — Package advanced demos and capability evidence (P2)

Add advanced sample maps, widgets and C++ examples. Show provider/platform capabilities and prerequisites. Keep provider selection shared wherever behavior is the same.

**Done when:** each advanced demo has working evidence for its declared supported path; unsupported combinations are individually listed. A disabled UI alone does not count as completing a demo or all-platform support.

**Depends on:** [SR-702](#e15-media-transforms-and-effects), [SR-703](#e15-media-transforms-and-effects), [SR-704](#e16-advanced-codec-and-encryption-capabilities), [SR-705](#e16-advanced-codec-and-encryption-capabilities), [SR-405](#e10-sample-packaging-and-developer-workflows)

**Epic complete when:** Channel mapping and encrypted-media tests provide real evidence for declared paths; advanced demos publish individual capability results and unresolved limitations.

**Milestone complete when:** Device and capture support enables transforms, audio processing, virtual backgrounds, multichannel Opus and E2EE on the declared paths. Publish measured results and available capabilities; list unsupported paths as gaps.

### M8 Production hardening

Predictable recovery, bounded resources and reproducible supported-platform packages.

**Prerequisites:** M1 to M7, incrementally.

#### E17 Runtime resilience and security

##### SR-801 — Validate ICE relay and recovery behavior (P0)

Test direct and forced-TURN routes, reachable TURN transport variants, packet loss/reordering, blocked UDP, mobile Wi-Fi/cellular transitions and suspend/resume, ICE restart and signaling reconnection.

**Done when:** supported recovery policies are bounded and observable; unrecoverable sessions fail clearly. Native integration tests distinguish reconnecting a signaling transport from recovering media and Janus subscriptions.

**Depends on:** [SR-305](#e08-provider-conformance-and-optional-janus-builds), [SR-505](#e12-room-experiences-and-recovery)

##### SR-802 — Harden admission and signaling boundaries (P0)

Check credentials, session/room permissions, signaling size/rate limits, malformed SDP/JSON, client origins where applicable and log redaction. Define integration with short-lived TURN credentials and service-specific permissions.

**Done when:** unauthorized cross-session/cross-room operations fail; invalid inputs do not crash the plugin; client packages contain no admin secrets or permanent TURN passwords; authentication is not inferred from a display name.

**Depends on:** [SR-104](#e04-unreal-server-and-client-integration), [SR-301](#e07-janus-signaling-and-service-adapters), [SR-505](#e12-room-experiences-and-recovery)

##### SR-803 — Measure performance and resource stability (P1)

Measure audio/video latency, CPU, memory, copies, dropped frames, queue pressure and multi-session limits, plus Android/iOS battery and thermal behavior on physical devices. Optimize measured bottlenecks; reduce texture copies or share encoders only where safe.

**Done when:** record repeatable baselines and budgets per supported target; a 30-minute mixed-session soak and repeated connect/disconnect cycles show bounded queues/resources. No universal latency, zero-copy or participant-count claim is made from one host.

**Depends on:** [SR-405](#e10-sample-packaging-and-developer-workflows), [SR-504](#e12-room-experiences-and-recovery), [SR-701](#e15-media-transforms-and-effects)

**Epic complete when:** Relay/loss/recovery and malformed-input tests pass; authorization boundaries hold and measured soak tests keep queues and resources bounded.

#### E18 Build validation and diagnosability

##### SR-804 — Build the engine and platform validation matrix (P1)

Automate standard UE 5.8 desktop client/Server builds and Android/iOS ARM64 client packaging and smoke tests. Add Linux-hosted Android dependency builds and macOS/Xcode iOS builds with physical-device runs; provision Windows/Linux runtime jobs when runners are available. Document a future UE 5.9 adapter review.

**Done when:** every supported matrix cell records versions and artifacts. Mac validation cannot close Windows/Linux or mobile runtime coverage; missing runners/devices remain Unverified. Android/iOS are required planned client targets; consoles remain Future.

**Depends on:** [SR-306](#e08-provider-conformance-and-optional-janus-builds), [SR-405](#e10-sample-packaging-and-developer-workflows), [SR-504](#e12-room-experiences-and-recovery), [SR-006](#e02-native-feasibility-and-test-fixtures), [SR-007](#e02-native-feasibility-and-test-fixtures), [SR-208](#e06-media-lifecycle-and-unreal-playback)

##### SR-805 — Verify final feature profiles and package contents (P0)

Compile, cook and package Full, Direct-only, Janus-only and data-only profiles on desktop and Android/iOS ARM64, plus Server on desktop/server hosts. Check descriptors, runtime/native-library dependencies, reflected classes and sample asset references.

**Done when:** disabled features are absent from the intended module/content graph; standard UE requires no engine-source patches; Server has no accidental RHI/editor requirement; Janus administration never ships in normal player packages. Verify one libwebrtc copy per target, correct ARM64 device payloads, and separate diagnostic/simulator artifacts; use SR-807 for app-size acceptance.

**Depends on:** [SR-002](#e01-api-and-build-contract), [SR-306](#e08-provider-conformance-and-optional-janus-builds), [SR-606](#e14-gateway-services-and-operator-tooling), [SR-706](#e16-advanced-codec-and-encryption-capabilities)

##### SR-806 — Finish debugging and crash diagnostics (P1)

Add structured session/thread traces, guidance on native debug symbols and useful connection/media failure reports. Review shutdown and cleanup around crashes; do not attempt unsafe recovery within the same process.

**Done when:** reproduce and diagnose representative native client/Server failures through Direct and Janus providers using documented logs; callbacks are traceable across engine and libwebrtc threads and sensitive fields remain redacted by default.

**Depends on:** [SR-405](#e10-sample-packaging-and-developer-workflows), [SR-505](#e12-room-experiences-and-recovery), [SR-801](#e17-runtime-resilience-and-security)

##### SR-807 — Optimize ARM64 builds and validate mobile app size (P1)

Apply the [mobile optimization and measurement procedure](docs/MOBILE_CLIENTS.md#app-size-validation) to pinned release artifacts. Evaluate codec selection, supported compiler/linker size options and dead-code elimination while keeping symbols, optional effects and models separate. Compare matching UE Shipping apps with RTC off/on for Full, Direct-only, Janus-only and data-only profiles.

**Done when:** reports record baseline/enabled bytes and deltas for device-specific Android delivery and clean installed size, and iOS app-thinning compressed/uncompressed estimates; distinguish dependency/archive sizes and record methods, devices, manifests and checksums. Establish numeric per-platform/profile budgets and CI regression checks; each optimized configuration passes its declared data/media, Janus/Direct, CPU/thermal and fallback tests. Unsupported variants remain explicit; vendor figures do not count as measurements.

**Depends on:** [SR-006](#e02-native-feasibility-and-test-fixtures), [SR-007](#e02-native-feasibility-and-test-fixtures), [SR-305](#e08-provider-conformance-and-optional-janus-builds), [SR-803](#e17-runtime-resilience-and-security), [SR-804](#e18-build-validation-and-diagnosability), [SR-805](#e18-build-validation-and-diagnosability)

**Epic complete when:** Declared desktop and Android/iOS targets have build/runtime evidence; package contents match selected features, mobile app-size reports meet recorded budgets, and representative failures can be traced through engine and libwebrtc threads.

**Milestone complete when:** Every declared engine/platform and applicable build profile passes its tests for loss, relay, reconnect, admission, lifecycle, resource limits, performance, diagnostics and packaging. Android/iOS ARM64 clients also pass physical-device and app-size gates with reproducible dependency artifacts.

### M9 Documentation and release

Package a plugin that developers can install and use in their own projects.

**Prerequisites:** M8.

#### E19 Documentation and source provenance

##### SR-901 — Write installation and sample guides (P1)

Document standard UE plugin installation, native client/Server builds and packaging, Janus/TURN test setups and every demo. Include the capability matrix, matching C++/Blueprint quick starts, Android/iOS artifact/toolchain setup, permissions/signing and app-size reproduction instructions.

**Done when:** a clean checkout follows the guides without engine edits or private developer paths; a new Unreal project can install the plugin, package a native sample and connect to the documented endpoint.

**Depends on:** [SR-406](#e10-sample-packaging-and-developer-workflows), [SR-601](#e13-playback-and-audio-room-services), [SR-602](#e13-playback-and-audio-room-services), [SR-603](#e13-playback-and-audio-room-services), [SR-604](#e14-gateway-services-and-operator-tooling), [SR-605](#e14-gateway-services-and-operator-tooling), [SR-606](#e14-gateway-services-and-operator-tooling), [SR-706](#e16-advanced-codec-and-encryption-capabilities), [SR-807](#e18-build-validation-and-diagnosability)

##### SR-902 — Record source provenance and notices (P1)

**Owner: SimurghRTC release maintainer.** The [SR-003 license review](docs/DEPENDENCY_REVIEW.md#license-and-distribution-review) found missing external OpenSSL/Opus/Speex notices in mobile artifacts, a missing simulator VPX patent file and no final-app notice validation. These block publication until resolved.

Maintain third-party notices, source origins, generated-asset licenses and a dependency inventory, including separately built mobile libwebrtc revisions, patches, codecs and binary provenance. Keep Apache 2.0 for the repository and document the separate engine and dependency obligations.

**Done when:** public sources contain no copied restricted engine implementation or unapproved binary redistribution; Janus is documented as an external service; sample/effect assets have recorded distribution rights.

**Depends on:** [SR-003](#e02-native-feasibility-and-test-fixtures), [SR-703](#e15-media-transforms-and-effects), [SR-705](#e16-advanced-codec-and-encryption-capabilities), [SR-006](#e02-native-feasibility-and-test-fixtures), [SR-007](#e02-native-feasibility-and-test-fixtures)

**Epic complete when:** A clean project follows the native setup guides; public sources and sample/effect assets have recorded provenance and applicable notices.

#### E20 Release qualification and packaging

##### SR-903 — Run complete demo and API regression (P0)

Run all applicable shared, provider and extension tests and every demo-matrix entry against the pinned environment. Check C++/Blueprint behavior, disabled features, physical Android/iOS results, mobile app-size budgets and unsupported-target reporting.

**Done when:** publish an evidence matrix with Passed/Failed/Unverified/Unsupported classifications and reasons; remaining advanced or host gaps prevent claiming complete requested-scope coverage.

**Depends on:** [SR-801](#e17-runtime-resilience-and-security), [SR-802](#e17-runtime-resilience-and-security), [SR-803](#e17-runtime-resilience-and-security), [SR-804](#e18-build-validation-and-diagnosability), [SR-805](#e18-build-validation-and-diagnosability), [SR-806](#e18-build-validation-and-diagnosability), [SR-901](#e19-documentation-and-source-provenance), [SR-807](#e18-build-validation-and-diagnosability)

##### SR-904 — Prepare the distributable plugin release (P1)

Prepare versioned packages, checksums, release notes, an API migration policy and reproducible build instructions. Include versioned Android/iOS ARM64 dependency artifacts, matching headers/helpers, separate diagnostic symbols, build manifests and app-size reports. Verify installation as a project plugin on standard UE without requiring consumers to build WebRTC.

**Done when:** release artifacts match documented profiles and evidence; a new project can run the supported samples. Publishing a marketplace listing, hosting servers or changing repositories is not part of this issue unless separately requested.

**Depends on:** [SR-903](#e20-release-qualification-and-packaging), [SR-902](#e19-documentation-and-source-provenance), [SR-008](#e02-native-feasibility-and-test-fixtures)

**Epic complete when:** Release evidence records each target and capability outcome; packages match selected profiles, checksums and reproducible build instructions.

**Milestone complete when:** Plugin packages are reproducible. Installation guides, sample walkthroughs, notices, capability/support matrices and release test records are complete. Verify installation and demos in a clean project.

## 5. Decisions

| ID | Decision |
|---|---|
| ADR-001 | C++ owns the public API, session state, native libwebrtc integration and provider protocol logic. Client-facing operations also have Blueprint interfaces. |
| ADR-002 | Both providers use the same native libwebrtc backend. Provider selection changes signaling and service behavior: Direct Unreal connects to the plugin's Unreal server endpoint; Janus connects to the selected Janus service. |
| ADR-003 | Direct Unreal mode terminates WebRTC on the Unreal server. It is not a UDP proxy, an SFU or a replacement for gameplay replication. |
| ADR-004 | Data is delivered through DataChannels; audio and video through WebRTC media tracks. Signaling is separate from both. |
| ADR-005 | Libwebrtc supplies DataChannels and media transport through separately built, pinned dependencies on desktop clients/servers and Android/iOS ARM64 clients. Approved 2026-10-19 under SR-008; migration is in progress. Each target links one WebRTC implementation; its crypto/codec symbols must be isolated from Unreal dependencies without engine source patches. Existing engine-backed probes remain feasibility evidence until replacements pass validation. |
| ADR-006 | Janus-specific functionality is isolated behind `SIMURGHRTC_WITH_JANUS` and optional provider/extension modules. Runtime capabilities also govern what an enabled backend can actually do. |
| ADR-007 | Shared operations have common semantics; unavailable services return an explicit capability/error result. Switching provider reconnects the session; it is not a transparent migration of a live PeerConnection. |
| ADR-008 | WebRTC may own its internal threads. Application callbacks are marshalled to the correct Unreal thread, with bounded audio/video queues. No new generic networking event-loop framework is part of this plugin. |
| ADR-009 | Janus remains the only SFU integration. Its mixers, gateways, recording and administration are also Janus extensions, even though they are not SFU features. |
| ADR-010 | Mobile clients reuse the shared API and private platform/version adapters. Dependency artifacts are built separately on pinned Linux/Android and macOS/Xcode hosts, with ARM64 release payloads and explicit codec/fallback capabilities. Mobile Server targets are excluded. |
| ADR-011 | Mobile app-size budgets derive from matching Shipping builds with RTC on/off. Record device-specific delivery and installed/uncompressed size deltas separately from developer artifacts, and enforce regression checks without silently removing supported features. |

See [Architecture](docs/ARCHITECTURE.md) for the API and build boundaries.

## 6. Engine and platform matrix

| Target | Planned coverage | Validation gate |
|---|---|---|
| Standard UE 5.8, macOS | Editor, packaged native client and native Server target | First implementation and runtime gate on the available Mac |
| Standard UE 5.8, Windows x64 | Native client and Server target | Build and run on a real Windows host/runner before declaring support |
| Standard UE 5.8, Linux x64 | Native client and headless Server target | Build and run on a real Linux host/runner before declaring support |
| Standard UE 5.8, Android ARM64 | Packaged native Unreal client; separately built libwebrtc; Full, Direct-only, Janus-only and data-only profiles | Linux-hosted dependency build, physical Android data/media and lifecycle tests, provider conformance and app-size reports before declaring support |
| Standard UE 5.8, iOS ARM64 | Packaged native Unreal client; separately built libwebrtc; Full, Direct-only, Janus-only and data-only profiles | macOS/Xcode dependency build, signed physical iPhone/iPad data/media and lifecycle tests, provider conformance and app-size reports before declaring support |
| UE 5.9 | Future compatibility port | Separate adapter audit and regression pass when the target engine is available |
| Other mobile architectures and consoles | Future evaluation | Separate SDK, capture/codec, hardware and platform-policy work; no inherited support claim |

The available test machine is a Mac. Android dependency builds use the pinned Linux container; iOS dependencies use the pinned Xcode SDK. Samsung lifecycle/local-media tests and ARM64 iOS simulator lifecycle/codec tests pass with Janus on/off using M155. Physical iPhone testing is deferred, and independent build hosts are still needed. Record the engine revision and toolchains for each result; Server targets may need a source build. Broader Android/iOS runtime support remains Unverified until its gates pass. Simulator results do not establish physical iPhone behavior; Windows/Linux runtime coverage also requires its own hosts. See [Mobile clients](docs/MOBILE_CLIENTS.md) for dependency manifests, codec optimization and size measurement.

## 7. Release gates

- **API:** Blueprint and C++ tests cover lifecycle, cancellation, timeouts, late callbacks and structured errors. Common examples contain no Janus-specific payloads or identifiers in application logic.
- **Direct Unreal:** Server accepts multiple independent clients, isolates their sessions, receives and sends real media/data, and releases sessions without an Editor, camera or physical audio device.
- **Janus:** Integration tests run against a pinned, controlled Janus instance. Public demo servers are for manual comparison, not CI dependencies.
- **Data:** Verify binary integrity, negotiated message limits, ordering/reliability and backpressure. Janus capabilities are measured per service; TextRoom is not treated as a generic binary/unreliable channel.
- **Media:** Verify codec negotiation, audio continuity, track replacement, resolution changes, end-of-track cleanup and sustained A/V playback. Measure CPU, copies, memory and latency before assigning budgets.
- **Native lifecycle:** Capture permissions, device changes, application suspend/resume, PIE stop and world/process teardown are exercised. Game, audio, render and libwebrtc thread ownership is explicit; unsupported capture or codec paths produce actionable errors.
- **Build:** Full, Direct-only, Janus-only and data-only profiles compile, cook and package on desktop and Android/iOS ARM64 clients; Server is validated on desktop/server hosts. The generated descriptor, selected library/headers, dependencies, assets and macros agree.
- **Mobile:** Separate Android/iOS builds reproduce pinned libwebrtc artifacts and link one implementation per app. Physical-device tests cover common C++/Blueprint data/media flows, Direct/Janus interoperability, permissions, audio routes/interruptions, codec fallback, thermal/battery behavior and bounded suspend/resume/network recovery.
- **App size:** Matching Shipping apps with RTC off/on publish absolute and incremental device-specific delivery and installed/uncompressed sizes per mobile client profile, with manifests and measurement methods. Budgets are established from these baselines and regression checks pass; SDK archives, symbols and simulator builds are reported separately.
- **Security:** No embedded Janus administration secret or long-lived TURN credential in a client package. Admission, room authorization and signaling validation are tested. DTLS-SRTP transport encryption is distinguished from optional application E2EE.
- **Distribution:** Engine/private library binaries and Janus server code are not copied into the public plugin repository as a shortcut. Separately built upstream mobile libraries are distributed as permitted versioned release artifacts with reproducible manifests, checksums, notices and source/patch provenance; symbols remain separate from device payloads.

## 8. Status

| Milestone | Epics | Issues | Status |
|---|---|---|---|
| [M0 Foundation and feasibility](#m0-foundation-and-feasibility) | E01–E02 | SR-001–SR-008 | In progress |
| [M1 Direct Unreal data](#m1-direct-unreal-data) | E03–E04 | SR-101–SR-107 | Planned |
| [M2 Direct Unreal media](#m2-direct-unreal-media) | E05–E06 | SR-201–SR-208 | Planned |
| [M3 Janus provider parity](#m3-janus-provider-parity) | E07–E08 | SR-301–SR-306 | Planned |
| [M4 Shared demo suite](#m4-shared-demo-suite) | E09–E10 | SR-401–SR-406 | Planned |
| [M5 Janus SFU rooms](#m5-janus-sfu-rooms) | E11–E12 | SR-501–SR-505 | Planned |
| [M6 Janus service demos](#m6-janus-service-demos) | E13–E14 | SR-601–SR-606 | Planned |
| [M7 Advanced media demos](#m7-advanced-media-demos) | E15–E16 | SR-701–SR-706 | Planned |
| [M8 Production hardening](#m8-production-hardening) | E17–E18 | SR-801–SR-807 | Planned |
| [M9 Documentation and release](#m9-documentation-and-release) | E19–E20 | SR-901–SR-904 | Planned |
