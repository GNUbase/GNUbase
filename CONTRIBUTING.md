# Contributing to GNUbase

Thanks for helping. GNUbase is licensed under the [GPL-3.0-or-later](LICENSE), and contributions are accepted under the same license.

[CLAUDE.md](CLAUDE.md) holds the rules for everyone, human or AI: architecture rules, test-driven development and code conventions. This file covers the workflow around them. Read both, plus [docs/architecture.md](docs/architecture.md) before you change module boundaries, threads or the engine API.

## Before you start

- Pick an issue from the earliest open [milestone](https://github.com/GNUbase/GNUbase/milestones). Basic functionality comes first, optional features later.
- Claim it: check the open pull requests and branches, then comment on the issue that you're taking it. Don't change files that another open pull request is changing.
- Many issues cite the reference's manual ("Reference manual: pp. N"). It is private; if you need those details, ask in the issue.

## Setting up

Clone with submodules and install the Git hooks, as described in the [README](README.md#building):

```bash
git clone --recursive https://github.com/GNUbase/GNUbase.git
cd GNUbase
tools/install-git-hooks.sh      # on Windows, run it in Git Bash
```

Formatting and static analysis use clang-format 22.1.3 and clang-tidy 22.1.8, the versions CI pins. Visual Studio 2026 includes them in `VC/Tools/Llvm/x64/bin`; elsewhere `pipx install clang-format==22.1.3` and `pipx install clang-tidy==22.1.8` install them.

## Naming policy

GNUbase follows the feature set and workflow of an established commercial DAW, called "the reference". Never write the reference's product name, its vendor's name or any of its branded feature names anywhere public: code, comments, file names, commit messages, branch names, issues, pull requests, the wiki and docs. Say "the reference", and use generic feature names, for example "mixer" or "sampler track".

Two checks enforce this:

- **Git hooks:** `tools/install-git-hooks.sh` makes Git run `tools/git-hooks`. Maintainers keep the private keyword list in `.git/info/private-keywords`, and the hooks block commits whose files, file names or messages contain a keyword. Contributors don't have the list, so their hooks don't scan and CI does instead. Never bypass the hooks with `--no-verify`.
- **CI:** the keyword scan checks every pull request: added lines, file names, commit messages, title and description. If it fails, its log lists where the keywords are, without repeating them. Remove them with new commits, or edit the title or description.

One exception: the VST3 SDK's C++ namespace is a keyword, so the list allows it in file content under `src/pluginhost/vst3/`, the VST3 adapter, and nowhere else. A list entry does that with a tab and the folders after the keyword, e.g. `<keyword><TAB>src/pluginhost/vst3/`; `tools/keyword-scan.sh` describes the format. If you keep a copy of the list, update that entry the same way: the CI secret has it. File names, commit messages and pull request texts are still scanned there, and all other keywords stay blocked in that folder.

## Workflow

1. Create one branch per issue from an up-to-date `main`.
2. Write the failing test first and make sure it fails, then implement until it passes. Tag every Catch2 test case `[unit]`, `[integration]` or `[ui]`, plus `[slow]` for long ones. Shared helpers are in `tests/testutils/`.
3. Before committing, run `tools/format.sh` and build and test with your preset (see the [README](README.md#building)). clang-tidy must report nothing: configure with `-DGNUBASE_CLANG_TIDY=ON` to run it while compiling. Fix findings rather than silencing them; `NOLINT` only with the check name and a reason.
4. Make small, focused commits with a summary line and a body that explains why.
5. Open a pull request: say what changed and why, list what you deliberately left out, and add "Closes #N". Code, build and CI changes always go through a pull request.
6. CI must pass on every platform: builds and tests on Windows, macOS and Linux, sanitizers, clang-tidy, formatting and license headers, and the keyword scan. When a check fails, read its log and fix it with a new commit. Don't force-push a pull request branch.
7. Maintainers squash-merge, so `main` gets one commit per pull request.

An issue is done when its tests came first and pass in CI on all platforms, it is reachable through the engine API and the MCP server, the UI uses only the engine API, the real-time rules hold and the docs are updated.

## Rules that reviews check

These are summaries; [CLAUDE.md](CLAUDE.md) has the full rules.

- **Engine and UI stay separate.** The engine is a headless library. The UI only sends commands through the engine API and renders engine events. CMake fails if the engine links UI code.
- **Real-time safety.** On audio threads: no allocations, no locks, no blocking system calls, no file or network I/O, no exceptions, and logging only through the real-time logger. The RT checker in the tests must stay green.
- **Plugins never run inside GNUbase.** They run in separate `gnubase-pluginhost` processes.
- **Deterministic rendering.** The same project and input give bit-identical output with any number of threads.
- **License headers.** Every source file starts with `SPDX-License-Identifier: GPL-3.0-or-later`. CI checks this with `tools/check-spdx.sh`.
- **Third-party code** comes in as a pinned git submodule in `ext/` under a GPLv3-compatible license. List it in [docs/third-party.md](docs/third-party.md); [docs/dependencies.md](docs/dependencies.md) explains how to add and update submodules.
- **Binary files** (images, fonts, audio, archives) go into Git LFS. Add a new file type to `.gitattributes` before committing the first file of that type.
