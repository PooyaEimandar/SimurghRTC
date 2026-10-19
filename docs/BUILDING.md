# Build and test the plugin

Use **Unreal Engine 5.8.3**, its supported platform toolchain and **Python 3.10+**. The commands below target Mac ARM64, the platform tested so far.

A regular engine installation is enough for Editor/Game tests. Dedicated Server builds need a source-built engine with Server support; follow the [Server guide](NATIVE_PROBE.md). For Android/iOS, [build the pinned ARM64 dependency](MOBILE_BUILDS.md), then follow the [packaged device probe](MOBILE_PROBE.md). Mobile device qualification remains open.

## 1. Check the source and fixtures

From the repository root:

```sh
python3 -m unittest discover -s tools/tests -v
python3 tools/prepare_profile.py --check-default
python3 tools/media_fixtures.py --check
```

These checks need neither Unreal nor Docker. To test local Janus/TURN services, use the separate [fixture guide](FIXTURES.md).

## 2. Build and run Unreal tests

Set `UE_ROOT` to your engine directory, the one containing `Engine`:

```sh
UE_ROOT=/absolute/path/to/UE_5.8
PROJECT="$PWD/Samples/ContractHost/ContractHost.uproject"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ContractHostEditor Mac Development \
  -Project="$PROJECT" -WaitMutex -NoHotReloadFromIDE -architecture=arm64

"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PROJECT" \
  -unattended -NullRHI -nosplash -nosound \
  -ExecCmds="Automation RunTests SimurghRTC." \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/Intermediate/Reports/Full"
```

The sample uses the Full profile. Its tests cover API validation, completion, cancellation, teardown, garbage collection, frame bounds, profile selection, native factory/peer lifetimes and local video/audio codecs. Check the report for failures; process exit alone is not enough.

For executable C++ and Blueprint graphs, including packaged runs, follow [Lifecycle examples](LIFECYCLE_EXAMPLES.md).

These tests run without graphics or audio devices. For texture capture and audio-mixer checks, follow the [media guide](MEDIA_PROBE.md).

## 3. Try a different build profile

A profile chooses which plugin modules are included. For a build without Janus:

```sh
python3 tools/prepare_profile.py --profile direct-only --output LocalBuilds/Direct
```

This creates a standalone `ContractHost.uproject` and copies only the selected plugin source. Set `PROJECT="$PWD/LocalBuilds/Direct/ContractHost.uproject"`, then repeat the build/test commands with a different report directory.

Use a new or empty destination under `LocalBuilds/`. Do not place staged projects under `Intermediate/`: the tested source engine's dependency cache can then trigger full rebuilds. After source changes, stage into a fresh directory to avoid stale copies.

| Profile | Included modules/features |
|---|---|
| `full` | Both providers, client media, and Server probe for Editor/Server targets |
| `direct-only` | Direct provider and client media; Janus source/types omitted |
| `janus-only` | Janus provider/extensions and client media; no Direct or Server module |
| `data-only` | Both client providers; no client media, Janus room extensions or Server module |
| `server` | Core, native adapter, Direct and Server probe; no client media or Janus |

Profiles select code; they do not make the unfinished session transport available. The data-only profile is a client build. To test a dedicated data-only factory, use the Server profile and its probe option.

Staging also selects sample content. `Examples/BP_RtcLifecycle` is shared by every profile. `Examples/Janus/BP_JanusProfile` needs the Janus extension module and is copied only into Full and Janus-only hosts. This rule lives beside the module lists in `Config/Profiles.json`; excluded assets are omitted before cooking.

The [lifecycle harness](LIFECYCLE_EXAMPLES.md) supports all four client profiles. It checks provider modules, reflected media/extension classes and the optional asset inside the running package. The package audit then checks UHT inputs, the linker map, one libwebrtc archive and the identity of the tested executable.

## 4. Check a minimal cook

After building the selected project:

```sh
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PROJECT" \
  -run=Cook -TargetPlatform=Mac -Map=/Engine/Maps/Entry \
  -SkipZenStore -unattended -NullRHI -nosplash -nosound
```

This cooks the empty Entry map to local files. It checks that selected modules load and cook. It does not test a packaged RTC call or executable Blueprint demo. Use the [Server](NATIVE_PROBE.md) and [media](MEDIA_PROBE.md) guides for packaged probes, and [validation results](M0_VALIDATION.md) for known warnings and workarounds.

## Source layout

All plugin modules below live under `Plugins/SimurghRTC/Source/`.

| Location | Purpose |
|---|---|
| `SimurghRTCCore` | Shared C++/Blueprint types, session API and operation lifecycle |
| `SimurghRTCNative` | Private libwebrtc adapter and local codec tests |
| `SimurghRTCDirect` | Placeholder for Direct signaling |
| `SimurghRTCMedia` | Optional client media API and texture/audio tests |
| `SimurghRTCServer` | Headless HTTP test endpoint for Editor/Server targets |
| `SimurghRTCJanus` | Optional Janus provider placeholder |
| `SimurghRTCJanusExtensions` | Optional Janus room API test types |
| `Samples/ContractHost` | Minimal Game, Editor and Server test project |
| `Config/Profiles.json` | Profile/module selection rules |
| `tools/prepare_profile.py` | Generates and stages profiles |
| `Fixtures/` | Versioned media/signaling inputs and service configuration |

## Build details

<details>
<summary>Engine version, module flags and mobile dependencies</summary>

The project targets UE **5.8.3**, but the `.uplugin` file uses `EngineVersion: 5.8.0`. This matches the engine's compatibility version. Setting that field to `5.8.3` caused an incompatibility prompt and skipped optional modules in unattended startup. It does not mean other engine patches have been tested.

Desktop builds link the engine's WebRTC 5414 library plus OpenSSL, Opus and VPX. The sample finds the source plugin through `AdditionalPluginDirectories`; staged samples use their local copy.

Generated descriptors select modules for Unreal's build tools and Header Tool. Build.cs reads `SimurghRTC.profile.json` and defines `SIMURGHRTC_WITH_DIRECT`, `SIMURGHRTC_WITH_JANUS`, `SIMURGHRTC_WITH_JANUS_EXTENSIONS` and `SIMURGHRTC_WITH_MEDIA`. The extension flag follows its own module selection. `SIMURGHRTC_WITH_NATIVE_TRANSPORT` remains **0** because shared session transport is unfinished. Server targets omit Janus and client-media modules, even under Full. Use a staged `server` host for the Server probe so its sample content also excludes client-only extensions.

After changing profile policy, use `prepare_profile.py --refresh-default`, update the matching Build.cs dependencies and rerun the checks. Changing a macro alone does not remove reflected types or assets.

Android/iOS are client-only targets. They require separate pinned ARM64 libwebrtc artifacts; a missing artifact must not silently fall back to the engine's mobile library. Provider selection is independent of platform library selection. SR-006/SR-007 cover integration and device startup. See [Mobile clients](MOBILE_CLIENTS.md) for build and app-size requirements.

</details>
