# GNUbase

Open-source (GPLv3) digital audio workstation for Windows, macOS and Linux. GNUbase closely follows the feature set and user experience of an established commercial DAW, called "the reference" below.

## Source of truth and naming
- The reference's operation manual is in the maintainers' private resources folder `u:\resources\GNUbase`. It is not part of this repo. Tickets cite it as "Reference manual: pp. N". Read the cited pages before implementing a ticket.
- Never write the reference product's or vendor's name anywhere public: code, comments, file names, commits, issues, wiki, docs. Use generic feature names, not the vendor's trademarked ones (say "mixer", not the branded name). Local git hooks enforce this using `.git/info/private-keywords`; never bypass them with `--no-verify`.
- Planning lives in GitHub milestones and issues: https://github.com/GNUbase/GNUbase. Basic functionality first, optional features later.

## Architecture rules (non-negotiable)
1. **Engine/UI separation.** The engine is a headless library with no UI dependency. The UI is an editor and event listener: it sends commands through the engine API and renders state from engine events. It never touches engine internals. Anything the UI can do must be possible through the engine API.
2. **Everything is testable and remote-controllable.** Every engine command is exposed by the built-in MCP server, generated from the command registry. UI features are reachable through the MCP UI tools: DOM tree, screenshots, input injection, audio capture, MIDI send/receive, plugin parameters.
3. **Third-party plugins never run inside GNUbase.** VST3, CLAP, AU (macOS), LV2 (Linux) and FST (VST2-compatible) plugins run in `gnubase-pluginhost` processes connected through shared-memory IPC. A crashing or hanging plugin must never take GNUbase down. Only GNUbase's own built-in processors run inside the engine.
4. **One internal plugin API.** Every format is adapted to the GNUbase plugin wrapper, which is modeled on CLAP's extension mechanism. Host code never talks to a plugin format directly. GNUbase-specific plugin features are added as wrapper extensions.
5. **Real-time safety.** On audio threads: no allocations, no locks, no blocking system calls, no file or network I/O, no exceptions, and no logging except through the RT-safe logger. Use the engine's lock-free queues. The RT-safety checker must stay green.
6. **Project format is DAWproject** (`.dawproject`). GNUbase-only data goes into an extra entry inside the container, so other DAWproject applications can still open the file. Importing other project formats comes later.

## Platforms and dependencies
- C++17, CMake. 64-bit only: Windows x64/ARM64, macOS universal (arm64 + x86_64), Linux x86_64/aarch64.
- Window management: SDL3. UI: RmlUi. Both are GNUbase forks (github.com/GNUbase) included as git submodules in `ext/`.
- Audio: ASIO and WASAPI (Windows), CoreAudio (macOS), PipeWire and JACK (Linux), plus a dummy device for tests and headless runs.
- Plugin bridges over the same IPC: 32-bit plugins (Windows, Linux), x86-64 plugins on Apple Silicon (Rosetta) and on Windows ARM64 (emulation).
- Third-party code: pinned git submodules in `ext/`, GPLv3-compatible licenses only, each listed in `docs/third-party.md`.

## Test-driven development
- Write the failing test first, then the code. No feature is done without tests.
- Catch2 v3, run with `ctest`. Engine tests render offline through the dummy device and compare against golden files with tolerance.
- UI tests drive the real app through the same automation API the MCP server exposes.
- Definition of done for every ticket: tests first and green in CI on all platforms, reachable through the engine API and MCP, UI only through the engine API, real-time rules respected, docs updated.

## Code conventions
- `.clang-format` and `.clang-tidy` are authoritative once they exist. Until then: types `PascalCase`, functions and variables `camelCase`, member variables `m_camelCase`, `#pragma once`, RAII, no raw `new`/`delete`, no `using namespace` in headers.
- Small, focused commits with tests green.

## Repository layout (target)
```
src/engine      headless engine library (no UI dependencies)
src/ui          SDL3/RmlUi front end, talks to the engine API only
src/app         executable: engine + UI + MCP server, also --headless
src/mcp         MCP server
src/ipc         shared-memory IPC used by the engine and the plugin hosts
src/pluginhost  plugin host process, scanner and format adapters
src/platform    OS abstractions: audio/MIDI backends, threads
tests/          Catch2 tests (unit, integration, ui)
ext/            git submodules (SDL, RmlUi, Catch2, ...)
docs/           architecture and design notes
```
