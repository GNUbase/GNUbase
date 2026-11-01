# Event display

The project window's timeline (#84, reference manual pp. 67–73, 101–104, 110, 316–317): the events of every track
under a ruler, with an overview line above, the project cursor and a cross-hair cursor, zoom and scrolling, and the
menus for the ruler's time format, zoom presets and auto-scroll. The code is `ui::EventDisplay`
(`src/ui/eventdisplay.h`, its menus in `src/ui/eventdisplaymenus.cpp`), with the time formats in
`src/ui/timeformat.h`; it draws with the canvas elements ([canvas.md](canvas.md)) and uses the widget library's
buttons, menus and dialogs ([widgets.md](widgets.md)).

## Structure

`EventDisplay` fills an element of a document and follows the project through the engine binding
([ui-engine-binding.md](ui-engine-binding.md)): it is one more client of the engine API, and shows what any client
changed. The app shows it in the main zone of the project window (`resources/ui/project.rml`, `AppUi::showProject`,
[docking.md](docking.md)), under the toolbar with the auto-scroll and zoom presets buttons, right of the track list
([track-list.md](track-list.md)), which shows a row beside each lane. Hiding the overview line, a strip of the window, hides the display's `.overview`.

```
.event-display                  the element the document gives it
  .overview                     the overview line
    event-lanes                 every event of the project in one row
    .overview-view              the part the view shows
  .timeline
    ruler.timeline-ruler        the ruler, in the chosen time format
    .event-area
      event-lanes.events        a lane per track
      event-lanes.muted-events  muted events again, over them (parts-and-events.md)
      event-lanes.selected-events  selected events again, over those
      .waveforms                a waveform per audio event near the view (peaks.md)
      .cross-hair-vertical, .cross-hair-horizontal
    .project-cursor             across the ruler and the lanes
```

Selecting and editing events, with the mouse and with the edit commands, is in [parts-and-events.md](parts-and-events.md);
snapping them, with the toolbar's snap controls, in [snap.md](snap.md).

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

**Time format menu** (`#ruler-format-menu`): the arrow button at the ruler's right end (`.ruler-format`, in the
document) and a right click anywhere in the ruler open it, with Bars+Beats, Seconds, Timecode, Samples and fps (User);
the ruler's format is checked. The ruler's time formats follow the reference's (p. 69). Its mode "bars and beats linear" (p. 70), which keeps bars
evenly spaced across tempo changes, comes with the tempo track (#155); the project's frame rate, start time and length
come with the project setup (#78): until then timecode runs at 25 fps.

## Zoom and scrolling

| Action | Command (`actions()`) | Keys | Mouse |
|---|---|---|---|
| Zoom in, zoom out by 2, around the project cursor, or onto it when it's out of view | `zoom.in`, `zoom.out` | H, G | Ctrl (Cmd) and the wheel, around the mouse, 1.25 per line; drag down or up in the ruler's lower half, around where the drag started, 2 per 40 dp |
| Track heights by 1.25 | `zoom.inVertically`, `zoom.outVertically` | Shift+H, Shift+G | Ctrl and Shift and the wheel |
| The whole project: from its start to the end of its last event, at least a minute | `zoom.full` | Shift+F | |
| Undo and redo zoom | `zoom.undo`, `zoom.redo` | | |
| Auto-scroll on or off | `view.autoScroll` | F | |
| Scroll | `scrollTo`, `scrollVertically` | | the wheel down, Shift and the wheel across (a tenth of the view per line) |

- Every zoom, and every drag that zooms, is one step in the zoom history; scrolling isn't. `zoomTo(from, to)` shows a
  stretch, such as a cycle marker's. Zoom stops at 10 µs and 60 s per dp, track heights at a quarter and eight times
  each track's height (40 dp, `g_trackHeight`, until the track list sets another, [track-list.md](track-list.md)).
  The tracks inside folded folders have no lane.
- **Commands.** `actions()` maps command ids to the display's functions. `AppUi` adds them as key commands
  ([key-commands.md](key-commands.md)) and binds the reference's keys (manual p. 1573) in the project window's focus
  zone, `project`, which the window has when it opens and wherever it is clicked. `KeyCommands::run` runs one as its
  key would; MCP clients press the keys with `ui_key`.
- **Zoom presets** are the user setting `ui.zoomPresets`, for every project: each a name and the seconds the view
  shows. `addZoomPreset`, `applyZoomPreset` (keeps the left edge), `removeZoomPreset` and `renameZoomPreset` change it
  through `settings.set`; changes from MCP or Lua apply at once.
- **Zoom presets menu** (`#zoom-presets-menu`, the toolbar's `#zoom-presets` button; reference pp. 103–104): the
  presets, which apply as above; Zoom Full; the cycle markers of the active marker track (`marker.list`, asked for
  again on every `history.changed`), each zooming to its range; Add..., which names the view's zoom in a dialog
  (`resources/ui/zoom-preset.rml`) and adds it; and Organize..., a dialog (`zoom-presets.rml`) with a row per preset,
  whose name can be edited and which a Delete button marks deleted. OK saves the presets as the dialog shows them, in
  one `settings.set`; Cancel and Escape drop the changes. The dialogs load from the folder of the display's document.
- Zoom to selection waits for selections (#85); the track height presets menu isn't there yet.

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
- **Auto-scroll button and menu.** The toolbar's toggle button `#auto-scroll` switches it, and shows it: `.checked`
  while it's on, `.suspended` while editing suspends it (the themes draw it in the warning color, as the reference
  changes the button's color, p. 317). A click on a suspended button resumes auto-scroll rather than switching it
  off. The arrow beside it opens `#auto-scroll-menu`: Page Scroll and Stationary Cursor, the mode checked, and Suspend
  Auto-Scroll when Editing, checked while it's on.

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
  Ctrl and the wheel through the UI tools. It drives the menus with clicks, keys and text as MCP's UI tools do: the
  time format menu from its arrow and a right click in the ruler, every format, the checked one; zoom presets added
  through the dialog (and cancelled with Escape), listed and applied, Zoom Full, a cycle marker from the engine
  listed and zoomed to, presets renamed and deleted in the organize dialog with OK and dropped with Cancel; the
  auto-scroll button on and off, the modes and suspending from its menu, checked, and the button suspended by a click
  in the lanes while the dummy device plays, and resumed by a click on it.
  The keys zoom, vertically too, zoom full and switch auto-scroll when the window opens and after a click into
  it, and zoom undo runs as a command.
