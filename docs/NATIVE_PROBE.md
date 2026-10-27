# Native factory and headless Server probe

This M0 probe runs the isolated **WebRTC M155** backend inside Unreal Engine **5.8.3**. It creates a factory and an idle PeerConnection, then exposes diagnostics through Unreal's **HTTPServer** on loopback. This probe does not exchange signaling or network media. For real Direct connections and messages, use [Shared sessions](SHARED_SESSIONS.md).

The packaged Mac ARM64 Server passed both factory modes and both occupied-port cases on **2026-10-22**. See the [Server validation snapshot](reviews/SR008-M155-server-validation.json) for artifact, executable and engine identities.

## Integration boundary

`SimurghRTCNative` owns the Unreal adapter. It calls the separately built library through six private C entry points. WebRTC C++ types, allocations, threads and reference counts stay inside that library. The public client API and Blueprint nodes are unchanged.

The runtime owns network, worker and signaling threads. Shutdown closes the peer, releases the factory and audio dependencies, joins the threads and releases its shared SSL initialization lease. This synchronous probe makes its calls on the game thread. The separate [asynchronous ownership layer](NATIVE_PEERS.md) moves native creation and teardown off the game thread for future providers. Loading the module alone starts no threads or listeners.

| Mode | Factory dependencies | What the probe establishes |
|---|---|---|
| `builtin-media` (default) | Builtin codecs, audio processing and a dummy audio device | A headless factory and peer can be created without hardware audio |
| `data-only` | Modular factory without media dependencies | An idle peer can be created without audio/video factories |

Data-only is a runtime mode; it does not promise a smaller linked library. Codec execution, render-target capture and mixer playback have separate [media probes](MEDIA_PROBE.md).

## Build selection and provenance

Stage a checked Mac ARM64 artifact from the [native backend build](NATIVE_BACKEND.md). UBT checks its source lock, bridge inputs and payload hashes. The Server profile excludes Janus and the Unreal media presentation module. It links one isolated WebRTC archive; Unreal's own HTTP, audio and crypto dependencies remain engine-owned.

Pass `--native-artifact` to the harness below. Both HTTP health and the startup log must identify that exact artifact manifest and M155 backend. The log check also applies when an occupied port prevents HTTP startup. Each report records the launched executable's SHA-256, PID and run ID.

The [M0 validation record](M0_VALIDATION.md) separates current M155 results from the earlier engine-bundled M109 feasibility results. Final dependency qualification and distribution notices remain tracked under SR-008 and SR-902.

## Build and stage a dedicated Server

A source-built UE 5.8.3 checkout with Server support is required for this acceptance check. The local installed distribution rejects Server targets; see Epic's [dedicated server setup](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-dedicated-servers-in-unreal-engine). No engine source modifications are required by this plugin.

Run from this repository root. Use a new staging path for a changed profile/source snapshot. Keep staged projects outside any parent `Intermediate` directory to avoid the tested source engine dependency-cache issue. Build commands write normal engine/toolchain caches outside the repository.

```sh
UE_ROOT=/absolute/path/to/UnrealEngine
NATIVE_ARTIFACT="$PWD/LocalBuilds/Artifacts/M155-Mac"
python3 tools/prepare_profile.py --profile server --native-artifact "$NATIVE_ARTIFACT" \
  --output LocalBuilds/NativeServer
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
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" --native-artifact "$NATIVE_ARTIFACT" \
  --output Intermediate/Reports/ServerMedia
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" --native-artifact "$NATIVE_ARTIFACT" \
  --data-only --output Intermediate/Reports/ServerData
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" --native-artifact "$NATIVE_ARTIFACT" \
  --occupied-port --output Intermediate/Reports/ServerOccupied
python3 tools/run_native_probe.py --kind server --executable "$SERVER_EXE" --native-artifact "$NATIVE_ARTIFACT" \
  --occupied-port --data-only --output Intermediate/Reports/ServerDataOccupied
```

Use a new/empty output directory for each run. The harness correlates the HTTP response to its launched PID and random run ID, checks the compiled Server flag and factory diagnostics, tests route/method rejection, and verifies process shutdown and listener closure. The occupied-port case verifies explicit bind failure, no readiness announcement and native cleanup. Each directory contains `result.json` and captured `process.log`, plus `unreal.log` when the engine creates a file log. The result identifies which runtime log was checked; the tested packaged Mac Server uses stdout.

UE 5.8's Mac generic `RequestExitWithStatus` ignores the requested exit code during graceful shutdown. The harness therefore accepts that documented engine behavior only with its explicit warning and failure/cleanup markers for the negative case. A positive run requires a valid HTTP response and clean exit. Use the **Python harness exit code** for automation; the Unreal exit code alone is insufficient.

## Editor fallback and lifecycle automation

An installed engine can run the same probe using `--kind editor-server --project "$PROJECT" --executable "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"`. Pass the same `--native-artifact` selection. Its JSON explicitly reports `compiled_server_target: false`; it cannot satisfy dedicated Server acceptance. Build that project's Editor target with the same engine first. Run Editor processes sequentially to avoid unrelated SDK/trace-log initialization races.

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
