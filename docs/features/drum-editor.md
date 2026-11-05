# Drum editor and drum maps

The drum editor (#129, reference manual pp. 1221-1242): the MIDI editors' content ([midi-editors.md](midi-editors.md))
for drum parts, with the drum sound list of the track's drum map, notes as diamonds, the drumstick and the controller
lanes ([controller-lanes.md](controller-lanes.md)); and drum maps, which change how the notes of the tracks that use
them play and come in.

## In the engine

**Drum maps** (`src/engine/midi/drummaps.h`) are part of the project (`Project::drumMaps`): `project.get` lists them as
`drumMaps`, they are saved in the project's GNUbase entry, and changing them is an undo step. A map has a name, a
sound for each of the 128 pitches and the drum sound list's `order`, from the top. A new project has the GM Map
(p. 1239): General MIDI's percussion names, every I-note and O-note its own pitch, channel 10 and a 1/16 snap; projects
saved before #129 get it when they load.

| Sound field | What it is | Reference |
|---|---|---|
| `name` | The drum sound's name | p. 1233 |
| `snap` | A note value: the length of new notes with Drum-Map Link | pp. 1233, 1240 |
| `mute` | The sound doesn't play | p. 1234 |
| `iNote` | The note played in that becomes this pitch | p. 1239 |
| `oNote` | The note that plays | pp. 1239-1240 |
| `channel` | 0-15, the MIDI channel it plays on, or `null` for Any: the note's own | p. 1238 |
| `output` | The MIDI output it plays on, empty for the track's (Default) | p. 1238 |

| Command | What it does | Reference |
|---|---|---|
| `drumMap.add` | New Map (Empty Map, the sounds Sound 1 to Sound 128) or New Copy of a map (`copy`), with the settings given | p. 1236 |
| `drumMap.set` | Changes a map's name, sounds (by pitch) and order; the change goes to every track using it | p. 1237 |
| `drumMap.remove` | Removes a map; the tracks that used it have none | p. 1236 |
| `drumMap.export` | Saves a map as a drum map file (`.drm`) | p. 1236 |
| `drumMap.import` | Loads a drum map file into the project as a new map | p. 1236 |
| `track.set drumMap` | The map a MIDI or instrument track's notes play through, 0 for none | p. 1239 |

They emit `drumMap.added`, `drumMap.changed` and `drumMap.removed` with the map (its id for removed).

- **Playback** (`src/engine/document/rendersnapshot.cpp`): the render snapshot plays each note of a track with a map
  as its sound: the O-note on the sound's channel, or the note's own with Any; a muted sound's notes don't play. The
  notes keep their pitch, so switching maps switches the instrument's sounds.
- **Input**: notes played into a track with a map take the pitch of the first sound, by pitch, whose I-note they are:
  `editor.input`'s step and MIDI input and `recording.add`.
- **Drum map files** are XML: `<DrumMap>` with the name, the snap values (`Quantize`), the sounds by pitch (`Map`),
  the `Order` and the outputs the sounds' `PortIndex` names, the first being the track's. Settings a file doesn't have
  are Empty Map's; an order that isn't each pitch once is the pitch order.
- **The editor views** ([midi-editors.md](midi-editors.md)) have the content `drum`, and `lengthQuantize` also takes
  `drumMap` (Drum-Map Link): new notes are as long as their sound's snap, or without a map the quantize grid. A closed
  editor opens the parts of a track with a drum map in the drum editor (Use Drum Editor when Drum Map is assigned,
  p. 1221).

## In the UI

The drum editor is `ui::KeyEditor` (`src/ui/keyeditor.h`) made with `drum`: `ui::MidiEditor` makes it while the view's
content is `drum`. It is the key editor ([key-editor.md](key-editor.md)) with these differences:

```
.key-editor.drum-editor
  .toolbar.key-editor-toolbar
    button.key-tool[key-tool]       Object Selection, Drumstick (key-tool draw), Line, Erase
    #key-length-quantize            Insert Length, menu #key-length-menu with Drum-Map Link (length drumMap)
    #drum-map                       the active track's map's name, or No Drum Map; menu #drum-map-menu
  .key-editor-body
    .key-editor-keys                the drum sound list: a .key-editor-key.drum-sound[pitch] per sound in the map's
                                    order (.selected for selected notes) with .drum-sound-pitch; with a map also
                                    button.drum-sound-mute (.checked while muted), textdisplay.drum-sound-name and
                                    textdisplay.drum-sound-field[sound-field=snap|iNote|oNote|channel]
    .key-editor-grid                a .key-editor-row per sound; the notes .key-note.drum-note, diamonds at their
                                    start on their sound's row
#drum-map-menu                      menuitem[drum-map] for No Drum Map (0) and each map, [checked] for the track's;
                                    menuitem[drum-map-action=new|copy|remove]
```

- **Drum sound list** (pp. 1233-1234). The rows follow the map's order; without a map they go up from pitch 0 and name
  the pitches only. A double click edits a field, which sends `drumMap.set`: the name, the snap as a note value, the
  I-note and O-note as a name such as C1 or a number, the channel as 1-16 or Any. The mute button mutes the sound.
- **Drumstick** (pp. 1240-1242). A click inserts a note at the snapped position, as long as Insert Length says; a drag
  to the right inserts one at every step of the quantize grid; a click on a note deletes it. The object selection
  tool's double click inserts and deletes as in the key editor. A move goes from row to row in the list's order.
- **Map menu** (pp. 1234, 1236, 1239). Choosing a map, or No Drum Map, sends `track.set` for the active track. New Map
  and New Copy add a map (`drumMap.add`) that becomes the track's; Remove removes the track's map.
- **Commands** (Edit menu, `app::AppUi`): `editor.openDrum` (Open Drum Editor, p. 1221) opens the selected MIDI parts
  in the editor window's drum editor; `drumMap.load` (Load Drum Map...) and `drumMap.save` (Save Drum Map...) load a
  `.drm` file into the project and save the drum map of the editor's active track, through the OS's file dialogs.

`resources/styles/keyeditor.rcss` lays the list and the diamonds out; the themes color the list's rows
(`tools/skin/generate.py`).

## Not yet

The Drum Map Setup dialog itself, with its list of maps, renaming a map in the UI, the Output pop-up and auditioning
sounds (its functions are in the Map menu and the Edit menu); outputs per sound in playback (the sounds play on the
track's output); MIDI thru through the I-notes and O-notes; name lists without a drum map; Pitch Visibility and its
options; Show Note Length; the grid type Use Snap from Drum Map and snapping to the sound's snap; the drumstick's
modifier keys for velocity, pitch, length and position, and deleting several notes with a drag; Solo Instrument;
O-Note Conversion (p. 1240); Create Drum Map from Instrument (p. 1238); reordering the list by dragging; the score
settings (Display Note, Noteheads, Voice, Instrument Type, Technique); importing the reference's own drum map files.

## Checks

- `tests/drummaps_tests.cpp`: the GM Map of a new project, New Map and New Copy, changing sounds and the order with
  their errors and undo, a track's map refused for audio tracks and unknown maps, removing a map taking it from the
  track; export and import round trip of a changed map, with errors for a missing and a wrong file; maps and the
  tracks' maps saved and loaded; playback through the O-notes, channels and mutes; step input and recording through
  the I-notes with Drum-Map Link; the drum editor content and parts of a track with a map opening in it.
- `tests/drumeditor_ui_tests.cpp` drives the app with `ui::Automation`: the sound list with the map's names, diamonds
  on their sounds' rows, the mute button and typing an O-note, a channel and a name; the drumstick inserting a note
  with Drum-Map Link, deleting it and inserting a row of notes with a drag; the Map menu adding a map that becomes the
  track's, removing it and choosing another.
