# Canvas elements

What editors show on a timeline is drawn by five custom RmlUi elements (#55): `<event-lanes>`, `<waveform>`,
`<curve>`, `<fade-lines>` (#138) and `<ruler>`. They build geometry from data that code gives them and draw it through RmlUi's render
interface, so they work wherever the UI does (SDL_GPU on Metal, Direct3D 12 and Vulkan, [render-backend.md](
../render-backend.md)). The code is in `src/ui/canvas.h`. The event display (#84), the waveform cache (#83), the key
editor and the automation lanes fill them.

## Elements

| Element | Data (C++) | Draws |
|---|---|---|
| `<event-lanes>` | `setLanes`: lanes with a height in dp and their events (start, length and an optional color), by start | events as rectangles in `color`, or in their own color, which is user data such as an event's or its track's color ([colors.md](colors.md)), outlined in `stroke-color` or, for an own color, `own-color-tint`, `event-inset` off their lane's edges: the events of tracks, or a key editor's notes with one lane per pitch |
| `<waveform>` | `setPeaks`: a time span and peak columns per channel as `media.peaks` returns them | the channels one above the other in `color`, or in an own color with `own-color-tint` over it, `event-inset` off its lane's edges inside its padding; a pixel shows the lowest and highest sample of the columns it covers, silence a line one pixel high |
| `<curve>` | `setPoints`: points with a value from 0 to 1 and the shape of the segment that starts at them, by position | a line in `stroke-color`, `stroke-width` wide, level with the first point before it and with the last after it; steps, ramps and bent curves as the engine plays them (`engine::automationBend`) |
| `<fade-lines>` | `setLanes`: per lane its height in dp, the fades of its audio events (start, length, shape, in or out), their volume lines (start, end, volume, curve points in clip frames) and the positions of fade and event volume handles | each fade's curve as the engine plays it (`engine::fadeGain`) from the bottom of its lane to the top, and each volume line as the engine plays it (`engine::curveGain`) up to +6 dB at the top, in `stroke-color`, `stroke-width` wide; the handles as squares at the top of the lane and at the selected events' curve points, in `color` ([fades.md](fades.md), [volume-curves.md](volume-curves.md)) |
| `<ruler>` | `setScale`: how to divide the timeline and label it (`RulerScale`: the marks between two positions, labelled ones at least a given distance apart) | ticks in `stroke-color` along the bottom, labelled ticks to the top, labels in `color` and the element's font, at least their width and an em apart. The default counts in decimal steps of 1, 2 and 5; `ui/timeformat.h` has bars and beats, seconds, timecode and samples ([event-display.md](event-display.md)) |

- **Time axis.** Positions are in whatever units the code that fills an element uses: ticks, samples or seconds. The
  attributes `start` (the position at the left edge, default 0) and `scale` (units per dp, default 1) map them to the
  screen, and `scroll-top` (dp, default 0) scrolls `<event-lanes>` down. Attributes make the view visible to the MCP
  UI tools' DOM and settable from Lua; elements that show the same timeline get the same `start` and `scale`.
- **Looks.** Colors and line widths come from the style sheets, as for every widget ([themes.md](themes.md)): `color`,
  and the properties `stroke-color` and `stroke-width`, which `ui::RmlUi` registers when it starts, along with the
  elements, so every document has them. Each theme styles the four elements; a change of style or theme builds them
  again. Code only gives them data.
- **Colors that are user data** (an event's or its track's color, `TimeSpan::color`, #94) show as the style sheet says
  (#1312): `own-fill-opacity` (a number from 0 to 1, default 1) is how opaque an event's body is in its own color;
  `own-color-tint` (`none` by default, or a color) is laid over the own color by its alpha, for the event's outline
  and for a `<waveform>` with an own color, which otherwise use `stroke-color` and `color`; `event-inset` (a length,
  default 0) keeps `<event-lanes>` events that far off their lane's top and bottom, in lanes higher than twice the
  inset. The defaults draw as before, so only style sheets that ask change. The default theme tints the event
  display's events this way, after the project window mockup (`art/mockups/main-window.webp`).
  - **The tint outlines on its own** (#1332): an event with an own color is outlined in the tinted color whenever
    `own-color-tint` isn't `none`, `stroke-width` wide, even if `stroke-color` is transparent and events without a
    color have no outline. A tint asks for the own color's outline; a style sheet that wants none sets the tint to
    `none`. Events that reach past the build's edges end that width and a pixel outside it, so their outline there
    stays out of view.
  - **Waveforms take the inset themselves** (#1332): a `<waveform>` is as high as its lane, so it keeps `event-inset`
    off the lane's edges inside its padding, which the shared style sheet gives it (`2dp 0`), as `<event-lanes>` keeps
    events: not in lanes no higher than twice the inset, where events drop it, nor where it would leave the waveform
    no pixel. The themes set the same inset on events and waveforms, so the waveform stays inside the event's outline
    without padding of its own.
  - The element reads these properties once per build, not per event.
  - Fade and volume lines still span the whole lane, where the event display's handles take the mouse.
- **Pixels.** Positions snap to whole pixels and line widths round to whole pixels, so edges stay sharp at every dp
  ratio. What an element draws is clipped to its content box by scissor, which is screen-aligned: canvas elements
  can't sit inside a transform.

## Virtualized drawing

Thousands of events can't each be an element: RmlUi lays out, styles and draws every element in a document, in view
or not. A canvas element is one element that builds its geometry, and only for what is around the view:

- A build covers the view and one view's width and height on each side: three by three views. Scrolling within that
  only moves the geometry (one translation per frame, no new vertices), so scrolling builds once per view's width or
  height scrolled. A change of data, scale, size, dp ratio or style builds again.
- `<event-lanes>` finds the lanes in range by binary search over their tops, and in each lane the first event in range
  by binary search over the starts, from the frame's start minus the lane's longest event, so a build costs the events
  it draws, not the events there are. `<curve>` does the same over its points, `<waveform>` loops over the pixels in
  range, and `<ruler>` over its ticks.
- Vertices are relative to the build's corner, so they stay small at any position on a long timeline; positions are
  doubles until then.
- An event costs 12 vertices with an outline (a ring and a fill), 4 without; an event narrower than twice the outline is
  drawn in the outline's colour.

## Benchmark

`gnubase-canvas-bench` (`tests/bench/canvas_bench.cpp`, built with the tests, run by hand as it needs a display and a
GPU) draws 10,000 events on 100 tracks, every other one in a color of its own (#1332), under a ruler in a 1600 by 1000 window, with vsync and the default theme's event look, as the
app does. It runs three phases and prints the frames per second against the display's refresh rate and the frames that
came later than one and a half refreshes; it fails if a phase falls behind the refresh rate:

1. scrolling a view of a tenth of the project and 16 of the tracks, 4 dp a frame across and 2 dp down and up;
2. scrolling with all 10,000 events in view;
3. zooming with all 10,000 events in view, which builds every event every frame.

On a 180 Hz display (Release build, Direct3D 12, a desktop GPU of 2023 and a 16-core desktop CPU, 2026-10-30), every
phase keeps up with the display, including zooming, which builds all 10,000 events every frame:

| Phase | Frames per second | Frames later than 1.5 refreshes | Builds |
|---|---|---|---|
| Scrolling, zoomed in | 179.6 | 1 of 539 | 2 |
| Scrolling, all events in view | 180.1 | 0 of 541 | 1 |
| Zooming, all events in view | 179.0 | 1 of 538 | 538 |

Run it after changing the canvas elements or the render backend:

```bash
build/windows-x64/bin/Release/gnubase-canvas-bench
```

## Checks

- `tests/canvas_tests.cpp` (fake window system): the elements and their style properties are registered; events are
  filled and outlined in the style sheet's colors, which change with the style; of 10,000 events only those around the
  view are built, scrolling within the build only moves it, and zooming, resizing and new data build again; a waveform
  pixel shows the peaks of the columns it covers; own colors at `own-fill-opacity` and with `own-color-tint`, which
  outlines colored events also without a `stroke-color` and keeps them out of view past the build's edges, and
  `event-inset` on events and waveforms, also in lanes too low for it; a curve builds only the segments around the view; the ruler's
  decimal scale and its marks.
- `tests/window_tests.cpp` draws all four through SDL_GPU at dp ratios 1 and 2 and checks their pixels: positions,
  outlines, clipping at the box, the waveform's full scale and silence, the curve's ramp, step and level, the ruler's
  ticks and labels, and scrolling.
- `tests/theme_tests.cpp`: only the default theme sets `own-fill-opacity`, `own-color-tint` and `event-inset` to
  something other than their defaults, so the other themes draw events as before, and no theme pads waveforms.
- `tests/uiapp_tests.cpp` compares the project window with golden images: `event-display.png` with an uncolored part,
  `event-colors.png` with a colored audio event and its waveform, colored parts plain, selected and muted, and a part
  in the theme's look.
