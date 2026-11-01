# Canvas elements

What editors show on a timeline is drawn by four custom RmlUi elements (#55): `<event-lanes>`, `<waveform>`,
`<curve>` and `<ruler>`. They build geometry from data that code gives them and draw it through RmlUi's render
interface, so they work wherever the UI does (SDL_GPU on Metal, Direct3D 12 and Vulkan, [render-backend.md](
../render-backend.md)). The code is in `src/ui/canvas.h`. The event display (#84), the waveform cache (#83), the key
editor and the automation lanes fill them.

## Elements

| Element | Data (C++) | Draws |
|---|---|---|
| `<event-lanes>` | `setLanes`: lanes with a height in dp and their events (start and length), by start | events as rectangles in `color`, outlined in `stroke-color`: the events of tracks, or a key editor's notes with one lane per pitch |
| `<waveform>` | `setPeaks`: a time span and peak columns per channel as `media.peaks` returns them | the channels one above the other in `color`; a pixel shows the lowest and highest sample of the columns it covers, silence a line one pixel high |
| `<curve>` | `setPoints`: points with a value from 0 to 1 and the shape of the segment that starts at them, by position | a line in `stroke-color`, `stroke-width` wide, level with the first point before it and with the last after it; steps, ramps and bent curves as the engine plays them (`engine::automationBend`) |
| `<ruler>` | `setScale`: how to divide the timeline and label it (`RulerScale`: the marks between two positions, labelled ones at least a given distance apart) | ticks in `stroke-color` along the bottom, labelled ticks to the top, labels in `color` and the element's font, at least their width and an em apart. The default counts in decimal steps of 1, 2 and 5; `ui/timeformat.h` has bars and beats, seconds, timecode and samples ([event-display.md](event-display.md)) |

- **Time axis.** Positions are in whatever units the code that fills an element uses: ticks, samples or seconds. The
  attributes `start` (the position at the left edge, default 0) and `scale` (units per dp, default 1) map them to the
  screen, and `scroll-top` (dp, default 0) scrolls `<event-lanes>` down. Attributes make the view visible to the MCP
  UI tools' DOM and settable from Lua; elements that show the same timeline get the same `start` and `scale`.
- **Looks.** Colors and line widths come from the style sheets, as for every widget ([themes.md](themes.md)): `color`,
  and the properties `stroke-color` and `stroke-width`, which `ui::RmlUi` registers when it starts, along with the
  elements, so every document has them. Each theme styles the four elements; a change of style or theme builds them
  again. Code only gives them data.
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
GPU) draws 10,000 events on 100 tracks under a ruler in a 1600 by 1000 window, with vsync and the default theme, as the
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
  pixel shows the peaks of the columns it covers; a curve builds only the segments around the view; the ruler's
  decimal scale and its marks.
- `tests/window_tests.cpp` draws all four through SDL_GPU at dp ratios 1 and 2 and checks their pixels: positions,
  outlines, clipping at the box, the waveform's full scale and silence, the curve's ramp, step and level, the ruler's
  ticks and labels, and scrolling.
