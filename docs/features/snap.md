# Snap and grid

Where editing in the project window puts positions (#87, reference manual pp. 105-109): snap types, grid types, snap
points, shuffle and snap to zero crossing. The engine's part is `src/engine/document/snap.h` with the `snap.*`
commands, plus `event.setSnapPoint`, `event.shuffle` and the `zeroCrossing` flag of `event.split` and `event.resize`
in `eventedits.cpp`; the event display's is in `src/ui/eventediting.cpp` and `eventdisplaymenus.cpp`.

## Settings

The project keeps its snap settings (`Project::snap`): on or off, the snap type, the time format the grid follows,
a grid type for each time format and snap to zero crossing. Like the quantize settings they are not undo steps; they
are saved in the DAWproject extension entry, and `project.get` has them as `snap`. Snap starts off, with the grid on
bars. `snap.get` reads them, `snap.set` changes what it is given and emits `snap.changed`.

The grid types depend on the time format (#413, pp. 105, 107-109): `format` is the project window's ruler format
(`barsBeats`, `seconds`, `timecode`, `samples`, `userFrames`), `grids` keeps the grid type chosen for each, by default
`bar`, `100ms`, `frame`, `100samples` and `frame`, and `grid` is the one in use, the format's. Switching the format
switches the grid to that format's, so the grid always matches the grid type menu that is shown, and switching back
finds the one chosen there before. `snap.set` with a grid type that isn't the format's refuses it when it is given a
format too; without one (as before #413) it switches to the grid type's format, the frames to `timecode` unless
`userFrames` is in use. Settings saved before #413 have only `grid`, and load with its format; older builds read
`grid`, the grid in use. A project from a newer build with a snap type, time format or grid type this build doesn't
know loads with what it knows; those are left out. Its `grids` entries with a time format or a grid type this build
doesn't know are kept, though (`SnapSettings::newerGrids`, #565): `snap.get` doesn't show them, but saving writes them
back for the newer build, until another grid type is chosen here for that format. Loading reads a format's grid
type from `grids`, and `grid` only when `grids` has none it can use (settings from before #413). Damaged values
(`on` that isn't true or false, a number for a name) are left out, not the rest of the project's extension entry. The grid follows the project
window's ruler, which the project keeps. Two things set `format`: the ruler's time format menu (`snap.set`), and a
new time format in the project setup (`project.set {timeFormat}`, #78, p. 69), which the engine makes the snap
format in the same command, and again on undo and redo of it, emitting `snap.changed`; the ruler follows. So the
project setup's time format sets the ruler, the ruler can choose its own afterwards (pp. 69, 317), and the grid types
are always the ruler's format's (pp. 105, 109); snapping is the same with and without a UI, and a `snap.set {format}`
right after it stays. The transport's time displays have formats of their own (`ui.timeFormats`), so the ruler and
the primary time display can differ.

The frame grids have the project's frame rate (project setup, `project.set {frameRate}`, #78), with its pull-down;
`userFrames` has the user's, 30 fps until that is a setting of its own (`time::g_userFramesPerSecond`), in the event
display and in `snap.position` alike.

| Snap type | A position goes to | Reference |
|---|---|---|
| `grid` | the nearest grid line | p. 105 |
| `gridRelative` | grid steps from where it was, keeping its distance to the grid: from 3.4.1 a bar on is 4.4.1; without where it was (new events), like `grid` | p. 106 |
| `events` | the start or end of another event, an audio event's snap point or a marker, when near | p. 106 |
| `shuffle` | nowhere: moved events trade places with their neighbours (`event.shuffle`) | p. 106 |
| `cursor` | the project cursor, when near | p. 106 |
| `gridCursor`, `eventsCursor`, `eventsGridCursor` | the nearest of what is near; otherwise the grid line, if the type has the grid | p. 106 |

| Grid type | Lines | Reference |
|---|---|---|
| `bar`, `beat` | bars, and beats of the signature's denominator, from the signature map, in ticks | p. 107 |
| `quantize` | the project's quantize grid (`quantize.set`), with tuplets and swing | p. 107 |
| `adaptToZoom` | a note value the view chooses, from each bar's start; none, or one as long as the bar, is bars | p. 107 |
| `1ms`, `10ms`, `100ms`, `1000ms` | seconds | p. 107 |
| `subframe` (an 80th), `quarterFrame`, `halfFrame`, `frame`, `twoFrames`, `second` | frames | pp. 107-108 |
| `1sample`, `10samples`, `100samples`, `44100samples` | samples | p. 108 |

- **One function snaps.** `engine::snap` takes the settings, the grid (`SnapGrid`: tempo and signature maps, sample
  rate, quantize settings, the zoom's note value and the frame rate) and the target (the position, where it was, the
  magnets, the cursor and how near they attract). The musical grids answer in ticks, so a part snapped to a bar starts
  exactly on it across tempo changes; the others in samples. `snap.position` runs it on the project, with every other
  event's edges and snap points and every marker as magnets; the event display runs it on what it shows while the
  mouse drags, so the preview and the result agree.
- **Markers attract from every marker track** (#415, p. 106: markers are events on the marker tracks, and other
  tracks' events attract too), the active one's or not: a marker's position, and a cycle marker's end too. Both
  `snap.position` and the event display take them from the project, the display from the same `project.get` answer
  as its events, converted to samples as the engine does, so a script or MCP client snapping a position gets what a
  drag does. Which marker track is active doesn't matter to snapping, nor whether the event display draws marker lines
  for it (`ui.markerLines`).
- **Snap points** (p. 105) are points in an audio event's clip (`AudioEvent::snapPoint`, clip frames), so they stay with
  the audio when the event is resized or split; a snap point outside the event counts as none. `event.setSnapPoint`
  sets them at a position, by default the project cursor (Snap Point to Cursor). Moving an audio event snaps its snap
  point instead of its start, and snap points attract with `events`.
- **Shuffle** (`event.shuffle`, p. 106): adjoining events on one track go where the position puts them among the
  events that adjoin them, after each one whose middle the position is past; the others close up and make room, so the
  row keeps its start and has no gaps. Locked events end the row.
- **Snap to zero crossing** (p. 105): `event.split` and normal sizing in `event.resize` with `zeroCrossing` move an
  audio event's cut to the nearest frame within 50 ms where the sum of its channels changes sign, the one of the two
  nearer to zero. A split only looks inside the event, and only at events the position cuts. The file is read on the
  command thread; a file that can't be read leaves the position as it is.

## The event display

- **Toolbar** (`resources/ui/project.rml`): the snap toggle `#snap`, the snap type menu `#snap-type-menu` behind
  `#snap-type`, the grid type menu `#grid-type-menu` behind `#grid-type`, and the toggle `#snap-zero-crossing`. The
  buttons show the settings (`.checked`, and the chosen type's name); the grid type menu has the grid types of the
  ruler's time format (bars and beats, seconds, timecode and fps (User), samples), filled in by the display from the
  engine's table (`engine::gridTypes`), the chosen one checked. Every change goes to the engine with `snap.set`; the
  display follows `snap.changed` and `project.get`. A grid type picked from the menu goes with the format the menu
  shows, so it is that format's even when the engine has switched to another one meanwhile.
- **Ruler format.** Choosing the ruler's time format sends it to the engine as the snap settings' `format`, and the
  display snaps with that format's grid at once; the ruler shows the format of the project's snap settings, so a
  project opens with the ruler format it was saved with. Until the engine has answered a `snap.set` with a format
  (from the ruler or the grid type menu), settings it sent before, in a `snap.changed` or a `project.get` answer,
  keep the display's format, so the ruler, the grid type menu and the snap grid never switch back for a moment.
- **Gestures.** Dragging events snaps the pressed event's snap point or start, and the selection keeps its distance to
  it; the preview shows the snapped place. Resizing snaps the pressed event's edge, Alt and a click splits at the
  snapped position. Events, markers and the cursor attract from 8 dp; the selection doesn't. Moves of events that are
  all on musical tracks go to the engine in ticks when a musical grid line placed them. With shuffle, a drag on one
  lane runs `event.shuffle`. Splits and resizes carry `zeroCrossing` when it is on.
- **Adapt to zoom** chooses the finest note value from a half note down to a 64th that is at least 12 dp wide at the
  tempo at the view's left edge; zoomed out further, bars.
- **Nudging** (`edit.nudge*`) moves by a grid step at the selection's start, snap on or off: a bar, a beat, the
  quantize step, the zoom's note value, or the linear grid's step.
- **Commands:** `edit.snap` switches snap on or off (J, the reference's key), `edit.snapPointToCursor` sets the snap
  point of the selected audio events that the cursor is in.

The tools snap too: selection ranges, drawn parts and the split tool ([tools.md](tools.md)).

Not yet: grid lines and snap point lines in the event display, the editors' own grids, and a user frame rate setting
(fps (User) is 30 until then).

## Checks

- `tests/snap_tests.cpp`: bar and beat lines across tempo and signature changes, the quantize grid with triplets across
  a tempo change, adapt to zoom, every linear grid and the user's frame rate, grid steps; every snap type and their
  combinations, magnets and the cursor only within range, grid relative in bars across a tempo change, in beats and on a
  linear grid; `snap.get` and `snap.set` saved with the project and not undone; `snap.position` with event edges,
  markers (also of a marker track that isn't active), snap points, the cursor, the zoom's note value, the frame rate and
  grid relative; a grid type per time format, switching formats, grid types of another format refused or choosing their
  format, and settings from before #413; each format's grid types (`gridTypes`) exactly those `hasGrid` accepts; a newer
  build's grids entries kept, written back when saved and replaced by another grid type chosen here, not by `snap.set`
  with what `snap.get` returned, and a `grid` read when the format's entry is unusable; damaged entries left out; the
  frame grids at the project's frame rate, with pull-down, and user frames at 30 fps; a new project time format
  switching the snap format, also on undo and redo, and a format set after it staying; snap points set and refused;
  shuffling forwards, backwards, to the end and to the front, its errors and a locked neighbour; splitting and resizing
  at zero crossings of a real file, not when sizing moves the contents or the file is missing; a split near an event's
  start still splits it, and one just outside an event leaves it alone.
- `tests/eventediting_tests.cpp` (fake window system, real engine): with snap on, a drag, a resize and an Alt click land
  on bars, shuffle swaps two parts, events, a position marker and the cursor attract, a position marker and a cycle
  marker's end on a linear marker track that isn't active attract a drag where `snap.position` puts it, a snap point set
  at the cursor snaps to a bar, grid relative moves a beat, nudges move a beat and a bar, and with snap off a drag
  doesn't snap; with the ruler in seconds a drag snaps to 100 ms, back in bars and beats to a bar, and the ruler follows
  the format of `snap.set`; a new ruler format kept at every step while a `snap.changed` or a `project.get` answer from
  before it comes in, and after several switches in a row.
- `tests/uiapp_tests.cpp`: the toolbar's snap and zero crossing buttons and J, the snap type menu checked and shown on
  its button, the grid type menu with the grid types of each ruler format, and back in a format the grid type chosen
  there shown and checked; the menu's items the engine's `gridTypes` for every format, and a grid type picked while
  the engine switches to another format going to the format shown; the project window's golden image shows the
  toolbar.
- `tests/dawproject_tests.cpp`: an extension entry with the snap settings as #87 saved them loads with the grid
  type's format; a newer build's grid types saved again; damaged snap settings loading with the rest of the entry.
