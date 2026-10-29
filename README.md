<p align="center">
  <img src="art/splash/loadingbanner.png" alt="GNUbase" width="100%">
</p>

<p align="center">
  <a href="https://github.com/GNUbase/GNUbase/actions/workflows/ci.yml"><img alt="CI" src="https://img.shields.io/github/actions/workflow/status/GNUbase/GNUbase/ci.yml?branch=main&style=flat-square&label=CI"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=flat-square"></a>
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599c?style=flat-square&logo=cplusplus&logoColor=white">
  <img alt="Platform: Windows | macOS | Linux" src="https://img.shields.io/badge/platform-Windows_%7C_macOS_%7C_Linux-0078d4?style=flat-square">
  <a href="https://github.com/GNUbase/GNUbase/milestones"><img alt="Status: early development" src="https://img.shields.io/badge/status-early_development-ffe066?style=flat-square"></a>
</p>

# GNUbase

GNUbase is a free and open-source digital audio workstation for Windows, macOS and Linux. The goal is a complete professional environment for recording, editing, MIDI and mixing, with features and workflow that closely follow an established commercial DAW, built from scratch in C++.

> [!NOTE]
> GNUbase is in early development. The foundation is in place: the build and CI on all platforms, real-time primitives, the time model, logging and crash reports, and the engine's command API. It can't play or record audio yet. The roadmap lives in the [milestones](https://github.com/GNUbase/GNUbase/milestones) and [issues](https://github.com/GNUbase/GNUbase/issues).

## Goals

- **Crash-safe plugin hosting:** plugins run in separate host processes, so a crashing plugin never takes GNUbase down. 32-bit and Intel-only plugins work through bridges.
- **Plugin formats:** VST3, CLAP, AU (macOS), LV2 (Linux) and VST2-compatible plugins via FST.
- **Audio drivers:** ASIO and WASAPI on Windows, CoreAudio on macOS, PipeWire and JACK on Linux.
- **Open project format:** projects are stored as [DAWproject](https://github.com/bitwig/dawproject) files.
- **Scriptable:** automate GNUbase with Lua.
- **AI-ready:** a built-in MCP server lets AI agents drive and test everything, from the engine to the user interface.
- **Native 64-bit** on x86-64 and ARM64.

## Platforms

| Platform | Architectures | Compiler | Tested in CI on |
|---|---|---|---|
| Windows | x64, ARM64 | Visual Studio 2026 (MSVC) | Windows Server 2025, Windows 11 ARM |
| macOS 11 or later | Universal binary (arm64 + x86_64) | Apple Clang | macOS 15 |
| Linux | x86_64, aarch64 | GCC or Clang | Ubuntu 24.04 |

## Building

You need Git with [Git LFS](https://git-lfs.com), CMake 3.25 or later and a C++17 compiler. Install Git LFS before cloning, so that images and other binary files are downloaded (otherwise run `git lfs pull` afterwards).

```bash
git clone --recursive https://github.com/GNUbase/GNUbase.git
cd GNUbase
git submodule update --init --checkout ext/vst3sdk     # the VST3 SDK, without its docs and GUI library
git -C ext/vst3sdk submodule update --init base cmake pluginterfaces public.sdk
tools/install-git-hooks.sh      # on Windows, run it in Git Bash
```

The install script sets up the repository's Git hooks; [CONTRIBUTING.md](CONTRIBUTING.md) explains them.

**Windows:** install Visual Studio 2026 with the "Desktop development with C++" workload, which includes CMake. Use the preset `windows-x64` or `windows-arm64`. From a developer prompt of the same architecture, `windows-x64-ninja` and `windows-arm64-ninja` build with Ninja instead, as CI does, and take a compiler cache such as sccache (`-DCMAKE_CXX_COMPILER_LAUNCHER=sccache`).

**macOS:** install Xcode or its command line tools, plus CMake and Ninja (`brew install cmake ninja`). Use the preset `macos`.

**Linux:** install Ninja and the libraries SDL needs for audio, video and input, and a Vulkan driver, which the UI draws through. On Ubuntu 24.04 that is the `LINUX_PACKAGES` list in [`.github/workflows/ci.yml`](.github/workflows/ci.yml), plus `pkg-config`; `mesa-vulkan-drivers` includes a software driver, so the window tests also run on machines without a GPU:

```bash
sudo apt-get install pkg-config ninja-build ccache \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libdbus-1-dev libibus-1.0-dev libudev-dev libfribidi-dev libthai-dev \
  libvulkan1 mesa-vulkan-drivers
```

Use the preset `linux-gcc` or `linux-clang`.

Then configure, build and test with your preset, here `linux-gcc`:

```bash
cmake --preset linux-gcc
cmake --build --preset linux-gcc --config Release
ctest --preset linux-gcc -C Release
```

The programs end up in `build/<preset>/bin/<configuration>/`. Add `-L unit` to the `ctest` command to run only the unit tests; the other labels are `integration`, `ui`, `slow` and `lua` (the Lua tests in `tests/lua`, see [docs/lua.md](docs/lua.md#tests)). The `ui` tests open hidden windows and draw them through SDL_GPU (Metal, Direct3D 12 or Vulkan); on Linux they use SDL's offscreen video driver, so they need no display. A machine without SDL_GPU skips them.

`gnubase` opens the main window, which stays empty until the UI takes shape in milestone M04.

## Headless mode and MCP

`gnubase --headless` runs the engine without UI and lets MCP clients, such as AI agents, use every engine command. By default it speaks MCP over stdin and stdout, so an agent can start it directly, for example in Claude Code:

```bash
claude mcp add gnubase -- /path/to/gnubase --headless
```

`--mcp-http` serves MCP over HTTP on 127.0.0.1 instead and prints the URL and the access token. `--script file.lua` runs a Lua script instead of serving MCP, with every engine command as a function, e.g. `gnubase.track.add{type = "audio"}` (reference: [docs/lua.md](docs/lua.md)); a script that raises an error, or whose test cases fail (`gnubase.test`), ends with exit code 4. MCP clients run scripts with the tool `script_run`. `--project FILE` loads a DAWproject file first, and `--sample-rate` and `--block-size` set up the audio device, which is the dummy device (no hardware) for now. `gnubase --help` lists everything. Exit codes: 0 done, 1 unexpected error, 2 invalid command line, 3 the audio device or the project could not be set up.

[docs/mcp.md](docs/mcp.md) explains how to connect Claude Code and other MCP clients, shows example sessions and the security model, and lists every tool.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) and [CLAUDE.md](CLAUDE.md) first: they hold the workflow, the architecture rules and the conventions for everyone, human or AI. Then pick an issue from the earliest open milestone.

## License

GNUbase is free software, licensed under the [GNU General Public License v3.0 or later](LICENSE). Third-party components and their licenses are listed in [docs/third-party.md](docs/third-party.md).
