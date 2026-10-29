# C++ and Blueprint data examples

These two Unreal **5.8.3** maps connect to the plugin's Unreal Server, send one Unicode text message and one binary message, check their echoes, then close the channel and disconnect.

| Map under `Content/Examples/Direct/Maps` | Implementation |
|---|---|
| `RtcDataBlueprint` | The actual `BP_RtcData` event graph |
| `RtcDataCpp` | `AContractDataCpp` in [ContractDataExample.cpp](../Samples/ContractHost/Source/ContractHost/ContractDataExample.cpp) |

The graph uses the public session nodes directly. Its shared C++ parent supplies configuration and checks results; it does not perform the Blueprint's connection, send or shutdown operations. The binary payload includes zero and non-ASCII bytes. No camera, microphone or graphics device is needed.

## Open and run

Build the sample host with a Direct-enabled profile and the current [Mac native artifact](NATIVE_BACKEND.md). Open either map in the Editor. The actor's **Config** supplies the endpoint and identity. Supply its access token at runtime; do not save credentials in an asset.

For a packaged application, select the map and provide the full signaling URL and a credential file:

```sh
"$CLIENT_EXE" /Game/Examples/Direct/Maps/RtcDataBlueprint \
  -SimurghRTCExampleEndpoint="$ENDPOINT" \
  -SimurghRTCExampleTokenFile="$TOKEN_FILE"
```

Use `RtcDataCpp` to run the native equivalent. Keep the token file readable only by your account. On packaged Mac builds, put it in the application's permitted container. The test runner below handles this automatically.

The Server must authorize the client and echo its messages. The reference fixture does this with a bounded queue; a normal `FDirectServer` does not echo automatically. See [Direct signaling](DIRECT_SIGNALING.md) and [Server routing](SERVER_ROUTING.md). Use HTTPS normally. For the local fixture only, `-SimurghRTCExampleLoopback` enables loopback HTTP and `-SimurghRTCSessionLoopbackProbe` selects loopback ICE.

The Output Log shows Connect, OpenChannel, CloseChannel and Disconnect completing. Missing configuration or a failed operation reports an explicit error. The sample is a small flow to inspect and adapt, with no additional UI.

## Follow the graph

Open **Examples → Direct → BP_RtcData**:

1. **BeginPlay:** prepare configuration, create and retain a session, bind message/channel/state events, then Connect.
2. **Connect success:** OpenChannel, save its returned handle, inspect its negotiated limits, then SendText and SendBytes.
3. **OnMessage:** verify each echo. After both arrive, CloseChannel and request Disconnect.
4. **Failure:** inspect the structured error and disconnect. **EndPlay:** call Shutdown immediately, without waiting for a callback.

The `TrackOperation` helper retains each async proxy and checks operation IDs. Async Blueprint nodes activate automatically. The native sample binds completion before calling `Activate()`. Both retain the session on their actor and use only the shared Core API for client operations.

The send calls here submit two small messages. Applications with larger queues should follow the [backpressure guidance](SHARED_SESSIONS.md#send-receive-and-handle-a-full-queue).

## Package and check

Use the [sample packaging steps](LIFECYCLE_EXAMPLES.md#package-the-examples), including `-cookdir="$PROJECT_DIR/Content/Examples"`, and select both data maps in the cook:

```sh
-map="/Engine/Maps/Entry+/Game/Examples/Direct/Maps/RtcDataCpp+/Game/Examples/Direct/Maps/RtcDataBlueprint"
```

Build a dedicated Server package as described in [Direct signaling](DIRECT_SIGNALING.md). Then run:

```sh
python3 tools/run_data_example_probe.py \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --client-project "$CLIENT_PROJECT" --client-executable "$CLIENT_EXE" \
  --output Intermediate/DataExampleProbe
```

Use a fresh output directory. The runner loads each cooked map in a separate Game process and checks five scenarios: exact echoes, Connect timeout, Disconnect during Connect, actor destruction during Connect, and actor destruction while OpenChannel is pending. It verifies the actual actor class, structured completion branches, game-thread events, immediate EndPlay shutdown, garbage collection and native closure. The pending-Connect cases wait until the HTTP fixture receives the request before triggering destruction or disconnect.

These checks use Mac ARM64 Development packages and explicit loopback networking. Mobile Blueprint flows, remote HTTPS, NAT/TURN and network media remain separate acceptance work.

Validated on **2026-10-29**: two fresh suites passed 20 packaged cases in total and recorded 20 native client closures, with no late callbacks or pending cleanup. All 27 Unreal and 181 Python tests pass; the existing shared-session and lifecycle checks also pass. The [validation record](reviews/SR106-blueprint-data-validation.json) includes package identities, asset hashes and detailed scope.

## Regenerate the assets

The three checked-in assets can be opened and cooked directly. Their source recipe is [ContractDataExamples.cpp](../Samples/ContractHost/Source/ContractHostEditor/ContractDataExamples.cpp). After building the Editor target, intentionally regenerate them in a staged project with:

```sh
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PROJECT" \
  -run=ContractExamples -DataExamples -ReplaceGeneratedExample \
  -unattended -NullRHI -nosound
```

Regeneration replaces the graph and the actor in each map. The generator checks every wire, verifies one sample actor per map, and requires the Blueprint compiler to report zero warnings and errors. Generated node IDs can change; binary-identical `.uasset` builds are not promised. Janus-only staging excludes the Direct sample directory.
