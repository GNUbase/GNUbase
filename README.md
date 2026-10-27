<p align="center">
  <img src="art/loadingbanner.png" alt="GNUbase" width="100%">
</p>

<p align="center">
  <a href="https://github.com/GNUbase/GNUbase/actions/workflows/ci.yml"><img alt="CI" src="https://img.shields.io/github/actions/workflow/status/GNUbase/GNUbase/ci.yml?branch=main&style=flat-square&label=CI"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=flat-square"></a>
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599c?style=flat-square&logo=cplusplus&logoColor=white">
  <img alt="Platform: Windows | macOS | Linux" src="https://img.shields.io/badge/platform-Windows_%7C_macOS_%7C_Linux-0078d4?style=flat-square">
  <a href="https://github.com/GNUbase/GNUbase/milestones"><img alt="Status: planning" src="https://img.shields.io/badge/status-planning-ffe066?style=flat-square"></a>
</p>

# GNUbase

GNUbase is a free and open-source digital audio workstation for Windows, macOS and Linux. The goal is a complete professional environment for recording, editing, MIDI and mixing, with features and workflow that closely follow an established commercial DAW, built from scratch in C++.

> [!NOTE]
> GNUbase is in the planning phase; there is no code yet. The roadmap lives in the [milestones](https://github.com/GNUbase/GNUbase/milestones) and [issues](https://github.com/GNUbase/GNUbase/issues).

## Highlights

- **Crash-safe plugin hosting:** plugins run in separate host processes, so a crashing plugin never takes GNUbase down. 32-bit and Intel-only plugins work through bridges.
- **Plugin formats:** VST3, CLAP, AU (macOS), LV2 (Linux) and VST2-compatible plugins via FST.
- **Audio drivers:** ASIO and WASAPI on Windows, CoreAudio on macOS, PipeWire and JACK on Linux.
- **Open project format:** projects are stored as [DAWproject](https://github.com/bitwig/dawproject) files.
- **Scriptable:** automate GNUbase with Lua.
- **AI-ready:** a built-in MCP server lets AI agents drive and test everything, from the engine to the user interface.
- **Native 64-bit** on x86-64 and ARM64.

## Technology

C++17, CMake, [SDL3](https://github.com/libsdl-org/SDL), [RmlUi](https://github.com/mikke89/RmlUi) with Lua, FreeType and [LunaSVG](https://github.com/sammycage/lunasvg), Catch2. Development is test-driven.

## Contributing

Read [CLAUDE.md](CLAUDE.md) first: it holds the architecture rules and conventions for everyone, human or AI. Then pick an issue from the earliest open milestone.

## License

GNUbase is free software, licensed under the [GNU General Public License v3.0 or later](LICENSE).
