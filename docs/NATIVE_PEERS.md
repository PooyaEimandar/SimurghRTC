# Asynchronous native peer ownership

SR-101 is in progress. The plugin can create and close an idle native PeerConnection on a dedicated worker. SDP, ICE exchange and network connections are still unfinished. The shared session API continues to return `NotImplemented` for connection attempts.

`FNativePeer` is an internal integration handle for future Direct/Janus providers. It does not change API v1 or add Blueprint nodes. The existing M0 factory and media probes keep their synchronous test paths.

## Lifetime rules

Call `FNativePeer::Open` on Unreal's game thread with a live UObject owner, a factory mode and an optional completion callback. Keep the returned handle. Invalid input, an unavailable backend or exhausted capacity returns a null handle and an error immediately.

An accepted request creates the factory and peer on one native worker. Its completion runs on a later game tick with `Ready` or `Failed`. **Ready means a local peer exists**, not that it has connected to another endpoint. The callback receives copied diagnostics; it never receives a native pointer.

Call `Close` from `EndPlay` or your owner's shutdown method, then release the handle. Close returns without waiting for WebRTC. It suppresses an undelivered completion and changes the state to `Closing`; `Closed` confirms that native teardown has finished. Calling Close again is safe. Dropping the last handle also requests close. The service detects lost UObject owners each tick and closes peers when their world is cleaned up.

Use the handle, including its destructor, only on the game thread. The module retains no strong reference to its UObject owner or handle. Completion captures are destroyed on the game thread. Avoid capturing owners strongly in callbacks when they should be allowed to disappear.

## Worker and shutdown

The first accepted peer starts one worker. All factory/peer operations and destruction stay on that same OS thread, as required by the native C interface. WebRTC still owns its internal signaling, network and media threads. At most 64 peer lifetimes can be pending; closing peers keep their slots until teardown is acknowledged and the registry is cleaned on a game tick.

Only module shutdown waits for the worker. It stops callback delivery, closes every peer, drains the command queue and joins the thread before unloading. Normal peer close and world cleanup do not wait. Release integration handles before unloading their module.

The synchronous `FNativeRuntime` adapter remains available for local probes. Its calls and destruction must use the thread that constructed it. Applications should use the asynchronous handle when normal gameplay must continue during teardown.

## Validation

Validated on **2026-10-24**, using Unreal 5.8.3, the Full profile and the accepted Mac ARM64 M155 artifact:

| Check | Result |
|---|---|
| Unreal Editor automation | 14 tests passed; no test warnings |
| Packaged Development Mac app | The same 14 tests passed; no test warnings |
| Source and fixture checks | 151 Python tests, profile policy and media fixtures passed |
| Package link and UHT audit | Expected modules, one libwebrtc archive, no Editor example generator; native and packaged executable IDs match |

The [validation record](reviews/SR101-ownership-validation.json) retains commands, reports, source/artifact hashes and engine state. Unrelated HTML5 test edits appeared in the shared engine checkout during the Game build; none were Mac build inputs, and this task did not change them.

Follow [Building](BUILDING.md) to stage a fresh project and run Unreal automation. The `SimurghRTC.Native.Async*` tests exercise actual M155 factories and peers, with a test gate that can hold the worker to check cancellation without timing guesses. They cover data/media overlap, repeated close, handle loss, UObject collection, world cleanup, bounded admission, reopening, callback reentrancy and shutdown drain.

For a packaged Mac check, follow the [Game build and packaging steps](LIFECYCLE_EXAMPLES.md#package-the-examples), then run the app's executable with:

```sh
REPORT="$HOME/Library/Containers/com.YourCompany.ContractHost/Data/Library/Application Support/Epic/ContractHost/Saved/Automation/Reports/SR101"
"$APP" -unattended -NullRHI -nosound \
  -ExecCmds="Automation RunTests SimurghRTC." \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$REPORT"
```

Set `APP` to the packaged `ContractHost.app/Contents/MacOS/ContractHost`. The default Mac sample is sandboxed, so write reports beneath its own Saved directory. If you change its bundle ID, use the Saved path shown in the runtime log. Check the JSON test report, including the number of tests performed; process exit alone does not establish a pass.

Mobile package validation of this new asynchronous path remains open. Earlier M0 package results cover the synchronous probes. M0's independent-host and deferred physical iOS checks also remain open.
