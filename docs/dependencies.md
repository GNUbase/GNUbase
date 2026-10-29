# Dependencies

Third-party code lives in `ext/` as git submodules. Only GPLv3-compatible licenses are allowed; [third-party.md](third-party.md) lists every component with its license. Add new ones there too.

## Forks

SDL and RmlUi are forked to the GNUbase account so we can carry our own changes.

| Submodule | Fork | Upstream | Branch | Pinned at |
|---|---|---|---|---|
| `ext/SDL` | [GNUbase/SDL](https://github.com/GNUbase/SDL) | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | `gnubase` | `release-3.4.18` |
| `ext/RmlUi` | [GNUbase/RmlUi](https://github.com/GNUbase/RmlUi) | [mikke89/RmlUi](https://github.com/mikke89/RmlUi) | `gnubase` | `6.3` |

Rules:
- GNUbase changes are commits on the fork's `gnubase` branch: small, focused and written so they could be sent upstream.
- The fork's default branch stays an untouched mirror of upstream.
- Never force-push `gnubase`. Older GNUbase commits must still be able to check out the submodule commit they pinned.
- The GNUbase repository pins an exact submodule commit. `branch = gnubase` in `.gitmodules` only matters for `git submodule update --remote`.

## Patched until forked

Rubber Band Library needs changes for real-time safety (see [time-stretch.md](time-stretch.md#real-time-safety)), but has no GNUbase fork yet. Until it has, the submodule points at the official repository, and the changes are one patch file that the build applies.

| Submodule | Upstream | Pinned at | Patch |
|---|---|---|---|
| `ext/rubberband` | [breakfastquay/rubberband](https://github.com/breakfastquay/rubberband) | `v4.0.0` | `ext/patches/rubberband.patch` |

- At configure time, `src/engine/CMakeLists.txt` copies Rubber Band's sources into the build folder and applies the patch with `git apply`, again whenever the patch or a file it changes is new. Configuring fails if the patch stops applying.
- To change the patch, apply it in `ext/rubberband` (`git apply ../patches/rubberband.patch`; add `--ignore-whitespace` where Git checks files out with CRLF line endings), edit, write the diff back (`git diff > ../patches/rubberband.patch`, keeping the header lines) and revert the submodule (`git apply -R ../patches/rubberband.patch`).
- When updating Rubber Band, check that the patch still applies, and drop the parts upstream has fixed.
- The rules above want a fork. Once a maintainer creates GNUbase/rubberband, the patch becomes commits on its `gnubase` branch, the submodule points at the fork, and the patch file and the copy step go.

## VST3 SDK

The VST3 SDK is used unchanged, from mirrors in the GNUbase account: the SDK's own repository names its vendor, which GNUbase never writes (CLAUDE.md, "Source of truth and naming"). `ext/vst3sdk` is [GNUbase/vst3sdk](https://github.com/GNUbase/vst3sdk), pinned at `v3.8.1_build_84`. The SDK is under the MIT license since `v3.8.0_build_66`. Its own submodules have relative URLs, so they resolve to the GNUbase mirrors too (`vst3_base`, `vst3_cmake`, `vst3_pluginterfaces`, `vst3_public_sdk`, ...).

GNUbase needs only `base`, `cmake`, `pluginterfaces` and `public.sdk`, not the SDK's documentation, tutorials or GUI library. So `.gitmodules` sets `update = none` for `ext/vst3sdk`, which a recursive update skips, and the SDK is fetched with two extra commands (see [Cloning](#cloning)). CMake stops with these commands if the SDK is missing, unless `GNUBASE_TEST_PLUGINS` is off: only the test plugins use it so far.

GNUbase uses only the interface headers in `pluginterfaces`, header-only, and implements the interfaces itself. The SDK's C++ namespace also names the vendor, so GNUbase code never writes it either: CMake reads it from `pluginterfaces/base/funknown.h` and passes it as the compile definition `GNUBASE_VST3_NS` (the `vst3-sdk` target), and `src/pluginhost/vst3/vst3ns.h` makes it available as `namespace vst`. Code includes that header and writes `vst::FUnknown`, `vst::Vst::IComponent` and so on.

To update the SDK, the maintainers update the mirrors first; then check out the new tag in `ext/vst3sdk`, update its four submodules and commit `ext/vst3sdk`.

## Other dependencies

Used unchanged, pinned to an upstream release tag.

| Submodule | Upstream | Pinned at |
|---|---|---|
| `ext/Catch2` | [catchorg/Catch2](https://github.com/catchorg/Catch2) | `v3.16.1` |
| `ext/json` | [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` |
| `ext/dr_libs` | [mackron/dr_libs](https://github.com/mackron/dr_libs) | `wav-0.14.5` (also has dr_flac 0.13.3 and dr_mp3 0.7.3) |
| `ext/stb` | [nothings/stb](https://github.com/nothings/stb) | commit `003501f` (stb_vorbis 1.22); stb has no release tags |
| `ext/flac` | [xiph/flac](https://github.com/xiph/flac) | `1.5.0` |
| `ext/r8brain` | [avaneev/r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | `version-6.5` |
| `ext/pugixml` | [zeux/pugixml](https://github.com/zeux/pugixml) | `v1.16` |
| `ext/miniz` | [richgel999/miniz](https://github.com/richgel999/miniz) | `3.1.2` |
| `ext/cpp-httplib` | [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | `v0.61.0` |
| `ext/lua` | [lua/lua](https://github.com/lua/lua) (the Lua team's repository) | `v5.4.9` |
| `ext/clap` | [free-audio/clap](https://github.com/free-audio/clap) | `1.2.10` |

`ext/json`, `ext/flac`, `ext/cpp-httplib` and `ext/lua` are shallow submodules (`shallow = true` in `.gitmodules`): their full history is large and never needed. dr_libs tags each library separately, so the pin is the newest tag whose commit has the newest release of all three; stb has no tags, so it is pinned to a commit. Lua is compiled as C++ (see the top-level `CMakeLists.txt`), so a Lua error unwinds C++ code with an exception and destructors run; include its C headers (`lua.h`, `lauxlib.h`, `lualib.h`), not `lua.hpp`. In the JSON library, implicit conversions are off (`JSON_ImplicitConversions`), so values are read with `get<T>()`.

To update one, check out the new tag inside the submodule and commit the new submodule commit:

```bash
cd ext/Catch2
git fetch --tags
git checkout v3.17.0      # the new release tag
cd ../..
git add ext/Catch2
git commit -m "Update Catch2 to v3.17.0"
```

## Runtime dependencies

What installers and packages have to ship or depend on, beyond the C++ runtime:

- **Linux: `libatomic.so.1`** (GCC's atomic library). Rubber Band asks whether `std::atomic<double>` is lock-free, which links `libatomic` where the compiler can't answer that inline, as Clang on Linux can't. `src/engine/CMakeLists.txt` adds it only when needed. Distribution packages depend on `libatomic1` (Debian, Ubuntu) or `libatomic` (Fedora); a self-contained bundle (AppImage, tarball) ships the library.

## Cloning

```bash
git clone --recursive https://github.com/GNUbase/GNUbase.git
cd GNUbase
git submodule update --init --checkout ext/vst3sdk
git -C ext/vst3sdk submodule update --init base cmake pluginterfaces public.sdk
```

In an existing clone:

```bash
git submodule update --init --recursive
git submodule update --init --checkout ext/vst3sdk
git -C ext/vst3sdk submodule update --init base cmake pluginterfaces public.sdk
```

The recursive update skips the VST3 SDK, so the last two commands fetch only the parts of it GNUbase needs (see [VST3 SDK](#vst3-sdk)).

## Changing a fork

```bash
cd ext/SDL
git switch gnubase
# edit, build, test, commit
git push origin gnubase
cd ../..
git add ext/SDL
git commit -m "SDL: <what changed>"
```

## Updating to a new upstream release

The example updates SDL; RmlUi works the same way with its own upstream URL and tag.

```bash
cd ext/SDL
git remote add upstream https://github.com/libsdl-org/SDL.git   # once per clone
git fetch upstream --tags
git switch gnubase
git merge release-3.4.20      # the new release tag; resolve conflicts, run all tests
git push origin gnubase
cd ../..
git add ext/SDL
git commit -m "Update SDL to release-3.4.20"
```

Then update the "Pinned at" column above. Merging (instead of rebasing) keeps every previously pinned commit reachable.

## Sending a change upstream

Create a branch from the upstream default branch, cherry-pick the commit from `gnubase`, push it to the fork and open a pull request against the upstream repository.
