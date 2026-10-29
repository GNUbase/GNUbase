# Render backend spike (#47)

A throwaway prototype for choosing how GNUbase's UI renders: it shows RmlUi documents through each candidate render
backend, takes screenshots offscreen and times frames with many meters and waveforms. It is not part of GNUbase and is
built only on request. The decision, SDL_GPU, and its reasons are in [docs/render-backend.md](../../docs/render-backend.md).

Backends, both from RmlUi's own `Backends` folder (`ext/RmlUi`, RmlUi 6.3):

- `gl3`: OpenGL 3.3 core through `RmlUi_Renderer_GL3`, which implements RmlUi's whole render interface.
- `gpu`: SDL_GPU through `RmlUi_Renderer_SDL_GPU`, which uses Metal on macOS, Direct3D 12 on Windows and Vulkan on
  Linux, or another driver with `--gpu-driver`. Its renderer draws geometry and textures with scissoring and transforms
  only: no clip masks, layers, filters or shaders.

## Build and run

```bash
cmake --preset linux-gcc -DGNUBASE_RENDER_SPIKE=ON
cmake --build --preset linux-gcc --config Release --target gnubase-render-spike
build/linux-gcc/bin/Release/gnubase-render-spike --backend gl3
build/linux-gcc/bin/Release/gnubase-render-spike --backend gpu --screenshot features-gpu.png
build/linux-gcc/bin/Release/gnubase-render-spike --backend gpu --document benchmark --benchmark 300
```

Any wrong option prints the usage. Without `--screenshot` or `--benchmark` it opens a window until you
close it. `run-all.sh` runs every backend with both documents and writes the screenshots, logs and `summary.md`; the
`Render spike` workflow (`.github/workflows/render-spike.yml`) runs it on Linux, Windows and macOS for every pull request
that changes the spike and uploads the results as artifacts.

## Documents

- `features`: one tile per thing a backend has to draw, from left to right and top to bottom: shapes with borders and
  rounded corners, a linear and a radial gradient, box shadows (outer and inset), a blur filter, a drop shadow filter,
  children clipped by rounded corners, a gradient mask, a transform, meters with gradient levels, a waveform built as
  geometry, and a waveform drawn into a texture. There is no text until FreeType arrives (#207).
- `benchmark`: a mixer's meters (`--meters`, default 256) above an arrangement's waveforms (`--waveforms`, default 48).
  Every frame moves every meter and scrolls every waveform, which rebuilds its geometry, as during playback. The levels
  are solid, so both backends draw the same. The timings are milliseconds per frame without vsync: the mean over the
  whole run, including the GPU's work, the median and 95th percentile per frame, and the median CPU time until the
  frame is submitted.

On CI runners without a GPU, OpenGL and Vulkan come from Mesa's software rasterizers and Direct3D 12 from WARP, so the
timings there compare the backends' CPU cost more than real GPUs. Run the benchmark on your own machine for those.
