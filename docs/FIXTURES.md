# Run the test fixtures

Fixtures are fixed test inputs and local services that make a test repeatable. This set is version **sr005-v1**. It includes generated audio/video, signaling examples, Janus and coturn (a TURN relay server).

You can run the Python tests without Unreal or Docker. The live service tests need Docker. These checks do not enable the unfinished Unreal connection API.

## 1. Run the offline tests

Use Python 3.10 or newer. No extra Python packages are needed. Run from the repository root:

```sh
python3 -m unittest discover -s tools/tests -v
python3 tools/media_fixtures.py --check
```

The tests check message rules, fixture hashes, credential handling and network-packet parsing. To write the audio/video files to disk:

```sh
python3 tools/media_fixtures.py --output Intermediate/Fixtures/MediaV1
```

Choose a new or empty output directory. The tool checks the saved hashes before writing anything.

## 2. Run local Janus and TURN

Start Docker Engine and Compose, then run:

```sh
python3 tools/fixture_stack.py build
python3 tools/fixture_stack.py init
python3 tools/fixture_stack.py up
python3 tools/fixture_stack.py check --output Intermediate/Reports/ServicesV1
python3 tools/fixture_stack.py down
```

The first build downloads the pinned sources and packages. `init` creates local credentials and a Compose file under `LocalBuilds/Fixtures/`. It refuses to overwrite an existing setup.

For later runs, reuse that setup: run `up`, `check` and `down`. Give each check a new output directory. Rebuild only when the image inputs change, then run `up` again to recreate the services. Use `status` to see whether they are running.

A successful check prints `"passed": true` and the report path. The report folder contains:

| File | Contents |
|---|---|
| `result.json` | Results, exact image ID, versions and source/configuration hashes |
| `process.log` | Test output, without credentials |
| `debian-packages.txt` | Installed package versions |

Always run `down` when finished, including after a failed check. It removes the containers and network but keeps credentials and reports. The test runner removes its own temporary test container, even on timeout.

## What is tested

| Service | Checks |
|---|---|
| Janus 1.4.2 | Version and loaded plugins; rejected unauthenticated requests; session/handle creation; VideoRoom join; acknowledgment followed by a matching completion event; ICE completion; cleanup |
| coturn 4.18.0 | Rejected missing, wrong and expired credentials; authenticated relay allocation; permission creation; an exact binary message sent through the relay and returned; allocation release |

The Janus test uses HTTP and VideoRoom. WebSockets and the other selected plugins load, but their message/media flows are not tested. No Unreal client connects to Janus in this test. [Validation results](M0_VALIDATION.md) lists the runs completed on the available Mac.

## Network and credentials

The services and TURN test peer run on a private Docker network. Only Janus control ports are exposed to the host, on loopback:

- HTTP: `http://127.0.0.1:18088/janus`
- WebSocket: `ws://127.0.0.1:18188`

TURN and media ports stay inside Docker. A phone or Unreal application on the host cannot use this setup as its TURN endpoint. LAN/Internet connections, TLS/WSS and Unreal-to-Janus media need later deployment and transport work. The tests do not use a public demo or STUN server.

Credentials are generated into `LocalBuilds/Fixtures/secrets/`, which Git ignores. Directories use mode 0700 and secret files 0600. Credentials stay out of image layers, Compose environment entries, process arguments and test reports. Containers write their temporary service configuration under `/tmp`.

For CI, supply `SIMURGHRTC_FIXTURE_JANUS_SECRET` and `SIMURGHRTC_FIXTURE_TURN_SECRET` from your secret store when running `init`. Each value must contain 32–128 ASCII letters, digits, underscores or hyphens. The TURN test derives short-lived credentials from its shared secret.

To use a different setup directory, pass `--runtime PATH` to each command. Run one stack at a time: all generated setups use the Compose project name `simurghrtc-sr005`. Stop the old stack before switching directories. If the Compose generator changes, initialize a fresh directory; existing configuration is never overwritten automatically.

## Audio and video files

| File | Contents | Size |
|---|---|---|
| `tone.s16le` | One second of 440 Hz audio, 48 kHz, stereo PCM16 little-endian; identical channels, amplitude 8000 | 192,000 bytes |
| `tone.wav` | The same samples with a 44-byte WAV header | 192,044 bytes |
| `quadrants.bgra` | Twelve 64×64 BGRA8 frames with red, green, blue and yellow quadrants; alpha 255, brightness 160–204 | 196,608 bytes |

The [media manifest](../Fixtures/media/v1/manifest.json) records SHA-256 checksums, formats and timestamps. Video runs at 30 fps, with RTP timestamps starting at 90,000 and increasing by 3,000 per frame.

A saved integer tone table keeps the input samples identical across platforms. The generated `SimurghRTCMediaFixtureV1.h` supplies those samples and pixels to the Unreal codec test. Follow [Building](BUILDING.md) to run it in `Samples/ContractHost`; the [media guide](MEDIA_PROBE.md) covers texture and audio-mixer tests.

Hashes apply to the raw inputs. Encoded VP8/Opus output can vary with the codec build, so those tests check frame/sample counts, timestamps and quality instead.

## Versions and maintenance

The [service lock file](../Fixtures/services/sources.lock.json) stores exact commits, archive checksums, the Debian image digest and package snapshot. The Docker build verifies these inputs.

| Setting | Value |
|---|---|
| Janus | Multistream v1.4.2 |
| coturn | 4.18.0 |
| Base/packages | Debian trixie-slim; snapshot `20261016T000000Z` |
| Janus plugins | EchoTest, VideoCall, TextRoom, VideoRoom, AudioBridge, Streaming |
| Janus transports | HTTP and WebSockets; Admin APIs disabled |
| Rooms | VideoRoom, AudioBridge and TextRoom use 9001; Streaming has no external sources |
| Internal ports | Janus RTP 20000–20031; TURN UDP 3478 and relays 49160–49175 |

These pins make the build inputs repeatable. Image bytes may still differ because of architecture and build timestamps. The test runner rejects stale source/configuration hashes or services using a different image, and limits a test run to 150 seconds.

Keep released fixture versions unchanged. When samples or expected message behavior change, add a new version and keep the older tests. Use `media_fixtures.py --refresh` only when deliberately preparing a fixture version. [Signaling v1](SIGNALING_V1.md) describes the message examples and their manifest.

<details>
<summary>Build notes and upstream references</summary>

The development image includes compilers and test tools. It uses a read-only root filesystem, temporary `/tmp`, dropped capabilities and limits on CPU, memory, processes and open files.

The entrypoint binds Janus WebSockets and coturn to each container's IPv4 address. In this Janus release, `ws_ip=0.0.0.0` was rejected; selecting only `eth0` stalled startup on the tested host. The image also sets execute permission on coturn's installed binary. No upstream or Unreal source patches were needed. More troubleshooting evidence is in [M0 validation](M0_VALIDATION.md).

Janus's source-archive build reports `not-a-git-repo`; its verified archive and lock file identify the commit. The image retains licenses at `/opt/fixture/JANUS-COPYING` and `/opt/fixture/COTURN-LICENSE`. See [fixture notices](../Fixtures/NOTICE.md).

References: [Janus pinned changelog](https://github.com/meetecho/janus-gateway/blob/0a24110ae55a172c4293749b763dbb66a138f9ec/CHANGELOG.md), [coturn 4.18.0](https://github.com/coturn/coturn/releases/tag/4.18.0), [STUN test vectors](https://www.rfc-editor.org/rfc/rfc5769.html), [STUN protocol](https://www.rfc-editor.org/rfc/rfc8489.html), [TURN protocol](https://www.rfc-editor.org/rfc/rfc8656.html).

The Python TURN client tests IPv4/UDP packets only. It is a test helper, not the application's network transport.

</details>
