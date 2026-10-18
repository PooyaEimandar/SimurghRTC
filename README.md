# SimurghRTC

SimurghRTC is a C++ plugin for **Unreal Engine 5.8.3**. Its goal is to let an Unreal application send WebRTC messages, audio and video to either an Unreal server or Janus using the same C++ API and Blueprint nodes.

<img src="Logo.png" alt="SimurghRTC logo" height="256">

## Start here

| Step | Read |
|---|---|
| Build the plugin and run Unreal tests | [Building](docs/BUILDING.md) |
| Run the test data and local Janus/TURN setup | [Test fixtures](docs/FIXTURES.md) |
| See what is finished and what comes next | [Roadmap](ROADMAP.md) |
| Use or review the C++ and Blueprint API | [API contract](docs/API_CONTRACT.md) |
| Understand the design | [Architecture](docs/ARCHITECTURE.md) |

## Technical references

- [Native Server test](docs/NATIVE_PROBE.md): libwebrtc setup and headless HTTP checks.
- [Native media test](docs/MEDIA_PROBE.md): codecs, textures, render targets and audio playback integration.
- [Signaling examples](docs/SIGNALING_V1.md): the Direct draft and Janus message rules.
- [Mobile clients](docs/MOBILE_CLIENTS.md): separate ARM64 builds, device testing and app-size validation.
- [Demo matrix](docs/DEMO_MATRIX.md): planned coverage of the Janus examples.

## License

The project uses the [Apache 2.0 license](LICENSE). Engine and third-party components keep their own licenses.
