# In-place editor

The in-place editor (#131, reference manual pp. 1259-1261): a MIDI or instrument track's lane in the project window
opens into a small key editor ([key-editor.md](key-editor.md)), to edit the track's notes and controllers in context
with the other tracks.

## In the engine

An in-place editor is one more editor view of `src/engine/document/editors.cpp` ([midi-editors.md](midi-editors.md)),
named `inPlace:<track>`, one per MIDI or instrument track. `editor.get`, `editor.set`, `editor.input` and
`editor.close` take that name like `lower` and `window`, so its selection, step and MIDI input and controller lanes
work as in the key editor. It is view state like the other views: no undo steps, not saved, and a new or loaded
project closes it.

| Command | What it does | Reference |
|---|---|---|
| `editor.inPlace` | Opens (`open` true), closes (false) or toggles (without `open`) the in-place editors of `tracks`; returns `tracks`, those whose in-place editors are open, in the track list's order. Without `tracks` it only lists them | pp. 1259, 1261 |
| `note.move` | Moves a MIDI `part`'s named `notes` to a `target` part, by `ticks` and by `semitones`, as the drag between in-place editors sends it: each note keeps its id and every other field, the move is one undo step and the shared copies of both parts follow it. A note may reach past the target part's end, as `note.add` and `note.set` allow. It refuses a part or a note that doesn't exist, a note that would land before the target part's start or outside the pitches 0-127, and a move between shared copies of one group, which must stay equal; nothing changes then. Emits `note.removed` for the source part and `note.added` for the target, and returns the moved notes | p. 1261 |

- Reading `editor.inPlace` without `tracks` leaves `engine.commandLog` unchanged; calls with `tracks` remain logged.
- **Parts.** An open in-place editor shows every MIDI part of its track, in order, and parts added later; its parts
  can't be set (`parts`, `showTracks`, `hideTracks` and `exclusive` are refused), and it always shows the key editor.
  Showing one track, its `editingMode` is `allParts` or `activePart`; `activeTrack` is refused.
  The first part is active; `activePart` activates another one. A removed track's editor closes, and comes back with
  the undo.
- `editor.open` refuses in-place editors; other tracks than MIDI and instrument tracks have none.
- The `editor` parameter's schema has the names as the pattern `^(lower|window|inPlace:[0-9]+)$`, so MCP clients and
  Lua see them, and a `maxLength` of 28 (`inPlace:` and at most 20 digits for the track: track ids come from the
  project file and may take all 64 bits, #1159). A longer name gets the length error before the pattern is matched, so
  that no name can make the matching overflow the stack (#1059); a number beyond 64 bits isn't a track.

## In the UI

`ui::InPlaceEditors` (`src/ui/inplaceeditor.h`) follows `editor.changed` of the in-place views, whichever client
changes them, and shows each open one as a `ui::KeyEditor` over its track's lane in the event display
([event-display.md](event-display.md)). `EventDisplay::setInPlaceTracks` makes those tracks' own lanes at least
240 dp high (`g_inPlaceHeight`, times vertical zoom), so the track list's row grows with it; the track's lanes (#99)
keep its own height.

```
.event-area
  .in-place-editors                   over the event lanes, no height of its own
    .in-place-editor[track]           over its track's own lane; .hidden while a folded folder hides the track,
                                      .toolbar-shown while the toolbar shows; the focus zone "inPlace"
      .key-editor                     the key editor (key-editor.md): the parts' row, keyboard, grid, controller lanes
        .key-editor-toolbar           starts with button.in-place-editing-mode and button.in-place-active-part
      button.in-place-toolbar-toggle  shows or hides the toolbar and the info line (p. 1259)
menu#in-place-editing-mode-menu-inPlace-<track>, menu#in-place-active-part-menu-inPlace-<track>   in the body
```

- **Timeline.** The key editor's grid lies on the project window's timeline: once a frame (`InPlaceEditors::update`)
  the grid's left and right edges get the times the ruler has there (`KeyEditor::setTimeRange`), so the notes and the
  parts' boxes are under their times while the project window zooms and scrolls. The keyboard at the left covers the
  start of the view. Between the edges the axis is linear in ticks: across a tempo change in the view the notes drift
  from the ruler until the key editor gets a tempo-mapped axis. The tempo map is the display's (`EventDisplay::time`),
  read once per project it reads, and a new range only moves the notes, a note being drawn, the parts' boxes and the
  controller lanes' bars (`KeyEditor::placeItems`, `ControllerLanes::setRange`), so scrolling, zooming and following
  the project cursor read and build nothing again.
- **Editing** is the key editor's: its tools, the toolbox on a right click, the transpose palette, step and MIDI input,
  the info line and the controller lanes, sent as `note.*` and `editor.set` for the view `inPlace:<track>`. Presses,
  clicks and double clicks in an in-place editor stay there: the event display doesn't select, move or add parts under
  it. A drag on its keyboard's left part zooms and scrolls the pitches, and a right click below a lane's name adds and
  removes controller lanes (p. 1261, [key-editor.md](key-editor.md), [controller-lanes.md](controller-lanes.md)).
  Dragging the selected notes onto another in-place editor's grid sends one `note.move` per source part, which moves
  them to that track's part at the drop, keeping their ids, their distances to the pressed one and every other field
  they have, as one undo step; nothing moves when the drop lies where the target track has no part, or a note would land
  before that part's start or outside the pitches (p. 1261). A click on a part in the row of parts activates it (p.
  1152). The editors' menus have ids of their own, ending in `-inPlace-<track>` (`ui::menuIdSuffix`), since they share
  the project window's document with each other and with the lower zone's editor.
- **Multiple part controls** (p. 1260), first in the toolbar: Part Editing Mode, a menu of All Parts and Active Part
  (`editingMode` allParts or activePart), and Activate Part for Editing, a menu of the track's parts with the active
  one's name on the button (`activePart`).
- **Quantizing and MIDI functions.** Each editor is the focus zone `inPlace`, with its track in `track`: a key command
  such as Quantize (Q, [quantize.md](quantize.md)) or MIDI > Functions ([midi-functions.md](midi-functions.md)) with
  the focus in it takes its selected notes, or all of its notes, never the project window's selected parts under it.
  The Edit and MIDI menus run in the project window's zone and take its selection.
- **Opening and closing.** MIDI > Open In-Place Editor (`editor.openInPlace`) opens it for the selected MIDI and
  instrument tracks, Edit In-Place (`editor.editInPlace`, Mod+Shift+I, p. 1261) opens or closes each one, both disabled
  until such a track is selected, and the inspector's Edit In-Place button (`button[control=editInPlace]`,
  [inspector.md](inspector.md)) the selected track's, checked while it is open. A double click on the controller lanes'
  footer strip, the place below the controller display (p. 1261, [controller-lanes.md](controller-lanes.md)), closes
  that track's editor too; one on the strip's Create and Setup buttons doesn't.

`resources/styles/keyeditor.rcss` lays the editors out; the themes give them an opaque background over the events
(`tools/skin/generate.py`).

## Not yet

Tracked in #1158: the setup panes of the toolbar and the info line (p. 1575), which only the project window has so far
([docking.md](docking.md)); the project window's info line showing the selected note (the editor's own info line shows
it with the toolbar); a tempo-mapped time axis; the keyboard in the track list's column; Edit In-Place in the track
list's controls, with the toolbar's triangle at the upper right of the track's row in the track list (p. 1259) instead
of over the editor; the toolbar's Acoustic Feedback, Indicate Transpositions and Event Colors (the key editor has none
yet).

## Checks

- `tests/inplaceeditor_tests.cpp`: `editor.inPlace` opening, toggling and closing MIDI and instrument tracks' editors
  with `editor.changed`, the list of open ones in the track list's order, every part of the track shown and parts
  added later, the selection and settings through `editor.set`, `editor.close`; audio tracks, unknown tracks and
  names, `editor.open`, setting parts, showing tracks, other contents, global tracks and the editing mode activeTrack
  refused, and the names' pattern in the schema; a track without parts; a removed track closing it and its undo
  bringing it back, and a new project closing them all. `tests/engine_tests.cpp` checks schema patterns and their text in the API references.
- `tests/inplaceeditor_ui_tests.cpp` drives the app with `ui::Automation`: MIDI > Open In-Place Editor for a MIDI and an
  audio track selected opens the MIDI track's editor over its grown lane, notes and the part's box at their times on the
  timeline, also after zooming and scrolling and with the toolbar shown; Edit In-Place, an engine client and a new
  project close and open it; a click selecting a note and a double click inserting one in the editor without the event
  display selecting or adding a part, the draw tool from the toolbar, a part added later activated with a click; the
  inspector's Edit In-Place button opening and closing it, unavailable on an audio track. Two in-place editors, a MIDI
  and an instrument track's, beside the lower zone's key editor, each with menus and a selection of its own, the
  multiple part controls setting their editor's editing mode and active part, and closing one leaving the other's menus;
  the track's lanes keeping their height, a folded folder hiding the editor; a time-linear track's notes on the
  timeline; a key of the keyboard held while the editor closes ending its pitch (key-editor.md); Q in the editor
  quantizing its selected note and not the selected part; a drag on the keyboard's left part zooming the pitches and
  scrolling them; a double click on the lanes' footer strip closing it, and one on the strip's buttons not; a note
  dragged into another in-place editor moving to that track's part with its id, as one undo step, with the other
  selected note, back with an undo into its selection, and dropped where the other track has no part or where a note
  wouldn't fit staying where it is; the menu commands waiting for a MIDI or instrument track; a note being drawn moving
  with the notes while the project window zooms.
- `tests/midiedit_tests.cpp`: `note.move` moving notes to another part as one undo step, keeping every field, a quantize
  origin included, and their ids, two notes keeping their distance, with its `note.removed` and `note.added` events,
  undo and redo; refusing a part or a note that doesn't exist, a note that would land before the target part's start or
  outside the pitches and a move between shared copies of one group, changing nothing then; a note reaching past the
  target part's end, a move within the source part itself, and the shared copies of the source and of the target staying
  consistent.
