# C++ and Blueprint lifecycle examples

These examples teach the frozen [API v1 contract](API_CONTRACT.md) in Unreal **5.8.3**. They run the same steps through a C++ actor and an actual Blueprint graph. They need no server, camera, microphone or graphics device.

| Step | Expected outcome |
|---|---|
| Create and retain a session; Disconnect | Local success, session Closed |
| Connect with a valid configuration | Failure with `NotImplemented`, session Failed |
| Start another Connect and cancel its proxy | One On Cancelled event |
| Start another operation and call Shutdown | Cancelled result, no completion callback |
| Destroy the actor with an operation pending | EndPlay shuts down immediately; no late completion |

A successful local Disconnect does not mean a network connection worked. The examples deliberately expect Connect to fail at the unfinished transport boundary. Later connection demos belong to M1 and M3.

## Open the examples

Open `Samples/ContractHost/ContractHost.uproject` in UE 5.8.3 after building its Editor target. In the Content Browser, open **Examples → BP_RtcLifecycle**. The graph uses the public Create Session, Connect, Disconnect, Cancel and Shutdown nodes. It handles all three completion branches, breaks the failure result to print its safe error message, and calls Shutdown from EndPlay.

The equivalent native actor is `AContractLifecycleCpp` in [ContractLifecycleExample.cpp](../Samples/ContractHost/Source/ContractHost/ContractLifecycleExample.cpp). The shared parent only records and checks outcomes; it does not execute the Blueprint's operation chain. Drop either example actor into a level and play. The Output Log records the three completion steps. Choose DirectUnreal or Janus in the actor's Config; Janus requires a profile that includes it.

In application code, retain the session and any dormant operation in transient `UPROPERTY` fields. Bind `OnCompletedNative` before `Activate()`, and call `Session->Shutdown()` from EndPlay. In Blueprints, keep the session in a variable and retain the exposed **Operation** proxy when cancellation will happen later. Async nodes activate automatically.

## Run the checks

These commands target the tested Mac ARM64 source engine. From the repository root, set the engine path and stage a fresh Full host:

```sh
UE_ROOT=/absolute/path/to/UnrealEngine
python3 tools/prepare_profile.py --profile full --output LocalBuilds/ApiFull
PROJECT="$PWD/LocalBuilds/ApiFull/ContractHost.uproject"

# Compile the example and its Editor-only Blueprint generator.
env UE_BUILD_FROM_XCODE=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  ContractHostEditor Mac Development -Project="$PROJECT" -architecture=arm64 -NoUBA -WaitMutex
env UE_SKIP_UBT=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" -Mode=ApplePostBuildSync \
  -Input="$UE_ROOT/Engine/Intermediate/Build/Mac/arm64/UnrealEditor/Development/PostBuildSync_ContractHostEditor.json" \
  -XmlConfigCache="$(dirname "$PROJECT")/Intermediate/Build/XmlConfigCache.bin"

python3 tools/run_lifecycle_probe.py \
  --executable "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  --kind editor-game --project "$PROJECT" --profile full \
  --output Intermediate/Reports/LifecycleFullEditor
```

The harness starts both actors, checks completion order, IDs and errors, destroys them, allows extra ticks for late callbacks, and requires the launched process to exit. Each run writes `result.json`, a correlated native report and logs. Use a fresh output directory for every run. Repeat with `--provider Janus` and another output directory to switch provider through configuration, without changing either example.

It also checks the selected provider modules, reflected media/extension types and Janus sample asset. Full and Janus-only run `Examples/Janus/BP_JanusProfile`: its real **Join Video Room** node receives no session and must report exactly one `InvalidArgument` failure. This tests optional Blueprint dependencies without claiming a room connection. Direct-only and data-only must have neither that asset nor its reflected extension types.

Run the Unreal automation tests from [Building](BUILDING.md) too. They cover deferred completion, cancellation, reentrant shutdown, garbage collection, profile exclusions and CPU-frame validation. Keep other builds from modifying the shared engine binaries during runtime validation; `-WaitMutex` only serializes UBT builds.

If an incremental build reports stale `GENERATED_BODY` line numbers after copying updated sample headers, repeat the build with `-ForceHeaderGeneration`.

## Package the examples

Build the Game executable, finalize the Mac app, and cook the Blueprint directory explicitly:

```sh
env UE_BUILD_FROM_XCODE=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  ContractHost Mac Development -Project="$PROJECT" -architecture=arm64 -NoDebugInfo -NoUBA -WaitMutex -MapFile
PROJECT_DIR="$(dirname "$PROJECT")"
env UE_SKIP_UBT=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" -Mode=ApplePostBuildSync \
  -Input="$PROJECT_DIR/Intermediate/Build/Mac/arm64/ContractHost/Development/PostBuildSync_ContractHost.json" \
  -XmlConfigCache="$PROJECT_DIR/Intermediate/Build/XmlConfigCache.bin"
env UE_BUILD_FROM_XCODE=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  ContractHost Mac Development -Project="$PROJECT" -architecture=arm64 -NoDebugInfo -ActionTypeFilter=WriteMetadata -WaitMutex

env UE_SKIP_UBT=1 "$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -WaitForUATMutex \
  -project="$PROJECT" -noP4 -platform=Mac -clientarchitecture=arm64 -editorarchitecture=arm64 \
  -clientconfig=Development -skipbuild -nocompileeditor -cook -stage -pak -package \
  -map=/Engine/Maps/Entry -cookdir="$PROJECT_DIR/Content/Examples" \
  '-AdditionalCookerOptions=-SkipZenStore -NullRHI -nosound' -unattended -utf8output

python3 tools/run_lifecycle_probe.py \
  --executable "$PROJECT_DIR/Saved/StagedBuilds/Mac/ContractHost.app/Contents/MacOS/ContractHost" \
  --kind game --profile full --output Intermediate/Reports/LifecycleFullGame
```

The environment flags split native compilation from Xcode app finalization, avoiding the recursive build problem on the tested source engine. Require successful native compilation and Xcode finalization before accepting the package. The harness can recover its correlated JSON report from stdout when packaged Mac file policy prevents writing the requested external report file.

Repeat with fresh `direct-only`, `janus-only` and `data-only` staged hosts and the matching `--profile`. The harness defaults to Janus for Janus-only and DirectUnreal otherwise; it rejects a provider excluded by the profile. Exercise both providers for Full and data-only. The shared lifecycle asset is identical across profiles; optional Janus content follows module selection. The generator module is Editor-only and must not appear in the packaged game.

After a packaged run, audit the selected modules and executable:

```sh
python3 tools/audit_profile_package.py \
  --project-dir "$PROJECT_DIR" --profile full \
  --runtime-result Intermediate/Reports/LifecycleFullGame/result.json \
  --output Intermediate/Reports/LifecycleFullGame/package-audit.json
```

The audit requires the Game link map produced by `-MapFile`, matching native/package UUIDs, the runtime executable hash, exact plugin modules in the map, and the expected reflected modules in UHT inputs. It also requires one linked libwebrtc archive. Runtime report schema **2** includes profile/asset checks; API contract version remains **1**. Older schema-1 reports remain historical evidence and are not accepted by the current runner.

## Regenerate the Blueprint

The checked-in `.uasset` is ready to open, compile and cook. Its source recipe is [ContractExamplesCommandlet.cpp](../Samples/ContractHost/Source/ContractHostEditor/ContractExamplesCommandlet.cpp). To intentionally replace the generated sample in a staged project:

```sh
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PROJECT" \
  -run=ContractExamples -ReplaceGeneratedExample -unattended -NullRHI -nosound \
  -abslog="$PWD/Intermediate/Reports/BlueprintGeneration.log"
```

Without the replacement flag, an existing sample is preserved. The generator requires every pin connection to succeed and compiles the graph with zero warnings/errors before saving. It does not modify the engine or use Python Editor plugins. Regeneration creates fresh editor node GUIDs; the `.uasset` bytes are not a reproducible build artifact.

Add `-JanusExample` to regenerate the optional Janus graph instead. This requires Full or Janus-only. Its options input uses the reflected Janus struct, so the sample tests both extension types and async nodes.

These examples validate lifecycle behavior on the tested host. They do not establish network calls, device capture, frame streaming, mobile Blueprint acceptance or Shipping app sizes. See [M0 validation](M0_VALIDATION.md) for the exact tested profiles and engine revision.
