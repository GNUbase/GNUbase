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

## Other dependencies

Used unchanged, pinned to an upstream release tag.

| Submodule | Upstream | Pinned at |
|---|---|---|
| `ext/Catch2` | [catchorg/Catch2](https://github.com/catchorg/Catch2) | `v3.16.1` |
| `ext/json` | [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` |
| `ext/dr_libs` | [mackron/dr_libs](https://github.com/mackron/dr_libs) | `wav-0.14.5` (also has dr_flac 0.13.3 and dr_mp3 0.7.3) |
| `ext/stb` | [nothings/stb](https://github.com/nothings/stb) | commit `003501f` (stb_vorbis 1.22); stb has no release tags |
| `ext/flac` | [xiph/flac](https://github.com/xiph/flac) | `1.5.0` |

`ext/json` and `ext/flac` are shallow submodules (`shallow = true` in `.gitmodules`): their full history is large and never needed. dr_libs tags each library separately, so the pin is the newest tag whose commit has the newest release of all three; stb has no tags, so it is pinned to a commit. In the JSON library, implicit conversions are off (`JSON_ImplicitConversions`), so values are read with `get<T>()`.

To update one, check out the new tag inside the submodule and commit the new submodule commit:

```bash
cd ext/Catch2
git fetch --tags
git checkout v3.17.0      # the new release tag
cd ../..
git add ext/Catch2
git commit -m "Update Catch2 to v3.17.0"
```

## Cloning

```bash
git clone --recursive https://github.com/GNUbase/GNUbase.git
```

In an existing clone:

```bash
git submodule update --init --recursive
```

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
