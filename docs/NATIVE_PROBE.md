# Native factory and headless Server probe

This M0 probe links Unreal Engine **5.8.3**'s WebRTC **5414 / M109**, creates a PeerConnection factory and an idle Unified Plan PeerConnection, and exposes diagnostics through standard UE **HTTPServer**. It does not exchange SDP, gather ICE, connect to a remote peer, send DataChannels or process media. The shared session API still reports `NotImplemented` for Connect.

## Integration boundary

`SimurghRTCNative` owns the adapter. Its public integration header uses Unreal types and a PImpl; all WebRTC includes, reference counts and callbacks stay in private source. `FNativeRuntime` is for plugin integration, not the application's shared client contract. Calls and destruction occur on the game thread. Start/stop are synchronous bounded setup operations; future negotiation must use asynchronous callbacks.

The adapter owns network, worker and signaling threads. It creates and closes the peer/factory on the signaling thread, releases audio dependencies on the worker thread, joins all threads, then releases its SSL initialization lease. Multiple runtime instances share that lease. Loading the module alone starts no threads or listeners. The automation test exercises overlapping instances, duplicate calls, teardown and restart.

| Mode | Factory dependencies | What the probe establishes |
|---|---|---|
| `builtin-media` (default) | Builtin audio/video codec factories, APM and plugin dummy audio device | The archive exports the factory/codecs/APM required to construct a headless peer |
| `data-only` | Modular factory without a media engine, ADM or APM | An idle peer can be created without constructing media dependencies |

The engine archive does not export `AudioDeviceModule::Create`. The private UE58 adapter supplies a dummy ADM using the M109 `AudioDeviceModuleDefault` interface. It opens no hardware and delivers no audio callbacks. The separate [SR-004 media probe](MEDIA_PROBE.md) now validates local generated VP8/Opus encode/decode, UE texture/render-target capture and procedural PCM consumption. It does not add ADM callbacks, remote tracks or a network media path to this factory probe. M109's RTP capability query requires a media engine; the data-only branch must not call it. Runtime data-only mode does **not** promise codec code is stripped from the linked binary.

## Build selection and provenance

The desktop adapter selects the engine's matching headers/archive through UBT's `WebRTC` module, with explicit `OpenSSL`, `libOpus` and `LibVpx` dependencies. Precompiled external modules did not propagate all private static links in the installed-engine build. Mac also links AudioToolbox, CoreAudio, CoreVideo and VideoToolbox; linking frameworks does not open a device.

UBT requires engine version 5.8.3 and the adapter checks `WEBRTC_5414`. Mac, Win64 and Linux x64 select the engine library; **only Mac ARM64 has been validated among desktop targets**. Android/iOS require a [separate ARM64 artifact](MOBILE_ARTIFACTS.md) and fail the build when it is absent or incompatible. They have no engine fallback. Packaged factory/teardown probes pass on Samsung S25 Ultra and the ARM64 iOS simulator; physical iPhone acceptance is deferred. Provider/module profiles are independent of library selection. `SIMURGHRTC_WITH_NATIVE_TRANSPORT=0` remains correct because the shared session service is not implemented.

Local audit on 2026-10-17:

| Item | Observation |
|---|---|
| Installed engine | UE 5.8.3, CL 58210709, compatible CL 55116800 |
| Source engine | UE 5.8.3, CL 0, branch `UE5`; Git `1a6598b42ab311f9d9766e10404e54c82ce1a0a1` |
| WebRTC archive | `Engine/Source/ThirdParty/WebRTC/5414/Lib/Mac/Release/libwebrtc.a` |
| Archive architecture and size | Universal x86_64 + arm64, 41,870,008 bytes; the probe target is arm64 |
| Archive SHA-256, identical in both engines | `172e7fdd53b10e78b5e06a268fca9f8ee85492e24f04fea800b65c35c07966cf` |
| WebRTC symbols | Engine module selects Release, including for Debug targets; its build rules state Release lacks debug symbols |
| Engine external dependencies on Mac | OpenSSL 1.1.1t; `libOpus` selects the `opus-1.1` Mac archives and Speex resampler; LibVpx 1.14.1 |
| Builtin sender audio names observed | `opus`, `red`, `ISAC`, `G722`, `PCMU`, `PCMA`, `CN`, `telephone-event` |
| Builtin sender video names observed | `VP8`, `rtx`, `VP9`, `red`, `ulpfec`; no H.264 or AV1 advertised |

These are local factory capabilities, including repair/auxiliary formats, not negotiated codec support or throughput measurements. The exact upstream WebRTC commit and Epic patch inventory are not established by the `5414` directory name. The archive checksum identifies the tested binary; it is not a mobile revision pin or an app-size measurement. The [completed SR-003 review](DEPENDENCY_REVIEW.md) assigns security/provenance remediation to SR-008 and final notices to SR-902 before release approval.

The repository contains our adapter source and no copied engine binaries or implementation. Relevant supplied notice locations include `Engine/Source/ThirdParty/Licenses/{WebRTC_License,OpenSSL_License,Opus_License,abseil-cpp_LICENSE}.txt`, and the WebRTC include tree's libyuv, libsrtp and libvpx license files. This is an initial inventory, not a complete redistributed-dependency notice bundle. Preserve the engine/dependency notices required for the final packaged product; the repository's Apache-2.0 license does not replace them. Mobile artifacts need their own full source/patch/license inventory.

## Build and stage a dedicated Server

A source-built UE 5.8.3 checkout with Server support is required for this acceptance check. The local installed distribution rejects Server targets; see Epic's [dedicated server setup](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-dedicated-servers-in-unreal-engine). No engine source modifications are required by this plugin.

Run from this repository root. Use a new staging path for a changed profile/source snapshot. Keep staged projects outside any parent `Intermediate` directory to avoid the tested source engine dependency-cache issue. Build commands write normal engine/toolchain caches outside the repository.

```sh
UE_ROOT=/absolute/path/to/UnrealEngine
python3 tools/prepare_profile.py --profile server --output LocalBuilds/NativeServer
PROJECT="$PWD/LocalBuilds/NativeServer/ContractHost.uproject"

"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostServer Mac Development \
  -Project="$PROJECT" -WaitMutex -NoHotReloadFromIDE -architecture=arm64 -NoUBA
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostEditor Mac Development \
  -Project="$PROJECT" -WaitMutex -NoHotReloadFromIDE -architecture=arm64 -NoUBA

```

The Editor build is used for cooking. The runtime acceptance launches the staged **ContractHostServer executable**, not UnrealEditor-Cmd. Pass the executable inside the staged `.app` when applicable, not the app directory. See [the validation record](M0_VALIDATION.md) for the actual staging path and outcomes.

The tested checkout's recursive Xcode app-finalization pre-action failed both with and without `-NoUBA`. C++ compilation/linking succeeded; running Unreal's own finalization mode separately succeeded. If and only if a build reaches this same finalization failure after all C++ actions succeed, use the recovery below before cooking. It invokes normal UBT modes, writes no engine source, and does not substitute for compilation:

```sh
PROJECT_DIR="$(dirname "$PROJECT")"
CACHE="$PROJECT_DIR/Intermediate/Build/XmlConfigCache.bin"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" -Mode=ApplePostBuildSync \
  -Input="$PROJECT_DIR/Intermediate/Build/Mac/arm64/ContractHostServer/Development/PostBuildSync_ContractHostServer.json" \
  -XmlConfigCache="$CACHE"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" -Mode=ApplePostBuildSync \
  -Input="$UE_ROOT/Engine/Intermediate/Build/Mac/arm64/UnrealEditor/Development/PostBuildSync_ContractHostEditor.json" \
  -XmlConfigCache="$CACHE"
# Only after both finalization steps succeed, generate the normal target/module receipts.
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostServer Mac Development \
  -Project="$PROJECT" -architecture=arm64 -NoUBA -ActionTypeFilter=WriteMetadata
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostEditor Mac Development \
  -Project="$PROJECT" -architecture=arm64 -NoUBA -ActionTypeFilter=WriteMetadata
```

`-NoUBA` disables accelerated compilation but does not resolve this recursive finalization failure by itself. Switching that option on existing accelerated output may require recompilation. The final validation record distinguishes compile/link, standalone finalization, metadata generation and staged runtime checks.

After successful builds/finalization and receipts, cook and stage:

```sh
"$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$PROJECT" -noP4 -platform=Mac -serverplatform=Mac -server -noclient \
  -serverarchitecture=arm64 -editorarchitecture=arm64 \
  -serverconfig=Development -skipbuild -nocompileeditor -cook -stage -pak \
  -map=/Engine/Maps/Entry -AdditionalCookerOptions="-SkipZenStore -NullRHI -nosound" \
  -unattended -utf8output
```

```sh
SERVER_EXE=/absolute/path/to/staged/ContractHostServer
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" \
  --output Intermediate/Reports/ServerMedia
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" \
  --data-only --output Intermediate/Reports/ServerData
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" \
  --occupied-port --output Intermediate/Reports/ServerOccupied
```

Use a new/empty output directory for each run. The harness correlates the HTTP response to its launched PID and random run ID, checks the compiled Server flag and factory diagnostics, tests route/method rejection, and verifies process shutdown and listener closure. The occupied-port case verifies explicit bind failure, no readiness announcement and native cleanup. Each directory contains `result.json` and captured `process.log`, plus `unreal.log` when the engine creates a file log. The result identifies which runtime log was checked; the tested packaged Mac Server uses stdout.

UE 5.8's Mac generic `RequestExitWithStatus` ignores the requested exit code during graceful shutdown. The harness therefore accepts that documented engine behavior only with its explicit warning and failure/cleanup markers for the negative case. A positive run requires a valid HTTP response and clean exit. Use the **Python harness exit code** for automation; the Unreal exit code alone is insufficient.

## Editor fallback and lifecycle automation

An installed engine can run the same probe using `--kind editor-server --project "$PROJECT" --executable "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"`. Its JSON explicitly reports `compiled_server_target: false`; it cannot satisfy dedicated Server acceptance. Build that project's Editor target with the same engine first. Run Editor processes sequentially to avoid unrelated SDK/trace-log initialization races.

After building the Full source host, run the contract/native tests (including the local media codec fixture):

```sh
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PROJECT" \
  -unattended -NullRHI -nosplash -nosound \
  -ExecCmds="Automation RunTests SimurghRTC." -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/Intermediate/Reports/NativeAutomation"
python3 -m unittest discover -s tools/tests -v
```

## Manual diagnostic options

Without `-SimurghRTCProbe`, the Server module opens no listener. With it, startup requires dedicated-server runtime mode and rendering disabled. The probe requires an isolated process with no existing HTTPServer module owner because that module exposes global start/stop operations.

| Option | Default / behavior |
|---|---|
| `-SimurghRTCProbePort=18777` | 1024–65535; bind only `127.0.0.1`, disallow port/address reuse |
| `-SimurghRTCProbeSeconds=15` | 1–3600 seconds; automatically close resources and exit |
| `-SimurghRTCProbeRunId=...` | At most 64 characters, echoed in diagnostics |
| `-SimurghRTCProbeDataOnly` | Select modular factory without media dependencies |

GET `/simurghrtc/probe/health` returns readiness JSON. There is no signaling route. The temporary per-port HTTPServer config override is restored on shutdown. At least one health request must arrive before the deadline; otherwise the probe logs failure and requests exit status 3. Startup failure requests status 2. Normal observed completion requests 0. On Mac use the harness as described above.
