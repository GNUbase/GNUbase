# UI render backend

Decision note for #47. Status: decided, SDL_GPU (option 2); see [Decision](#decision). The prototype is in
`spikes/render-backend/` (its README says how to run it) and the `Render spike` workflow runs it on every platform.

## Question

RmlUi lays out and styles the UI, but it draws through a render interface that the application provides. GNUbase needs
one that works on every platform it supports (Windows x64 and ARM64, macOS on Apple Silicon and Intel, Linux x86_64 and
aarch64), across several windows on monitors with different scales (CLAUDE.md, rule 10), sharp at every UI scale, fast
with hundreds of meters and long waveforms, and able to render offscreen for screenshots, which the MCP UI tools and UI
tests need (#58, #59).

## Candidates

All three are in RmlUi 6.3's own `Backends` folder, so none has to be written from scratch:

- **OpenGL 3.3 core** (`RmlUi_Renderer_GL3`).
- **SDL_GPU** (`RmlUi_Renderer_SDL_GPU`): SDL3's GPU API, which runs on Metal on macOS, Direct3D 12 on Windows and
  Vulkan on Linux (and on Windows if asked). GNUbase already builds SDL3.
- **Vulkan** (`RmlUi_Renderer_VK`), with MoltenVK on macOS.

Left out: RmlUi's Direct3D 11 and 12 renderers (Windows only, so a second backend would be needed everywhere else), its
OpenGL 2 renderer (no core profile on macOS) and SDL_Renderer (no transforms or effects).

## What the prototype shows

The `features` document has one tile per thing a renderer has to draw. Rendered on Linux with Mesa (llvmpipe for OpenGL,
lavapipe for Vulkan):

| Tile | OpenGL 3 | SDL_GPU |
|---|---|---|
| Shapes, borders, rounded corners | yes | yes |
| Linear and radial gradients | yes | missing |
| Box shadows, outer and inset | yes | wrong: a white box, and a stray black box at the window's corner |
| Blur and drop-shadow filters | yes | missing |
| Children clipped by rounded corners | yes | clipped to the rectangle only |
| Gradient mask | yes | missing |
| Transforms | yes | yes |
| Meters with gradient levels | yes | missing: gradients |
| Waveform as geometry, waveform as texture | yes | yes |

The reason is the renderers, not the APIs: RmlUi's GL3 renderer implements the whole render interface (layers and
compositing, filters, shaders for gradients, clip masks through the stencil buffer, masks), while its SDL_GPU and Vulkan
renderers implement only geometry, textures, scissoring and transforms. RmlUi skips what a renderer lacks, and box
shadows, which it draws through layers, come out wrong.

Results on every platform: see [CI results](#ci-results). Since #285, SDL_GPU draws every tile: see
[Effects in SDL_GPU](#effects-in-sdl_gpu-285).

## CI results

The `Render spike` workflow on GitHub's runners, which have no GPUs: Linux uses Mesa, Windows WARP (Direct3D 12's
software rasterizer), macOS a virtual GPU. Timings are milliseconds per frame for the benchmark document, mean over 300
frames without vsync; they show what runs where, not how fast real GPUs are.

| Runner | OpenGL 3 | SDL_GPU |
|---|---|---|
| Linux x64 | llvmpipe, OpenGL 4.5: all tiles, 31 ms | Vulkan on lavapipe: 38 ms |
| Linux arm64 | llvmpipe, OpenGL 4.5: all tiles, 53 ms | Vulkan on lavapipe: 44 ms |
| Windows x64 | no OpenGL 3 on the runner; with Mesa's `opengl32.dll`: all tiles, 58 ms | Direct3D 12 on WARP: 41 ms; Vulkan: no driver |
| Windows ARM64 | no OpenGL 3 on the runner, and Mesa's build didn't run | Direct3D 12 on WARP: 19 ms; Vulkan: no driver |
| macOS Intel | Apple's software renderer, OpenGL 4.1: blur missing, shadows wrong, 79 ms | Metal refused: the virtual GPU doesn't meet SDL_GPU's requirements |
| macOS Apple Silicon | Apple's software renderer, OpenGL 4.1: blur missing, shadows wrong, 58 ms | Metal: 10 ms |

- SDL_GPU draws the same tiles on Direct3D 12, Metal and Vulkan, with the same gaps as on Linux: the renderer, not the
  API, decides what is drawn.
- On macOS, OpenGL doesn't use the virtual GPU at all, and Apple's software renderer gets RmlUi's layers and filters
  wrong. Whether Apple's hardware OpenGL draws them right needs a real Mac.
- On Windows, OpenGL exists only through the GPU vendor's driver; without one there is OpenGL 1.1, and the spike
  crashes instead of reporting it (it doesn't check the version it got).

## Criteria

**Features.** GNUbase's look (#53) will use gradients for meters and faders, shadows for windows, menus and dragged
items, and rounded clipping for panels. With OpenGL 3 they work today. With SDL_GPU or Vulkan, GNUbase would first have
to implement layers, compositing with blend modes, the blur, drop-shadow and colour-matrix filters, gradient shaders
and clip masks in RmlUi's renderer: the GL3 renderer has about 2,200 lines for them, plus their shaders, which SDL_GPU
needs in three formats (SPIR-V, DXIL, MSL) compiled with SDL_shadercross at build time.

**Platforms.**

- Windows x64: all three. OpenGL and Vulkan come with every desktop GPU driver; SDL_GPU uses Direct3D 12.
- Windows ARM64: SDL_GPU uses Direct3D 12, which every ARM64 device has. Vulkan comes with Qualcomm's drivers. OpenGL
  comes from Microsoft's compatibility pack (OpenGL on top of Direct3D 12), which a device may need to install first.
- macOS: SDL_GPU uses Metal. OpenGL is deprecated since macOS 10.14 but still there, up to 4.1 core, on Intel and Apple
  Silicon. Vulkan needs MoltenVK in the app bundle.
- Linux: OpenGL everywhere. SDL_GPU and Vulkan need a Vulkan driver, which most GPUs of the last ten years have.

**HiDPI.** No difference. SDL3 gives each window's size in pixels and RmlUi scales with the context's dp ratio; the
prototype's screenshots at dp ratio 2 are as sharp as at 1 with both backends.

**Several windows** (rule 10). SDL_GPU claims any number of windows for one device, each with its own swapchain, so every
window presents on its own schedule and monitor. OpenGL shares one context between windows by making it current for
each in turn; with vsync on, each window's swap waits for its monitor's refresh, so the windows have to be presented from
separate threads or with vsync on one window only. Both work; SDL_GPU is simpler here.

**Offscreen rendering.** SDL_GPU renders into any texture and downloads it with a fence, without a window; the prototype
does that for every screenshot. OpenGL reads the frame back with `glReadPixels`; the GL3 renderer composites into the
window's framebuffer, so screenshots of hidden or covered windows need it to composite into a framebuffer object
instead, a small change in GNUbase's RmlUi fork.

**Performance.** RmlUi's cost is mostly on the CPU: layout, and building geometry for what changed. Both renderers
submit the same geometry in the same order, so neither is faster by design. The benchmark (256 meters
moving and 48 waveforms scrolling every frame, solid levels so both draw the same) runs on both; software rasterizers on
CI runners can't compare GPUs, so the numbers there compare CPU cost. Custom elements for waveforms, MIDI notes and
curves (#55) build their geometry from data on either backend; drawing them with their own shaders needs GLSL with
OpenGL, and HLSL compiled to three formats with SDL_GPU.

**Testing.** UI tests compare screenshots on every platform (#59). SDL_GPU runs on the Linux, Windows (x64 and ARM64)
and Apple Silicon runners with what the runner has. OpenGL 3 needs Mesa downloaded on Windows, where no ARM64 build ran,
and draws filters and shadows wrong on the macOS runners.

**Future.** OpenGL gets no new features and Apple could remove it from macOS one day, though nothing is announced. SDL_GPU
is maintained with SDL, which GNUbase depends on anyway, and is native on every platform.

## Options

1. **OpenGL 3 now.** Everything works today; M04 (#48, #49) can start at once. Risks: OpenGL's future on macOS, and
   Windows ARM64 devices without the compatibility pack.
2. **SDL_GPU now.** Native everywhere, simplest for several windows and offscreen rendering, and testable on the CI
   runners. The missing features (weeks of renderer work and a shader toolchain) become their own ticket. The first
   M04 tickets don't need them: windows (#48), RmlUi with text (#49), the engine binding (#50) and the MCP UI tools (#58)
   draw geometry and textures. The theme (#53) and the widgets (#54) need gradients, shadows and rounded clipping, so
   they wait for that ticket.
3. **OpenGL 3 now, SDL_GPU later.** Start with OpenGL 3 behind one GNUbase class that owns the RmlUi render interface,
   windows and screenshots, so nothing else knows which API draws. Port the missing features to SDL_GPU's renderer in
   GNUbase's RmlUi fork as a separate ticket (and offer them upstream), and switch once it passes the same screenshot
   tests.

## Recommendation

Option 2, with the render code behind one GNUbase class either way, so nothing else knows which API draws. The CI
results tip it: SDL_GPU runs on the native API of every platform GNUbase supports, including Windows ARM64, and on five
of the six CI runners as they are, while OpenGL 3 draws everything on CI only on Linux and, with Mesa, on Windows x64,
and depends on an API Apple deprecated. Its one gap, RmlUi's effects, is work GNUbase can do once, in its RmlUi fork, while M04 starts with the
windows and the RmlUi integration; the theme waits for it. If the effects are needed sooner than that work can be done,
option 3 is the fallback.

## Decision

SDL_GPU, option 2, decided by the maintainers on 2026-10-29:

- GNUbase draws its UI through SDL_GPU: Metal on macOS, Direct3D 12 on Windows, Vulkan on Linux. One GNUbase class owns
  the RmlUi render interface, the windows' swapchains and offscreen screenshots, so nothing else knows which API draws
  (#48, #49).
- RmlUi's effects (layers, filters, gradients, clip masks and masks) are ported to its SDL_GPU renderer in GNUbase's
  RmlUi fork: #285. The theme (#53) and the widgets (#54) wait for it.
- OpenGL 3 stays only in the spike.

## Effects in SDL_GPU (#285)

GNUbase's RmlUi fork gives the SDL_GPU renderer (`ext/RmlUi/Backends/RmlUi_Renderer_SDL_GPU.cpp`) the rest of RmlUi's
render interface, ported from the OpenGL 3 renderer: clip masks, layers composited with blend modes, the filters
(opacity, blur, drop shadow, and the colour matrices brightness, contrast, grayscale, hue-rotate, invert, saturate and
sepia), mask images, and linear, radial and conic gradients, repeating or not. Box shadows come from layers and
filters, so they work too.

- **Design.** As in the OpenGL 3 renderer: the frame is drawn into multisampled layers that share a stencil buffer
  (the clip masks); filters work on four postprocess textures that the layers are resolved into; at the end the base
  layer is blended onto the window's texture. RmlUi's calls only record commands, and `EndFrame` runs them after one
  copy pass that uploads the frame's geometry, so drawing and uploads don't take turns.
- **Multisampling.** Two samples per pixel by default, as the OpenGL 3 renderer (`ui::g_defaultMsaaSamples`;
  `GpuRenderer` and the renderer take another count). A device without that count takes the next larger one it has:
  Mesa's lavapipe has four but not two. The UI tests ask for four, which every API has, with the same sample positions
  everywhere, and which Mesa used for the references.
- **Shaders.** One HLSL source per shader in `ext/RmlUi/Backends/RmlUi_SDL_GPU`, compiled ahead of time into SPIR-V,
  DXIL and MSL and committed as C arrays in `ShadersCompiledSPV.h`, as upstream does. Building GNUbase needs no shader
  tools, and every platform runs the same compiled shaders. Compiling them at build time instead would mean building
  SDL_shadercross with DXC and SPIRV-Cross on every platform and CI runner, for files that rarely change. Whoever
  changes a shader regenerates the header with SDL_shadercross (built from its repository with DXC 1.9.2602) and
  commits both:

  ```bash
  python3 ext/RmlUi/Backends/RmlUi_SDL_GPU/compile_shaders.py path/to/shadercross
  ```

- **Tests.** `RmlUi's effects look as RmlUi's OpenGL 3 renderer draws them` (`tests/window_tests.cpp`) draws each tile
  of the spike's `features` document at dp ratios 1 and 2 and compares it with the OpenGL 3 renderer's drawing of it
  (`tests/golden/ui/effects`, cut from the spike's screenshots by `spikes/render-backend/reference-tiles.py`), averaged
  over 3x3 pixels within 16 of 255: OpenGL draws bottom-up, so its samples cover edges from the other side. Blurs
  downscale in steps that round to even pixels, so the test puts each tile as far from the window's corner, modulo 64
  pixels, as it is in the references.
- **Benchmark** (the spike's benchmark document, milliseconds per frame, mean over 300 frames; Vulkan on lavapipe with
  four CPU cores, three to four runs each):

  | Renderer | ms per frame |
  |---|---|
  | RmlUi 6.3's SDL_GPU renderer | 31 to 44, mean 38 |
  | The port, without multisampling (`--msaa 1`) | 34 to 35 |
  | The port, multisampled (lavapipe: four samples) | 47 to 49 |
  | OpenGL 3 on llvmpipe, multisampled | 29 |

  Without multisampling, as RmlUi 6.3's renderer drew, the port is as fast or faster: the frame's uploads go in one copy
  pass. Multisampling costs lavapipe a third more, since it rasterizes on the CPU. The `Render spike` workflow times
  both on every platform.
- **lavapipe and the scissor.** Mesa's lavapipe (25.2) lets a scissored draw into a multisampled texture cover half the
  samples of the pixels left of and above the scissor rectangle. Fullscreen passes into the layers are therefore
  bounded by their geometry (the viewport) as well as by the scissor; otherwise they would pick up whatever an earlier
  frame left beside the region in the postprocess textures.
