# Third-party components

GNUbase is licensed under the GPL-3.0-or-later, so everything that goes into it must be under a license compatible with the GPLv3. This page lists every third-party component in the repository and its license. Add new components here in the same pull request that adds them; [dependencies.md](dependencies.md) explains how submodules are added and updated.

## Components

| Component | Where | Used for | In the app | License | License text |
|---|---|---|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | `ext/SDL` (GNUbase fork) | windows, input, graphics | yes, linked statically | zlib | `ext/SDL/LICENSE.txt` |
| [RmlUi](https://github.com/mikke89/RmlUi) | `ext/RmlUi` (GNUbase fork) | user interface (core and debugger) | yes, linked statically | MIT | `ext/RmlUi/LICENSE.txt` |
| [JSON for Modern C++](https://github.com/nlohmann/json) | `ext/json` | JSON in the engine API | yes, header-only | MIT | `ext/json/LICENSE.MIT` |
| [Catch2](https://github.com/catchorg/Catch2) | `ext/Catch2` | tests | no, only test programs | BSL-1.0 | `ext/Catch2/LICENSE.txt` |

Some of these bundle code from other projects under its own license:

| Bundled in | Component | Where | License |
|---|---|---|---|
| SDL | HIDAPI | `ext/SDL/src/hidapi` | BSD-3-Clause, GPL-3.0 or the original HIDAPI license, at the user's choice |
| SDL | yuv2rgb | `ext/SDL/src/video/yuv2rgb` | BSD-3-Clause |
| RmlUi | itlib and robin_hood containers | `ext/RmlUi/Include/RmlUi/Core/Containers` | MIT |
| RmlUi | Courier Prime Code font, embedded in the debugger | `ext/RmlUi/Source/Debugger` | SIL Open Font License 1.1 |

The JSON library's repository also contains files under Apache-2.0, BSD-3-Clause and GPL-3.0 (its `LICENSES` folder). The headers GNUbase includes are all MIT.

## Why these licenses are compatible

- **zlib, MIT, BSD-3-Clause and BSL-1.0** are permissive licenses that the Free Software Foundation lists as GPL-compatible.
- **GPL-3.0** is GNUbase's own license; HIDAPI is used under it or under BSD-3-Clause.
- **SIL Open Font License 1.1** is for fonts. It explicitly allows fonts to be "bundled, embedded, redistributed and/or sold with any software". GNUbase's rules already allow fonts under this license (CLAUDE.md, "Platforms and dependencies").

Binary releases must include the copyright notices and license texts of the MIT, BSD-3-Clause and SIL Open Font License components. zlib and BSL-1.0 only require them in source distributions, but shipping them too costs nothing.

## Not part of GNUbase

- **System libraries:** SDL uses the operating system's audio, graphics and input libraries (for example ALSA, PulseAudio, PipeWire, X11 and Wayland on Linux). They come with the operating system and are not shipped with GNUbase.
- **Build and CI tools:** CMake, Ninja, compilers, clang-format, clang-tidy, ccache and Git LFS are used to build and check GNUbase, not included in it.

## Planned

CLAUDE.md plans these components. Check the license of the exact release when one is added, and move it to the table above.

| Component | Used for | License |
|---|---|---|
| Lua 5.4 | scripting, UI scripts through RmlUi's Lua plugin | MIT |
| FreeType | text rendering | FreeType License or GPLv2, at the user's choice; the FreeType License is GPLv3-compatible |
| LunaSVG | SVG icons and graphics through RmlUi's SVG plugin | MIT |
| Fonts | user interface text | SIL Open Font License 1.1 or Apache-2.0, with the license file next to each font |
