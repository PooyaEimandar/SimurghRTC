# Mobile Direct-data checks

SR-107 runs the same [C++ and Blueprint data maps](DATA_EXAMPLES.md) on Android ARM64 and the iOS ARM64 simulator, against a packaged Unreal 5.8.3 dedicated Server. The mobile libraries include the current M155 DataChannel bridge and remain separate from the Mac library.

Packaged data/lifecycle checks, local HTTPS/TURN, reconnect, controlled network-loss recovery and app background/resume pass. Channel options, message limits, packet loss/reordering, partial-reliability expiry and multi-channel queue recovery pass too. Physical iOS testing and Wi-Fi/cellular transitions remain open.

## Build the packages

Follow [Native backend](NATIVE_BACKEND.md) to rebuild the Android, iOS device and iOS simulator libraries. Build matching Mac, Android, iOS device and iOS simulator artifacts for the current bridge. Use the Mac artifact for the Editor and Server. Earlier artifacts remain historical evidence and do not match the new ICE exports.

Use [Mobile probe](MOBILE_PROBE.md) to stage and package **Direct-only** and **Full** projects. Include both maps in each client cook:

```sh
-map="/Engine/Maps/Entry+/Game/Examples/Direct/Maps/RtcDataCpp+/Game/Examples/Direct/Maps/RtcDataBlueprint"
```

The Server uses the `server` profile. The runner checks package identity, the selected native artifact and the actual actor loaded from each cooked map.

## Run the sample checks

For the iOS simulator:

```sh
python3 tools/run_mobile_data_example_probe.py \
  --platform IOSSimulator --device "$SIMULATOR_UDID" \
  --client-project "$CLIENT_PROJECT" --app "$SIMULATOR_APP" \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --output Intermediate/MobileDataSimulator
```

For physical Android, use `--platform Android`, an authorized `adb` serial in `--device`, and the packaged APK in `--app`. Run each profile separately with a fresh output directory. The runner installs only the selected sample app and stops its own Server process.

Each map runs five cases: exact Unicode/binary echoes, Connect timeout, Disconnect during Connect, actor destruction during Connect, and actor destruction during OpenChannel. Reports check operation IDs, game-thread callbacks, immediate EndPlay shutdown, actual object collection, native closure and no late callbacks. Temporary credentials are kept in private app storage and removed afterward.

The simulator uses loopback ICE. Android uses normal network ICE, so the phone and Mac need a usable network path. An automatically allocated `adb reverse` port carries only the development HTTP signaling; WebRTC carries the messages. The runner removes only its own reverse ports. These checks use authenticated loopback HTTP, and do not establish HTTPS or TURN support.

## Run HTTPS and TURN checks

The shared config now accepts STUN/TURN settings. Supply `IceServers` at runtime and set `bRelayOnly` to require a relay. The Server has its own ICE settings. See [API configuration](API_CONTRACT.md#configuration-and-topology) for formats and limits.

Build the local Mac TURN fixture from the existing coturn source pin:

```sh
python3 tools/build_macos_turn_fixture.py --output LocalBuilds/TurnFixture
```

This fixture records the installed Mac compiler, OpenSSL and libevent libraries. It does not install a service or change system trust. The existing Docker fixture remains available for the fully pinned service stack.

Then run the network suite for each profile:

```sh
python3 tools/run_mobile_network_probe.py \
  --platform IOSSimulator --device "$SIMULATOR_UDID" \
  --client-project "$CLIENT_PROJECT" --app "$SIMULATOR_APP" \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --turn-fixture LocalBuilds/TurnFixture/fixture.json \
  --output Intermediate/MobileNetworkSimulator
```

For Android, use `--platform Android`, the phone's adb serial, the APK path, and `--listen-ip "$MAC_LAN_IP"`. The phone must reach that Mac address. The runner uses adb reverse only for HTTPS; TURN carries the WebRTC data over the local network.

Each suite checks C++ and Blueprint text/binary echoes over HTTPS with direct ICE, TURN/UDP and TURN/TCP. It verifies selected relay candidates on both ends, then checks invalid signaling credentials, an untrusted certificate and a certificate for the wrong hostname. Certificate failures must happen before signaling reaches the Server. Non-timeout HTTP failures report `InternalError`; actual deadlines report `Timeout`.

The simulator run creates and deletes its own simulator, including its temporary CA trust. Android receives a CA file only inside the sample app for that run; device trust settings stay unchanged. Temporary keys, credentials, owned adb ports, listeners and processes are cleaned up.

The local TURN fixture uses relay sockets on the Mac's loopback interface, and both peers must use the relay. These tests do not establish Internet NAT traversal, blocked-UDP fallback, TURN/TLS, or mobile network transitions.

## Run reconnect and loss recovery checks

Use the same arguments as the network suite with `tools/run_mobile_recovery_probe.py` and a fresh output directory. Rebuild the sample packages first so they contain the current C++ flow and regenerated Blueprint graph.

Each map runs four cases: graceful reconnect over direct ICE, graceful reconnect through TURN/UDP, loss of HTTPS signaling, and loss of the TURN relay. Each case exchanges different text/binary payloads in two rounds. It keeps the session ID, opens a new channel, rejects the old channel handle and checks final native cleanup.

For loss cases, the runner interrupts only its local HTTPS proxy or TURN process. It waits for `Failed`, reads the structured connection error and checks that game-thread ticks continue for at least three seconds before restoring the fixture. The sample then calls Connect again through its own C++ code or Blueprint node. This checks explicit reconnection after a controlled outage; Wi-Fi/cellular handover and ICE restart require separate tests.

## Run background and resume checks

Use the network suite's arguments with `tools/run_mobile_background_probe.py`. The runner sends the Android sample to Home, or switches the disposable iOS simulator to Settings, then brings the same process back. It uses real OS events; it does not broadcast test lifecycle events inside Unreal.

Each C++/Blueprint map tests a connected session over direct ICE and TURN/UDP, plus a Connect held pending at the HTTPS fixture. The sample calls Shutdown on deactivation, before suspension, and creates a fresh session on reactivation. It also records the actual background and foreground events. Reports check silent cancellation, old Server-session cleanup, new session/channel IDs, exact messages after resume, object collection and final native closure. The runner keeps the app away for at least eight seconds. Game-thread timing excludes that intentional suspension interval.

These checks cover an app switch with the process preserved. Screen lock, forced termination, low-memory eviction and physical iOS behavior require separate validation. The sample makes no background-networking guarantee.

## Run ordering, limits and backpressure checks

Use the network suite's arguments with `tools/run_mobile_channel_probe.py`. Rebuild the Server and mobile packages first, including the regenerated Blueprint graph. Run Direct-only and Full separately.

Each C++/Blueprint map runs all six ordering/reliability combinations over verified local HTTPS. It checks the options at both peers, exact Unicode/binary echoes, empty messages, a 32-message burst and the 64 KiB message limit. Ordered channels must preserve sequence; unordered channels must preserve the message set. Text limits are checked in UTF-8 bytes, including multibyte characters.

Reliable channels also fill the 256 KiB send buffer and the 64-message queue, require `ResourceExhausted` for the rejected send, then retry after the queue drains. The rejected payload must arrive exactly once after retry. The checks also mutate caller-owned payloads after admission to verify that the provider copied them.

Two additional cases per map repeat the reliable ordered flow through TURN/UDP and with a lower 32 KiB negotiated limit. The lower-limit fixture changes only the SDP `max-message-size` attribute in both directions; this is test setup, not a production signaling feature. Reports require matching remote limits, bounded buffered amounts, responsive game-thread ticks and complete cleanup.

This suite checks configured options and exact delivery on a healthy local path. The impairment suite below checks behavior when packets are dropped or reordered. Unordered delivery is allowed to arrive in order.

## Run packet loss, expiry and multi-channel checks

Use the network suite's arguments with `tools/run_mobile_impairment_probe.py`. Rebuild the Server and both mobile profiles, including the regenerated Blueprint graph. Each C++/Blueprint map runs seven cases through local TURN/UDP:

| Cases | What must happen |
|---|---|
| Reliable, ordered and unordered | All 33 messages return exactly once while the fixture drops every seventh eligible packet and reverses adjacent packet pairs. Ordered messages preserve sequence; unordered messages preserve the set. |
| No retransmits, ordered and unordered | Eight messages admitted during a three-second data blackout stay absent after traffic resumes. Fresh text and binary messages arrive. |
| 100 ms lifetime, ordered and unordered | The same blackout expires eight old messages. Fresh messages still arrive, with no old or duplicate echoes during a ten-second quiet interval. |
| Five reliable channels | Four channels fill the shared 1 MiB send queue. A send on the empty fifth channel returns `ResourceExhausted`; its exact retry and messages on all five channels arrive after the queue drains. |

A bounded UDP proxy sits between the client and the local TURN fixture. It changes only traffic using DTLS application keys after the channel opens (including SCTP control packets); HTTPS signaling, TURN control, ICE checks and the DTLS handshake continue. The runner starts impairment only after the channels open and records actual packet drops and reordered pairs. It changes no system firewall or network settings.

Both partial-reliability cases wait ten seconds after fresh messages arrive to catch late delivery. These checks test SCTP message lifetime and retransmission settings; the plugin's separate 30-second send-queue deadline remains untested. Packet order at the proxy does not require unordered application messages to arrive out of order. The reports also check channel options at the Server, per-channel and aggregate buffer limits, game-thread responsiveness, explicit channel closes and native peer cleanup.

## Check the native libraries

The standalone fixture checks all six ordering/reliability combinations, exact UTF-8/binary payloads, empty messages, 32/64 KiB negotiated limits, ordered bursts and bounded receive overflow:

```sh
python3 tools/run_native_data_probe.py \
  --artifact "$MOBILE_ARTIFACT" --device "$DEVICE_ID" \
  --output Intermediate/MobileNativeData
```

Use an Android serial or an iOS simulator UDID to match the artifact. Android requires a physical ARM64 device. This fixture exercises the native library directly; it does not replace packaged shared-API tests. The iOS device library can separately receive the existing `run_native_abi_probe.py --link-only` check without claiming a device run.

## Evidence and remaining work

These suites passed on **2026-10-29–30**, using the Galaxy S25 Ultra (Android 17) and ARM64 iOS 27.0 simulator. Each covers Direct-only and Full packages through the actual C++ and Blueprint maps.

| Suite | Cases | Main result | Build details |
|---|---:|---|---|
| Basic data and lifecycle | 40 | Exact messages, owner destruction, cancellation and cleanup | [Initial record](reviews/SR107-mobile-data-validation.json) |
| HTTPS and TURN | 36 | Verified HTTPS, UDP/TCP relays and expected certificate/authentication rejections | [Network record](reviews/SR107-mobile-network-validation.json) |
| Reconnect and loss recovery | 32 | 128 exact round trips after reconnect or controlled signaling/relay loss | [Recovery record](reviews/SR107-mobile-recovery-validation.json) |
| Background and resume | 24 | 80 exact round trips and eight silent pending-Connect cancellations through real app switches | [Background record](reviews/SR107-mobile-background-validation.json) |
| Channel options, limits and queues | 64 | 4,704 exact round trips, 256 expected rejected sends and 64 native client closures | [Channel record](reviews/SR107-mobile-channel-validation.json) |
| Packet impairment and shared queues | 56 | 768 exact round trips, 256 old messages discarded, eight recoveries from the shared 1 MiB limit | [Impairment record](reviews/SR107-mobile-impairment-validation.json) |

The latest impairment suite passed **16 reliable loss/reordering cases, 32 partial-reliability blackout cases and eight five-channel pressure cases**. It recorded 688 dropped packets and 970 reordered packet pairs. Every case also forced an authenticated Server-close race and required Disconnect to succeed. All 88 client channels closed explicitly, all 56 native clients shut down, and no late callbacks or pending cleanup remained. The largest active game-thread tick gap was 304 ms.

The final packages also passed 52 regressions: 20 lifecycle cases and 32 earlier channel cases. All 29 Unreal tests, 211 Python tests and five package audits passed. Native WebRTC artifacts were reused unchanged; the Server and mobile samples were rebuilt. The earlier channel record retains its original 64-case results and build details.

Records contain package/library hashes, device identity, build checkout states, case results and cleanup. These are local Development builds; their absolute package sizes are not Shipping download sizes or measured RTC size deltas. Earlier suites used their own recorded packages.

M1 remains in progress because physical iOS acceptance is deferred. Screen lock, OS termination, Wi-Fi/cellular transitions, ICE restart and the separate plugin send-queue deadline still need validation. The completed impairment checks cover the controlled local conditions described above.
