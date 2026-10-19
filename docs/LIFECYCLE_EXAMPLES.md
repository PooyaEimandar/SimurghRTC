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

Run the Unreal automation tests from [Building](BUILDING.md) too. They cover deferred completion, cancellation, reentrant shutdown, garbage collection, profile exclusions and CPU-frame validation. Keep other builds from modifying the shared engine binaries during runtime validation; `-WaitMutex` only serializes UBT builds.

If an incremental build reports stale `GENERATED_BODY` line numbers after copying updated sample headers, repeat the build with `-ForceHeaderGeneration`.

## Package the examples

Build the Game executable, finalize the Mac app, and cook the Blueprint directory explicitly:

```sh
env UE_BUILD_FROM_XCODE=1 "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
  ContractHost Mac Development -Project="$PROJECT" -architecture=arm64 -NoDebugInfo -NoUBA -WaitMutex
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

Repeat with a fresh `direct-only` staged host and `--profile direct-only` to prove the same asset loads and runs without Janus classes. `prepare_profile.py` copies the shared Content directory as well as source. The generator module is Editor-only and must not appear in the packaged game.

## Regenerate the Blueprint

The checked-in `.uasset` is ready to open, compile and cook. Its source recipe is [ContractExamplesCommandlet.cpp](../Samples/ContractHost/Source/ContractHostEditor/ContractExamplesCommandlet.cpp). To intentionally replace the generated sample in a staged project:

```sh
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PROJECT" \
  -run=ContractExamples -ReplaceGeneratedExample -unattended -NullRHI -nosound \
  -abslog="$PWD/Intermediate/Reports/BlueprintGeneration.log"
```

Without the replacement flag, an existing sample is preserved. The generator requires every pin connection to succeed and compiles the graph with zero warnings/errors before saving. It does not modify the engine or use Python Editor plugins. Regeneration creates fresh editor node GUIDs; the `.uasset` bytes are not a reproducible build artifact.

These examples validate lifecycle behavior on the tested host. They do not establish network calls, device capture, frame streaming, mobile Blueprint acceptance or Shipping app sizes. See [M0 validation](M0_VALIDATION.md) for the exact tested profiles and engine revision.
