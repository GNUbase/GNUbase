# Event display

The project window's timeline (#84, reference manual pp. 67–73, 101–104, 110, 316–317): the events of every track
under a ruler, with an overview line above, the project cursor and a cross-hair cursor, zoom and scrolling. The code
is `ui::EventDisplay` (`src/ui/eventdisplay.h`), with the time formats in `src/ui/timeformat.h`; it draws with the
canvas elements ([canvas.md](canvas.md)).

## Structure

`EventDisplay` fills an element of a document and follows the project through the engine binding
([ui-engine-binding.md](ui-engine-binding.md)): it is one more client of the engine API, and shows what any client
changed. The app shows it in the main window (`resources/ui/project.rml`, `AppUi::showProject`) until the docking
system (#56) and the track list (#81) give the project window its zones.

```
.event-display                  the element the document gives it
  .overview                     the overview line
    event-lanes                 every event of the project in one row
    .overview-view              the part the view shows
  .timeline
    ruler.timeline-ruler        the ruler, in the chosen time format
    .event-area
      event-lanes.events        a lane per track
      .cross-hair-vertical, .cross-hair-horizontal
    .project-cursor             across the ruler and the lanes
```

`resources/styles/base.rcss` lays it out; the themes color it ([themes.md](themes.md)). Code sets only what comes from
data: the canvas elements' attributes `start` (seconds at the left edge), `scale` (seconds per dp) and `scroll-top`,
where the cursors are, and the overview's rectangle. The cross-hair cursor shows by style sheet while the mouse is over
the lanes and the display has the class `cross-hair` (`setCrossHair`); tools that hide it come with #86. The
attributes show the view to MCP's `ui_dom`.

- **Data.** On `history.changed` (every change to the project and every undo) and `project.reset` it asks for
  `project.get` again, one request at a time, and shows each track's audio events and MIDI parts in seconds: musical
  positions through the tempo map, linear ones by the sample rate, audio lengths by their media file's rate. The
  project cursor follows `transport.changed`, and while playing `Engine::playbackPosition` once a frame. Reading the
  whole project after each change is simple; reading only what the events name can come when projects get large.
- **Time axis.** Seconds from the project start, so that the view keeps its time when the sample rate changes. Nothing
  scrolls before the project start.
- **Live reload** loads `project.rml` again without the display in it; reopen the app.

## Ruler and time formats

The ruler shows bars and beats (the default), seconds, timecode at the project's frame rate, samples, or timecode at a
frame rate of the user's (`setRulerFormat`; its `format` attribute names it: `barsBeats`, `seconds`, `timecode`,
`samples`, `userFrames`). `ui::rulerScale` puts labels at least their width and an em apart:

- **Bars and beats** where the tempo and signature maps put them: every few bars (powers of two), every beat, every
  sixteenth, or every 60, 30, 15, 5 or 1 display ticks (120 per sixteenth), the finest step that keeps labels apart
  where the tempo is fastest and the bars or beats shortest. Labels leave out what is zero: `5`, `5.2`, `5.2.3`,
  `5.2.3.60`. Minor ticks mark beats, sixteenths or parts of the step.
- **Seconds** in steps from 1 ms to an hour (`0:00:05`, `0:00:01.020`), **timecode** in steps of frames, then of
  seconds and minutes (`00:01:00:00`, `;` before the frames at drop-frame rates), **samples** in decimal steps.
- `ui::formatTime` writes a position the way position displays do (`1.1.1.0`, `0:00:01.500`, `00:00:01:12`, `72000`),
  exactly as the engine converts it (`engine/time`).

The ruler's time formats follow the reference's (p. 69). Its mode "bars and beats linear" (p. 70), which keeps bars
evenly spaced across tempo changes, comes with the tempo track (#155); the project's frame rate, start time and length
come with the project setup (#78): until then timecode runs at 25 fps.

## Zoom and scrolling

| Action | Command (`actions()`) | Mouse |
|---|---|---|
| Zoom in, zoom out by 2, around the project cursor, or onto it when it's out of view | `zoom.in`, `zoom.out` | Ctrl (Cmd) and the wheel, around the mouse, 1.25 per line; drag down or up in the ruler's lower half, around where the drag started, 2 per 40 dp |
| Track heights by 1.25 | `zoom.inVertically`, `zoom.outVertically` | Ctrl and Shift and the wheel |
| The whole project: from its start to the end of its last event, at least a minute | `zoom.full` | |
| Undo and redo zoom | `zoom.undo`, `zoom.redo` | |
| Auto-scroll on or off | `view.autoScroll` | |
| Scroll | `scrollTo`, `scrollVertically` | the wheel down, Shift and the wheel across (a tenth of the view per line) |

- Every zoom, and every drag that zooms, is one step in the zoom history; scrolling isn't. `zoomTo(from, to)` shows a
  stretch, such as a cycle marker's. Zoom stops at 10 µs and 60 s per dp, track heights at a quarter and eight times
  `g_trackHeight` (40 dp, until tracks have heights of their own, #81).
- **Commands.** `actions()` maps command ids to the display's functions, for the key command system (#57) to bind
  keys to (the reference's H, G, Shift+H and Shift+G). Until it does, MCP clients and tests zoom with the mouse
  through the UI tools (`ui_wheel` with `modifiers`, `ui_drag`).
- **Zoom presets** are the user setting `ui.zoomPresets`, for every project: each a name and the seconds the view
  shows. `addZoomPreset`, `applyZoomPreset` (keeps the left edge), `removeZoomPreset` and `renameZoomPreset` change it
  through `settings.set`; changes from MCP or Lua apply at once.
- The presets menu, the ruler's time format menu and the auto-scroll menu come with the widget library's menus (#54).
  Zoom to selection waits for selections (#85); track height presets for the track list (#81).

## Overview line, cursors and auto-scroll

- **Overview line.** Every event of the project in one row, from the start to the project's length (and further when
  the view is), with a rectangle for the view. Dragging the rectangle scrolls, dragging its edges zooms (one step in
  the history), a click outside it centres the view there.
- **Project cursor.** A line across the ruler and the lanes at the transport's position. A click in the lower half of
  the ruler moves it there (`transport.locate`).
- **Cross-hair cursor.** Two lines through the mouse while it is over the lanes.
- **Auto-scroll** (`setAutoScroll`, `setAutoScrollMode`) keeps the cursor in view while playing: *page* turns the
  view a page when the cursor leaves it, *stationary* keeps the cursor in the middle. With
  `setSuspendAutoScrollWhenEditing`, a click in the lanes while playing suspends it until playback stops or
  auto-scroll is switched on again.

## Checks

- `tests/timeformat_tests.cpp`: positions in every format read as the engine converts them, also at drop-frame and
  user frame rates; bars and beats on the ruler where tempo and signature changes put them, at every level of zoom;
  seconds, timecode and samples in round steps; labels never closer than asked, across a tempo ramp.
- `tests/eventdisplay_tests.cpp` (fake window system, real engine): events and the cursor where the engine has them,
  following changes and undo from another client; zoom around the cursor, its history, zoom full and zoom to a
  stretch; vertical zoom; scrolling and zooming with the wheel and modifiers; the ruler's click and drag; the overview
  line's drag, click and edge; auto-scroll in both modes, suspended and off, while the dummy device plays; zoom
  presets through the user settings; the cross-hair cursor; the time format; the command ids.
- `tests/uiapp_tests.cpp` shows the project window in the app's own window through SDL_GPU: the cursor's box 150 dp
  from the lanes as MCP's `ui_dom` reports it, a golden image (`tests/golden/ui/event-display.png`), and zooming with
  Ctrl and the wheel through the UI tools.
