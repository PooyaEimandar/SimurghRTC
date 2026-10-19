# Packaged mobile lifecycle probe

This M0 probe checks whether the separately built ARM64 libwebrtc works inside a packaged Unreal 5.8.3 app. It creates and closes idle PeerConnections; it does not connect to a remote peer or test camera, microphone or network media.

Run both **Direct-only** and **Full** to check packaging with Janus disabled and enabled. See [validation results](M0_VALIDATION.md) for what has actually passed.

## Prepare a project

First [build the pinned libraries](MOBILE_BUILDS.md). Then stage a fresh project:

```sh
python3 tools/prepare_mobile_probe.py --profile direct-only \
  --artifact LocalBuilds/Artifacts/Android-arm64-itanium \
  --artifact LocalBuilds/Artifacts/IOS-arm64-release \
  --output LocalBuilds/MobileProbeDirect \
  --app-id org.simurghrtc.probe.direct
```

For iOS signing, add `--ios-team YOURTEAMID`. Use the ten-character development team ID, not an Apple account email. Sign in and create an Apple Development certificate in Xcode first. Keep signing credentials outside this repository.

The tool copies source and checked artifacts, enables the Android permission helper required by Unreal's Java activity, and writes `mobile-probe.json`. Android uses ARM64, API 26 minimum, SDK 36, GLES and embedded cooked data. iOS uses ARM64 with a minimum deployment target of 15.0. The sample enables Unreal's scene-based lifecycle setting, required for launch when building with the iOS 27 SDK ([Apple guidance](https://developer.apple.com/documentation/uikit/transitioning-to-the-uikit-scene-based-life-cycle)).

Use a new output directory after source changes. For the Full profile, change `--profile`, the directory and the application ID to `full`, `LocalBuilds/MobileProbeFull` and `org.simurghrtc.probe.full`.

## Build and package

Use an Unreal 5.8.3 installation with the selected mobile platform components. The tested source engine needs explicit mobile cooking modules in its Editor build. Set the Android SDK, NDK r27c and Java 21 paths for Android builds.

```sh
UE_ROOT=/absolute/path/to/UnrealEngine
PROJECT="$PWD/LocalBuilds/MobileProbeDirect/ContractHost.uproject"

"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostEditor Mac Development \
  -Project="$PROJECT" -architecture=arm64 -SimurghRTCAndroidCook -SimurghRTCIOSCook -WaitMutex

"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHost Android Development \
  -Project="$PROJECT" -architecture=arm64 -NoDebugInfo -MapFile -WaitMutex

"$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -WaitForUATMutex \
  -project="$PROJECT" -noP4 -platform=Android -cookflavor=ASTC \
  -clientarchitecture=arm64 -editorarchitecture=arm64 -clientconfig=Development \
  -skipbuild -nocompileeditor -cook -stage -pak -package -map=/Engine/Maps/Entry \
  '-AdditionalCookerOptions=-SkipZenStore -NullRHI -nosound' -unattended -utf8output
```

For iOS, use `IOS` in the Game build and package commands, and omit `-cookflavor=ASTC`. Automatic signing must finish successfully before installing the resulting `.app`. The [Server guide](NATIVE_PROBE.md) explains the source engine's recursive Xcode finalization issue. Skipping finalization is only a compile check; it does not produce an accepted signed package.

Keep build, cook and signing logs. An APK from a compile-only step may lack cooked content; run the package step before device testing.

Run cooking after other Unreal builds finish. This engine's cooker launches an SDK check that cannot wait for a busy UBT lock; a collision can misleadingly report that the mobile target platform is unavailable.

When switching staged projects, build that project's Editor immediately before cooking. Another project build can change the shared Editor build ID and leave the previous project's modules out of date.

## Test with the iOS simulator

Use the existing Xcode and an installed iOS simulator runtime. No Apple development team or physical iPhone is needed for this check.

Build a separate simulator dependency, then stage it:

```sh
PYTHON=/absolute/path/to/python3.10
"$PYTHON" tools/mobile_build.py fetch --platform IOSSimulator --work LocalBuilds/MobileIOS
"$PYTHON" tools/mobile_build.py build --platform IOSSimulator \
  --work LocalBuilds/MobileIOS --engine "$UE_ROOT" --python "$PYTHON" \
  --output LocalBuilds/Artifacts/IOSSimulator-arm64

"$PYTHON" tools/ios_simulator_support.py --engine "$UE_ROOT" \
  --output LocalBuilds/SimulatorSupport/nghttp2-arm64

python3 tools/prepare_mobile_probe.py --profile direct-only \
  --artifact LocalBuilds/Artifacts/IOSSimulator-arm64 \
  --simulator-support LocalBuilds/SimulatorSupport/nghttp2-arm64 \
  --output LocalBuilds/MobileSimulatorDirect --app-id org.simurghrtc.probe.simdirect
```

The artifact contains simulator libwebrtc and a simulator VPX library built from Unreal's pinned source archive. It links Unreal's simulator OpenSSL and Opus libraries. Device archives remain separate and cannot substitute for simulator binaries.

To test the saved Xcode 27.1 repeat build, select `LocalBuilds/Artifacts/IOSSimulator-repeat-20261018-a`. Use that same artifact for both profiles, with fresh project directories and separate application IDs. Build the HTTP/2 support library with the same pinned Xcode into a new output directory as well. The [validation record](M0_VALIDATION.md) identifies the exact manifests used by each run.

This engine also supplies a device-only `nghttp2` library to its content streaming module. The support step builds the matching pinned 1.64.0 release for the simulator. The sample target explicitly loads that library so the linker does not select device objects. It verifies the archive hash and leaves the engine unchanged. This support library is separate from the WebRTC artifact and is used only by simulator probe builds.

Build the simulator executable and the Editor modules needed for cooking, then package:

```sh
PROJECT="$PWD/LocalBuilds/MobileSimulatorDirect/ContractHost.uproject"

env UE_BUILD_FROM_XCODE=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  ContractHost IOS Development -Project="$PROJECT" -architecture=iossimulator \
  -NoUBA -NoDebugInfo -MapFile -MaxParallelActions=8 -WaitMutex

env UE_BUILD_FROM_XCODE=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  ContractHostEditor Mac Development -Project="$PROJECT" -architecture=arm64 \
  -SimurghRTCIOSCook -NoUBA -NoDebugInfo -MaxParallelActions=8 -WaitMutex

env UE_SKIP_UBT=1 "$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -WaitForUATMutex \
  -project="$PROJECT" -noP4 -platform=IOS -clientarchitecture=iossimulator \
  -editorarchitecture=arm64 -clientconfig=Development -skipbuild -nocompileeditor \
  -cook -stage -pak -package -map=/Engine/Maps/Entry \
  '-AdditionalCookerOptions=-SkipZenStore -NullRHI -nosound' \
  '-xcodebuildoptions=SDKROOT=iphonesimulator CODE_SIGN_IDENTITY=- CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=NO' \
  -unattended -utf8output
```

The first two commands compile; the last stages cooked content and creates an ad-hoc signed simulator app. `SDKROOT=iphonesimulator` is required because this engine's generated Xcode project otherwise selects the device SDK. Check Xcode's `BUILD SUCCEEDED` messages as well as UAT's exit code: this engine can report UAT success after a signing failure. The harness also verifies the bundle platform and signature.

Both profiles passed with the Xcode/SDK 27.1 repeat-build artifact and matching packaging tools, running on the installed iOS 27.0 simulator. See [packaged results](M0_VALIDATION.md#packaged-mobile-probe-work) for the selected manifest and measurements. Earlier runtime reports retain their original dependency identities; the iOS device build keeps its 27.0 pin.

Run the resulting simulator `.app` with its UDID from `xcrun simctl list devices available`:

```sh
python3 tools/run_mobile_probe.py --platform IOSSimulator \
  --project LocalBuilds/MobileSimulatorDirect \
  --app LocalBuilds/MobileSimulatorDirect/Saved/StagedBuilds/IOS/ContractHost.app \
  --device SIMULATOR_UDID \
  --output Intermediate/MobileProbes/IOSSimulatorDirect
```

The harness boots the selected simulator if needed, installs and launches the app, reads its saved report, and terminates that app. It matches the report UUID and process ID to the launch. This lifecycle check uses `-nullrhi -sm5` and `-nosound`; graphics, microphone and camera behavior are outside its scope. Simulator staging adds [headless shader metadata](../Config/IOSSimulatorProbe/DataDrivenPlatformInfo.ini) through Unreal's project platform configuration because the iOS NullRHI expects a generic SM5 entry. A pass is recorded as `validation_scope: "simulator"`, with `simulator_build: true`. It does not establish physical-device support or app delivery size. Repeat with the Full profile to check Janus-enabled linkage.

## Run on a device

Connect and authorize the Android device for USB debugging. Give the harness its exact serial and packaged APK:

```sh
python3 tools/run_mobile_probe.py --platform Android \
  --project LocalBuilds/MobileProbeDirect \
  --app /absolute/path/to/ContractHost.apk --device ADB_SERIAL \
  --output Intermediate/MobileProbes/AndroidDirect
```

For iOS, enable Developer Mode, trust the Mac and use a signed device app:

```sh
python3 tools/run_mobile_probe.py --platform IOS \
  --project LocalBuilds/MobileProbeDirect \
  --app /absolute/path/to/ContractHost.app --device COREDEVICE_ID \
  --output Intermediate/MobileProbes/IOSDirect
```

Use `xcrun devicectl list devices` for the identifier. For iOS 16, use `--ios-backend libimobiledevice` and the USB UDID from `idevice_id -l`; the legacy developer disk image must already be mounted. Xcode 27's device debugging supports iOS 17 or later, even though its SDK can build apps targeting iOS 15. Older device support must come from a compatible Apple toolchain. See [Apple's compatibility table](https://developer.apple.com/xcode/system-requirements).

The harness installs the supplied app, starts a fresh run, saves its log and stops that app afterward. Use a new evidence directory each time. `result.json` must report `passed: true` and `app_stopped: true`.

## What counts as a pass

Each run completes three cycles: nine factories and nine peers, six explicit stops and three destructor cleanups. It checks media/data-only factories, duplicate calls, overlapping owners and restart after stop. The report must match the run ID, platform, profile and SHA-256 of the staged artifact manifest. Editor, simulator, stale and incomplete reports cannot count as device acceptance.

Keep the linker response/map alongside the report and verify that only the selected libwebrtc archive is linked. For Android, inspect final ELF load-segment alignment as well as the ARM64-only APK. The harness records Development package sizes. These are initial measurements, not Shipping delivery sizes or the incremental cost of RTC; those require matching RTC-off/on builds described in [Mobile clients](MOBILE_CLIENTS.md).

For simulator evidence, also record the selected VPX and HTTP/2 archives, Xcode/SDK versions, and the engine revision. Compare the native executable's UUID with the packaged executable's UUID to tie the link map to the app that ran. Verify Janus is absent in Direct-only and present in Full. Keep earlier reports attached to their original dependency manifests.
