# Snap and grid

Where editing in the project window puts positions (#87, reference manual pp. 105-109): snap types, grid types, snap
points, shuffle and snap to zero crossing. The engine's part is `src/engine/document/snap.h` with the `snap.*`
commands, plus `event.setSnapPoint`, `event.shuffle` and the `zeroCrossing` flag of `event.split` and `event.resize`
in `eventedits.cpp`; the event display's is in `src/ui/eventediting.cpp` and `eventdisplaymenus.cpp`.

## Settings

The project keeps its snap settings (`Project::snap`): on or off, the snap type, the grid type and snap to zero
crossing. Like the quantize settings they are not undo steps; they are saved in the DAWproject extension entry, and
`project.get` has them as `snap`. Snap starts off, with the grid on bars. `snap.get` reads them, `snap.set` changes
what it is given and emits `snap.changed`.

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
- **Snap points** (p. 105) are points in an audio event's clip (`AudioEvent::snapPoint`, clip frames), so they stay with
  the audio when the event is resized or split; a snap point outside the event counts as none. `event.setSnapPoint`
  sets them at a position, by default the project cursor (Snap Point to Cursor). Moving an audio event snaps its snap
  point instead of its start, and snap points attract with `events`.
- **Shuffle** (`event.shuffle`, p. 106): adjoining events on one track go where the position puts them among the
  events that adjoin them, after each one whose middle the position is past; the others close up and make room, so the
  row keeps its start and has no gaps. Locked events end the row.
- **Snap to zero crossing** (p. 105): `event.split` and normal sizing in `event.resize` with `zeroCrossing` move an
  audio event's cut to the nearest frame within 50 ms where the sum of its channels changes sign, the one of the two
  nearer to zero. The file is read on the command thread; a file that can't be read leaves the position as it is.

## The event display

- **Toolbar** (`resources/ui/project.rml`): the snap toggle `#snap`, the snap type menu `#snap-type-menu` behind
  `#snap-type`, the grid type menu `#grid-type-menu` behind `#grid-type`, and the toggle `#snap-zero-crossing`. The
  buttons show the settings (`.checked`, and the chosen type's name); the grid type menu has the grid types of the
  ruler's time format (bars and beats, seconds, timecode and fps (User), samples), filled in by the display, the chosen
  one checked. Every change goes to the engine with `snap.set`; the display follows `snap.changed` and `project.get`.
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

Not yet: grid lines and snap point lines in the event display, snapping ranges and drawing (they come with the tools,
#86), the editors' own grids, and a frame rate of the project's (#78; timecode grids use 25 fps until then).

## Checks

- `tests/snap_tests.cpp`: bar and beat lines across tempo and signature changes, the quantize grid with triplets
  across a tempo change, adapt to zoom, every linear grid and the user's frame rate, grid steps; every snap type and
  their combinations, magnets and the cursor only within range, grid relative in bars across a tempo change, in beats
  and on a linear grid; `snap.get` and `snap.set` saved with the project and not undone; `snap.position` with event
  edges, markers, snap points, the cursor, the zoom's note value, the frame rate and grid relative; snap points set and
  refused; shuffling forwards, backwards, to the end and to the front, its errors and a locked neighbour; splitting
  and resizing at zero crossings of a real file, not when sizing moves the contents or the file is missing.
- `tests/eventediting_tests.cpp` (fake window system, real engine): with snap on, a drag, a resize and an Alt click land
  on bars, shuffle swaps two parts, events and the cursor attract, a snap point set at the cursor snaps to a bar, grid
  relative moves a beat, nudges move a beat and a bar, and with snap off a drag doesn't snap.
- `tests/uiapp_tests.cpp`: the toolbar's snap and zero crossing buttons and J, the snap type menu checked and shown on
  its button, the grid type menu with the grid types of each ruler format; the project window's golden image shows the
  toolbar.
