# GNUbase

Open-source (GPLv3) digital audio workstation for Windows, macOS and Linux. GNUbase closely follows the feature set and user experience of an established commercial DAW, called "the reference" below.

## Source of truth and naming
- The source of truth is the reference's Operation Manual for version 15 Pro (English, 1655 pages). It is not part of this repo. Tickets cite it as "Reference manual: pp. N". Read the cited pages before implementing a ticket; if you don't have the manual, ask a maintainer about them.
- Never write the reference product's or vendor's name anywhere public: code, comments, file names, commits, issues, wiki, docs. Use generic feature names, not the vendor's trademarked ones (say "mixer", not the branded name). The git hooks in `tools/git-hooks` enforce this with the private list in `.git/info/private-keywords`: install them once per clone with `tools/install-git-hooks.sh` and never bypass them with `--no-verify`. CI scans every pull request and every push to main against the same list (`.github/workflows/keyword-scan.yml`).
- One exception: the VST3 SDK's C++ namespace is the vendor's name, so the list allows that name in the content of files in `src/pluginhost/vst3/`, the VST3 adapter, and nowhere else: not in file names, commit messages, comments or prose, and not in any other folder. The SDK is the submodule `ext/vst3sdk`, from the GNUbase mirror. In the list, a keyword followed by a tab and folders separated by spaces is allowed in those folders' file content.
- Planning lives in GitHub milestones and issues: https://github.com/GNUbase/GNUbase. Basic functionality first, optional features later.

## Architecture rules (non-negotiable)
Details and reasoning (processes, modules, threads, engine API, IPC) are in [docs/architecture.md](docs/architecture.md). Read it before changing module boundaries, threads or the engine API. How each feature works goes into its own file in `docs/features/`, never into `architecture.md`, so that pull requests don't conflict there.

1. **Engine/UI separation.** The engine is a headless library with no UI dependency. The UI is an editor and event listener: it sends commands through the engine API and renders state from engine events. It never touches engine internals. Anything the UI can do must be possible through the engine API.
2. **Everything is testable and remote-controllable.** Every engine command is exposed by the built-in MCP server, generated from the command registry. UI features are reachable through the MCP UI tools: DOM tree, screenshots, input injection, audio capture, MIDI send/receive, plugin parameters.
3. **Third-party plugins never run inside GNUbase.** VST3, CLAP, AU (macOS), LV2 (Linux) and FST (VST2-compatible) plugins run in `gnubase-pluginhost` processes connected through shared-memory IPC. A crashing or hanging plugin must never take GNUbase down. How plugins are grouped into host processes is a user setting (shared, per vendor, per plugin, per instance); there is no in-process mode. Only GNUbase's own built-in processors run inside the engine.
4. **One internal plugin API.** Every format is adapted to the GNUbase plugin wrapper, which is modeled on CLAP's extension mechanism. Host code never talks to a plugin format directly. GNUbase-specific plugin features are added as wrapper extensions.
5. **Real-time safety.** On audio threads: no allocations, no locks, no blocking system calls, no file or network I/O, no exceptions, and no logging except through the RT-safe logger (`platform/log.h`). Use the lock-free queues in `src/platform/rt`. The RT-safety checker must stay green.
6. **Deterministic rendering.** The same project and input produce bit-identical output for any number of processing threads. Independent track chains run in parallel; busses sum their inputs in a fixed order once all are ready (#212).
7. **Project format is DAWproject** (`.dawproject`). GNUbase-only data goes into an extra entry inside the container, so other DAWproject applications can still open the file. Importing other project formats comes later.
8. **Lua is the scripting language.** Users automate GNUbase with Lua, and we write integration and UI tests in it. Lua bindings are generated from the engine command registry, like the MCP tools, so both always cover the same commands. The UI uses RmlUi's Lua plugin on the same runtime. Lua never runs on audio threads, and scripts that come with a project never run without the user's consent.
9. **Docking UI.** Windows are built from zones, tab groups and splitters modeled on the reference: an always-visible main zone, switchable left, right and lower zones, panels that can be docked or opened as their own window. Layouts are serializable for workspaces.
10. **Multi-monitor.** GNUbase works across several monitors through separate top-level windows: the mixer, the editors and other panels open in windows of their own, e.g. the mixer with a key command (F3 by default, as in the reference). Each window renders at its own monitor's scale and follows it when moved to another monitor. Workspaces save which windows are open, with their positions, sizes and monitors, and move windows whose monitor is gone onto a connected one.

## Platforms and dependencies
- C++17, CMake. 64-bit only: Windows x64/ARM64, macOS universal (arm64 + x86_64), Linux x86_64/aarch64.
- Window management: SDL3. UI: RmlUi with its Lua plugin (Lua 5.4). Both are GNUbase forks (github.com/GNUbase) included as git submodules in `ext/`.
- Text: FreeType with bundled free fonts (SIL OFL or Apache-2.0, e.g. from Google Fonts), license files shipped next to the fonts.
- Graphics: icons and UI graphics are SVG, rendered by LunaSVG through RmlUi's SVG plugin, so they stay sharp at every UI scale. Bitmaps only where unavoidable.
- Audio: ASIO and WASAPI (Windows), CoreAudio (macOS), PipeWire and JACK (Linux), plus a dummy device for tests and headless runs.
- Plugin bridges over the same IPC: 32-bit plugins (Windows, Linux), x86-64 plugins on Apple Silicon (Rosetta) and on Windows ARM64 (emulation).
- Third-party code: pinned git submodules in `ext/`, GPLv3-compatible licenses only, each listed in `docs/third-party.md`.
- Binary files (images, fonts, audio, video, executables, libraries, archives) are stored in Git LFS. Add a new binary type to `.gitattributes` before committing the first file of that type. Text formats such as SVG, RML, RCSS and Lua stay in plain git.

## UI conventions
- **Styling lives in RCSS, never in code.** C++ and Lua don't set colors, fonts, borders, spacing or any other style property. They change an element's state, and the stylesheets decide how each state looks: pseudo-classes (`:hover`, `:active`, `:focus`, `:checked`, `:disabled`) and state classes such as `.selected`, `.muted` or `.recording`. Code may only set geometry that comes from data, such as where an event sits in the timeline or a fader's position.
- **One design system.** Shared stylesheets give every widget its look in one place. Documents add layout, not a look of their own, so the whole UI follows one design.
- **Themes.** The overall look is a theme that can be switched at runtime: GNUbase default, dark and light to start with (#53). A theme is a set of stylesheets over the shared ones, and it owns every color, font and visual detail. Code and documents never refer to a particular theme.
- **Sizes in `dp`.** Lay out the UI in `dp` (RmlUi's density-independent pixels), not `px`: fonts, spacing, borders, icon and widget sizes. Scaling for HiDPI displays and a user-chosen UI scale then only changes the context's dp ratio (#48, #53). Use `px` only where exactly one device pixel is meant, such as hairline separators.

## Test-driven development
- Write the failing test first, then the code. No feature is done without tests.
- Catch2 v3 for C++ tests, Lua tests in `tests/lua/`, all run with `ctest`. Engine tests render offline through the dummy device and compare against golden files with tolerance.
- UI tests, in C++ or Lua, drive the real app through the same automation API the MCP server exposes.
- Tag every Catch2 test case with `[unit]`, `[integration]` or `[ui]`, plus `[slow]` for long ones. Tags become ctest labels: `ctest --preset <platform> -L unit`.
- Shared helpers live in `tests/testutils/`: `TempDir`, `compareAudio`, float WAV read/write and `compareWithGolden`. Golden files are `tests/golden/<name>.wav` (Git LFS). On a mismatch the actual and difference WAVs are written to the build's test output folder; run with `GNUBASE_UPDATE_GOLDEN=1` to accept new audio.
- A new or changed engine command needs a description and parameter and result schemas: MCP tools, Lua functions, `docs/lua.md` and the tool catalogue in `docs/mcp.md` are generated from them. Tests fail until both docs are regenerated: run the tests with `GNUBASE_UPDATE_DOCS=1` and commit them.
- Definition of done for every ticket: tests first and green in CI on all platforms, reachable through the engine API and MCP, UI only through the engine API, real-time rules respected, docs updated.

## Code conventions
- Every source file starts with the SPDX header `SPDX-License-Identifier: GPL-3.0-or-later`; CI checks every tracked file with `tools/check-spdx.sh`.
- Formatting: clang-format 22 with `.clang-format` (tabs, braces on their own line, 120 columns). Run `tools/format.sh` before committing; CI fails on unformatted code. Visual Studio 2026 ships clang-format and clang-tidy 22 in `VC/Tools/Llvm/x64/bin`.
- clang-tidy (`.clang-tidy`) runs in CI with warnings as errors. A finding is fixed, not silenced; `NOLINT` only with the check name and a reason.
- Naming, enforced by clang-tidy: types `PascalCase`, functions and variables `camelCase`, private and protected members `m_camelCase`, globals and namespace-level constants `g_camelCase`, macros `UPPER_CASE`.
- `#pragma once`, RAII, no raw `new`/`delete`, no `using namespace` in headers.
- Small, focused commits with tests green.
- CMake lists source files one per line, sorted, so pull requests that add files rarely conflict.
- Test locally before pushing; CI confirms the other platforms. Merge `main` into a branch only when it conflicts or you need something from it, since every push runs all of CI again. Resolve conflicts in `docs/lua.md` and `docs/mcp.md` by taking either side and running the tests with `GNUBASE_UPDATE_DOCS=1`.

## Repository layout (target)
```
src/engine      headless engine library (no UI dependencies)
src/ui          SDL3/RmlUi front end, talks to the engine API only
src/app         executable: engine + UI + MCP server, also --headless
src/mcp         MCP server
src/ipc         shared-memory IPC used by the engine and the plugin hosts
src/pluginhost  plugin host process, scanner and format adapters
src/platform    OS abstractions (audio/MIDI backends, threads), real-time primitives, logging, crash reports
src/scripting   Lua runtime and generated engine bindings
tests/          Catch2 tests (unit, integration, ui); tests/testutils helpers, tests/golden files, tests/lua for Lua tests
resources/      fonts (with license files), UI documents, SVG icons, themes/ (generated by tools/skin/generate.py)
art/            source artwork (Git LFS): icon/ program icon, splash/ loading banner (also in the README), social/ avatar
ext/            git submodules (SDL, RmlUi, Lua, FreeType, LunaSVG, Catch2, nlohmann/json, ...)
docs/           architecture and design notes
```
