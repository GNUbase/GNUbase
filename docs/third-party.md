# Third-party components

GNUbase is licensed under the GPL-3.0-or-later, so everything that goes into it must be under a license compatible with the GPLv3. This page lists every third-party component in the repository and its license. Add new components here in the same pull request that adds them; [dependencies.md](dependencies.md) explains how submodules are added and updated.

## Components

| Component | Where | Used for | In the app | License | License text |
|---|---|---|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | `ext/SDL` (GNUbase fork) | windows, input, graphics | yes, linked statically | zlib | `ext/SDL/LICENSE.txt` |
| [RmlUi](https://github.com/mikke89/RmlUi) | `ext/RmlUi` (GNUbase fork) | user interface (core and debugger), and from its `Backends` folder the SDL input code and the SDL_GPU renderer | yes, linked statically | MIT | `ext/RmlUi/LICENSE.txt` |
| [JSON for Modern C++](https://github.com/nlohmann/json) | `ext/json` | JSON in the engine API | yes, header-only | MIT | `ext/json/LICENSE.MIT` |
| [dr_libs](https://github.com/mackron/dr_libs): dr_wav, dr_flac, dr_mp3 | `ext/dr_libs` | reading WAV, RF64, AIFF, FLAC and MP3 files | yes, compiled in | Unlicense (public domain) or MIT-0, at the user's choice | `ext/dr_libs/LICENSE` |
| [stb](https://github.com/nothings/stb) | `ext/stb` | `stb_vorbis` reads Ogg Vorbis files; `stb_image` reads image files for UI textures and `stb_image_write` writes screenshots as PNG | yes, compiled in | MIT or Unlicense (public domain), at the user's choice | `ext/stb/LICENSE` |
| [libFLAC](https://github.com/xiph/flac) | `ext/flac` | writing FLAC files | yes, linked statically | BSD-3-Clause | `ext/flac/COPYING.Xiph` |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | `ext/r8brain` | sample-rate conversion | yes, linked statically | MIT | `ext/r8brain/LICENSE` |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) | `ext/rubberband` (patched with `ext/patches/rubberband.patch`, see [dependencies.md](dependencies.md)) | time-stretching and pitch-shifting ([time-stretch.md](time-stretch.md)) | yes, linked statically | GPL-2.0-or-later | `ext/rubberband/COPYING` |
| [pugixml](https://github.com/zeux/pugixml) | `ext/pugixml` | reading and writing DAWproject's XML | yes, linked statically | MIT | `ext/pugixml/LICENSE.md` |
| [miniz](https://github.com/richgel999/miniz) | `ext/miniz` | reading and writing DAWproject's ZIP container | yes, linked statically | MIT | `ext/miniz/LICENSE` |
| [Lua](https://www.lua.org) | `ext/lua` | scripting | yes, linked statically | MIT | the end of `ext/lua/lua.h` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | `ext/cpp-httplib` | the MCP server's HTTP transport | yes, header-only | MIT | `ext/cpp-httplib/LICENSE` |
| [CLAP](https://github.com/free-audio/clap) | `ext/clap` | the CLAP plugin interface: test plugins now, the CLAP adapter of the plugin host later | no, only test plugins so far; header-only | MIT | `ext/clap/LICENSE` |
| [VST3 SDK](https://github.com/GNUbase/vst3sdk) | `ext/vst3sdk` (GNUbase mirror, [dependencies.md](dependencies.md#vst3-sdk)) | the VST3 plugin interfaces, for the VST3 adapter of the plugin host and the test plugins; the module loader and host classes, for the adapter | yes, in `gnubase-pluginhost`, linked statically (the interfaces and parts of `public.sdk`) | MIT | `ext/vst3sdk/LICENSE.txt` |
| [Catch2](https://github.com/catchorg/Catch2) | `ext/Catch2` | tests | no, only test programs | BSL-1.0 | `ext/Catch2/LICENSE.txt` |

Some of these bundle code from other projects under its own license:

| Bundled in | Component | Where | License |
|---|---|---|---|
| SDL | HIDAPI | `ext/SDL/src/hidapi` | BSD-3-Clause, GPL-3.0 or the original HIDAPI license, at the user's choice |
| SDL | yuv2rgb | `ext/SDL/src/video/yuv2rgb` | BSD-3-Clause |
| RmlUi | itlib and robin_hood containers | `ext/RmlUi/Include/RmlUi/Core/Containers` | MIT |
| RmlUi | Courier Prime Code font, embedded in the debugger | `ext/RmlUi/Source/Debugger` | SIL Open Font License 1.1 |
| Rubber Band Library | Speex resampler, KissFFT, SSE and NEON math functions, getopt, float_cast | `ext/rubberband/src/ext` | permissive licenses, in each folder; GNUbase's build compiles none of them (it uses Rubber Band's built-in FFT and resampler) |
| r8brain-free-src | Takuya Ooura's FFT | `ext/r8brain/fft4g.h` | free to use, copy, modify and distribute for any purpose; see the file's header |

The JSON library's repository also contains files under Apache-2.0, BSD-3-Clause and GPL-3.0 (its `LICENSES` folder). The headers GNUbase includes are all MIT. The FLAC repository also contains programs under GPL-2.0-or-later and documentation under GFDL; GNUbase builds only libFLAC. Of stb, GNUbase uses only `stb_vorbis.c`, `stb_image.h` and `stb_image_write.h`. r8brain-free-src also contains PFFFT (FFTPACK license), which GNUbase doesn't build: it uses the built-in Ooura FFT. Of the VST3 SDK, GNUbase fetches only `base`, `cmake`, `pluginterfaces` and `public.sdk`, each MIT with its own `LICENSE.txt`. It builds the interfaces in `pluginterfaces` and, from `public.sdk`, the module loader, the host classes and the string conversion ([dependencies.md](dependencies.md#vst3-sdk)); the SDK's GUI library, documentation and tutorials are not fetched.

## Why these licenses are compatible

- **zlib, MIT, BSD-3-Clause and BSL-1.0** are permissive licenses that the Free Software Foundation lists as GPL-compatible.
- **Unlicense and MIT-0** put the code in the public domain or allow everything without conditions, so they are compatible with any license.
- **GPL-3.0** is GNUbase's own license; HIDAPI is used under it or under BSD-3-Clause.
- **GPL-2.0-or-later** lets Rubber Band Library be used under the GPL-3.0, GNUbase's license. Its authors also sell it under a commercial license, which GNUbase doesn't need.
- **SIL Open Font License 1.1** is for fonts. It explicitly allows fonts to be "bundled, embedded, redistributed and/or sold with any software". GNUbase's rules already allow fonts under this license (CLAUDE.md, "Platforms and dependencies").

Binary releases must include the copyright notices and license texts of the MIT, BSD-3-Clause and SIL Open Font License components. zlib and BSL-1.0 only require them in source distributions, and the Unlicense and MIT-0 components need none, but shipping them too costs nothing.

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
