# Mobile Direct-data checks

SR-107 runs the same [C++ and Blueprint data maps](DATA_EXAMPLES.md) on Android ARM64 and the iOS ARM64 simulator, against a packaged Unreal 5.8.3 dedicated Server. The mobile libraries include the current M155 DataChannel bridge and remain separate from the Mac library.

Basic data/lifecycle checks, local HTTPS/TURN, explicit reconnect, controlled signaling/relay loss recovery and app background/resume now pass. Broader packaged reliability/backpressure checks and Wi-Fi/cellular transitions remain open. Physical iOS testing remains deferred; simulator results do not close that device gate.

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

## Check the native libraries

The standalone fixture checks all six ordering/reliability combinations, exact UTF-8/binary payloads, empty messages, 32/64 KiB negotiated limits, ordered bursts and bounded receive overflow:

```sh
python3 tools/run_native_data_probe.py \
  --artifact "$MOBILE_ARTIFACT" --device "$DEVICE_ID" \
  --output Intermediate/MobileNativeData
```

Use an Android serial or an iOS simulator UDID to match the artifact. Android requires a physical ARM64 device. This fixture exercises the native library directly; it does not replace packaged shared-API tests. The iOS device library can separately receive the existing `run_native_abi_probe.py --link-only` check without claiming a device run.

## Evidence and remaining work

On **2026-10-29**, Direct-only and Full packages passed all **40 cases** on the Galaxy S25 Ultra (Android 17) and the ARM64 iOS 27.0 simulator. They recorded 40 native client closures, 16 exact message round trips, actual object collection, no late callbacks and no pending cleanup. Android used real network ICE to the Mac Server. All four mobile package audits, the Android/simulator/Mac native DataChannel fixtures, the iOS device C ABI link check, 27 Unreal tests and 192 Python tests passed.

The runner records package and library hashes, device/runtime identity, each case, Server counters and cleanup. Development package sizes are recorded as absolute bytes; they are not Shipping download sizes or an RTC size delta.

The follow-up network suite passed **36 cases** across both profiles and platforms: 24 successful C++/Blueprint message cases and 12 certificate/authentication rejection cases. It recorded 48 exact text/binary round trips. TURN/UDP and TURN/TCP runs confirmed selected relay candidates at both ends. All 28 Unreal tests and 196 Python tests passed, along with native lifecycle, codec, DataChannel and TURN checks and package audits. The final packages also passed 20 timeout, cancellation, owner-destruction and cleanup regression cases.

On **2026-10-30**, all **32 recovery cases** passed across both profiles and platforms. They recorded 128 exact text/binary round trips, 64 native client closures and 192 rejected uses of retired channel handles. Both implementations reported signaling and relay loss, kept the game thread ticking, then reconnected after the fixture was restored. The largest measured tick gap was 351 ms. The final packages also passed 20 lifecycle regression cases. All 29 Unreal tests and 198 Python tests passed; the five Server/mobile package audits passed too.

The **24 background/resume cases** also passed on 2026-10-30, across both profiles and platforms. They recorded 80 exact text/binary round trips, 48 native client closures, 96 rejected uses of retired channel handles and eight silent pending-Connect cancellations. All four real lifecycle events were observed, and each app kept its process while switching away and returning. The first simulator attempts exposed delayed shutdown after suspension; moving Shutdown to deactivation fixed it. The final packages passed 20 lifecycle and eight reconnect/loss recovery regressions, all 29 Unreal tests, 200 Python tests and four mobile package audits. Native libraries and the previously validated Server package were reused unchanged.

See the [initial data record](reviews/SR107-mobile-data-validation.json), [HTTPS/TURN record](reviews/SR107-mobile-network-validation.json), [recovery record](reviews/SR107-mobile-recovery-validation.json) and [background/resume record](reviews/SR107-mobile-background-validation.json) for exact builds and scope. M1 remains in progress: broader packaged ordering/reliability, limits/backpressure tests and physical iOS acceptance remain open. Screen lock, OS termination, Wi-Fi/cellular transitions and ICE restart need separate validation.
