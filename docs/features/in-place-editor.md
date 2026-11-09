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

- **Parts.** An open in-place editor shows every MIDI part of its track, in order, and parts added later; its parts
  can't be set (`parts`, `showTracks`, `hideTracks` and `exclusive` are refused), and it always shows the key editor.
  The first part is active; `activePart` activates another one. A removed track's editor closes, and comes back with
  the undo.
- `editor.open` refuses in-place editors; other tracks than MIDI and instrument tracks have none.

## In the UI

`ui::InPlaceEditors` (`src/ui/inplaceeditor.h`) follows `editor.changed` of the in-place views, whichever client
changes them, and shows each open one as a `ui::KeyEditor` over its track's lane in the event display
([event-display.md](event-display.md)). `EventDisplay::setInPlaceTracks` makes those lanes at least 240 dp high
(`g_inPlaceHeight`, times vertical zoom), so the track list's row grows with it.

```
.event-area
  .in-place-editors                   over the event lanes, no height of its own
    .in-place-editor[track]           over its track's own lane; .hidden while a folded folder hides the track,
                                      .toolbar-shown while the toolbar shows
      .key-editor                     the key editor (key-editor.md): the parts' row, keyboard, grid, controller lanes
      button.in-place-toolbar-toggle  shows or hides the toolbar and the info line (p. 1259)
```

- **Timeline.** The key editor's grid lies on the project window's timeline: once a frame (`InPlaceEditors::update`)
  the grid's left and right edges get the times the ruler has there (`KeyEditor::setTimeRange`), so the notes and the
  parts' boxes are under their times while the project window zooms and scrolls. The keyboard at the left covers the
  start of the view. Between the edges the axis is linear in ticks: across a tempo change in the view the notes drift
  from the ruler until the key editor gets a tempo-mapped axis.
- **Editing** is the key editor's: its tools, the toolbox on a right click, the transpose palette, step and MIDI input,
  the info line and the controller lanes, sent as `note.*` and `editor.set` for the view `inPlace:<track>`. Presses,
  clicks and double clicks in an in-place editor stay there: the event display doesn't select, move or add parts
  under it. A click on a part in the row of parts activates it (p. 1152). The editors' menus have ids of their own,
  ending in `-inPlace-<track>` (`ui::menuIdSuffix`), since they share the project window's document with each other
  and with the lower zone's editor.
- **Opening and closing.** MIDI > Open In-Place Editor (`editor.openInPlace`) opens it for the selected MIDI and
  instrument tracks, Edit In-Place (`editor.editInPlace`, Mod+Shift+I, p. 1261) opens or closes each one, and the
  inspector's Edit In-Place button (`button[control=editInPlace]`, [inspector.md](inspector.md)) the selected
  track's, checked while it is open.

`resources/styles/keyeditor.rcss` lays the editors out; the themes give them an opaque background over the events
(`tools/skin/generate.py`).

## Not yet

The project window's info line showing the selected note (the editor's own info line shows it with the toolbar);
zooming and scrolling the pitches by dragging the keyboard (the editor scrolls with its scroll bar); a
tempo-mapped time axis; adding and removing controller lanes with a right click below the lane's name; closing with a
double click below the controller display; dragging notes from one in-place editor to another; the keyboard in the
track list's column; Edit In-Place in the track list's controls; the toolbar's Acoustic Feedback, Indicate
Transpositions and Event Colors (the key editor has none yet); MIDI functions and quantizing on its selected notes.

## Checks

- `tests/inplaceeditor_tests.cpp`: `editor.inPlace` opening, toggling and closing MIDI and instrument tracks' editors
  with `editor.changed`, the list of open ones in the track list's order, every part of the track shown and parts
  added later, the selection and settings through `editor.set`, `editor.close`; audio tracks, unknown tracks and
  names, `editor.open`, setting parts, showing tracks, other contents and global tracks refused; a track without parts;
  a removed track closing it and its undo bringing it back, and a new project closing them all.
- `tests/inplaceeditor_ui_tests.cpp` drives the app with `ui::Automation`: MIDI > Open In-Place Editor for a MIDI
  and an audio track selected opens the MIDI track's editor over its grown lane, notes and the part's box at their
  times on the timeline, also after zooming and scrolling and with the toolbar shown; Edit In-Place, an engine client
  and a new project close and open it; a click selecting a note and a double click inserting one in the editor
  without the event display selecting or adding a part, the draw tool from the toolbar, a part added later activated
  with a click; the inspector's Edit In-Place button opening and closing it, unavailable on an audio track.
