# Drum editor and drum maps

The drum editor (#129, #882, reference manual pp. 1221-1242): the MIDI editors' content ([midi-editors.md](midi-editors.md))
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
| `channel` | 0-15, the MIDI channel it plays on, or `null` (or -1, as Lua sends it) for Any: the note's own | p. 1238 |
| `output` | The MIDI output it plays on, empty for the track's (Default) | p. 1238 |

| Command | What it does | Reference |
|---|---|---|
| `drumMap.add` | New Map (Empty Map, the sounds Sound 1 to Sound 128) or New Copy of a map (`copy`), with the settings given; `track` then uses it, in the same undo step | p. 1236 |
| `drumMap.set` | Changes a map's name, sounds (by pitch) and order; the change goes to every track using it | p. 1237 |
| `drumMap.remove` | Removes a map; the tracks that used it have none | p. 1236 |
| `drumMap.export` | Saves a map as a drum map file (`.drm`) | p. 1236 |
| `drumMap.import` | Loads a drum map file into the project as a new map | p. 1236 |
| `track.set drumMap` | The map a MIDI or instrument track's notes play through, 0 for none | p. 1239 |
| `midi.oNoteConversion` | O-Note Conversion: the notes of tracks with a map (`parts`, `notes`) get their sound's O-note as their pitch | p. 1240 |

They emit `drumMap.added`, `drumMap.changed` and `drumMap.removed` with the map (its id for removed).

- **Playback** (`src/engine/document/rendersnapshot.cpp`): the render snapshot plays each note of a track with a map
  as its sound: the O-note on the sound's channel, or the note's own with Any; a muted sound's notes don't play. The
  notes keep their pitch, so switching maps switches the instrument's sounds.
- **Input**: notes played into a track with a map take the pitch of the first sound, by pitch, whose I-note they are:
  `editor.input`'s step and MIDI input and `recording.add`.
- **MIDI thru** (pp. 1239-1240): what is played live into a track with a map, from its MIDI input or `midi.inject`,
  plays as recording it would: through the I-notes to the sound, then the sound's O-note on its channel, not at all
  if the sound is muted. The render snapshot gives each such channel the 128 notes (`RenderChannel::drumInput`), and
  the sequencer maps the notes it gets with them on the audio thread (`MidiSequencer::modifyInput`) before the
  track's MIDI modifiers, instead of Live Input. With `midi.inject`'s `drumSounds` the notes' keys are the sounds'
  pitches, not I-notes: they play their sounds as the track's notes do (the second 128 entries of `drumInput`), so
  that the drum editor auditions a sound whose I-note an earlier sound has too. They are no MIDI input: the recorder
  leaves them out of recordings and the retrospective buffer (`Recorder::sendMidi`).
- **Loading**: a note's pitch must be 0-127 and its channel 0-15 in the GNUbase entry of a project (as the DAWproject
  import clamps them), since the maps and the editors take the pitch as an index; an entry that has others is
  damaged, and the project loads from `project.xml`.
- **Drum map files** are XML: `<DrumMap>` with the name, the snap values (`Quantize`), the sounds by pitch (`Map`),
  the `Order` and the outputs the sounds' `PortIndex` names, the first being the track's. Settings a file doesn't have
  are Empty Map's; an order that isn't each pitch once is the pitch order.
- **The editor views** ([midi-editors.md](midi-editors.md)) have the content `drum`, and `lengthQuantize` also takes
  `drumMap` (Drum-Map Link): new notes are as long as their sound's snap, or without a map the quantize grid. A closed
  editor opens the parts of a track with a drum map in the drum editor while the user setting `editing.useDrumEditor`
  is on, as by default (Use Drum Editor when Drum Map is assigned, pp. 1221, 1603; the preferences' Editors page).
  The drum editor's view also has `pitchVisibility` (Pitch Visibility On/Off) with its option `reverseSounds`
  (Reverse Drum Sound List), `showNoteLength` (Show Note Length On/Off, p. 1227) and `gridType` (Grid Type, p. 1227):
  `quantize` (Use Quantize), or `drumMap` (Use Snap from Drum Map), each note snapping to its sound's snap.

## In the UI

The drum editor is `ui::KeyEditor` (`src/ui/keyeditor.h`) made with `drum`: `ui::MidiEditor` makes it while the view's
content is `drum`. It is the key editor ([key-editor.md](key-editor.md)) with these differences:

```
.key-editor.drum-editor
  .toolbar.key-editor-toolbar
    button.key-tool[key-tool]       Object Selection, Drumstick (key-tool draw), Line, Erase
    #key-length-quantize            Insert Length, menu #key-length-menu with Drum-Map Link (length drumMap)
    #drum-map                       the active track's map's name, or No Drum Map; menu #drum-map-menu
    .key-input[setting=pitchVisibility], #drum-pitch-options (menu #drum-pitch-menu with
                                    menuitem[setting=reverseSounds]), .key-input[setting=showNoteLength],
                                    #drum-grid-type (menu #drum-grid-menu with menuitem[grid-type])
  .key-editor-body
    .key-editor-keys                the drum sound list: a .key-editor-key.drum-sound[pitch] per sound in the map's
                                    order (.selected for selected notes) with .drum-sound-pitch; with a map also
                                    button.drum-sound-mute (.checked while muted), textdisplay.drum-sound-name and
                                    textdisplay.drum-sound-field[sound-field=snap|iNote|oNote|channel|output]
    .key-editor-grid                a .key-editor-row per sound; the notes .key-note.drum-note, diamonds at their
                                    start on their sound's row, or with Show Note Length .key-note boxes
#drum-map-menu                      menuitem[drum-map] for No Drum Map (0) and each map, [checked] for the track's;
                                    menuitem[drum-map-action=new|copy|remove|setup]
```

- **Drum sound list** (pp. 1233-1234). The rows follow the map's order; without a map they go up from pitch 0 and name
  the pitches only. A double click edits a field, which sends `drumMap.set`: the name, the snap as a note value, the
  I-note and O-note as a name such as C1 or a number, the channel as 1-16 or Any, the output as a name or Default. The
  mute button mutes the sound. A click on a sound's pitch auditions it (p. 1236): it goes into the track as if
  played (`midi.inject` with `drumSounds`), so the map plays it on the sound's O-note and channel while the mouse is
  down, also when an earlier sound has its I-note. With
  Pitch Visibility and Reverse Drum Sound List the list goes from the bottom of the map's order.
- **Drumstick** (pp. 1240-1242). A click inserts a note at the snapped position, as long as Insert Length says; a drag
  to the right inserts one at every step of the quantize grid; a click on a note deletes it, and a drag from it to the
  right (4 dp or more) also the notes of its sound that start up to where the mouse is released, in one undo step, also
  when it is released outside the editor. A click that hardly moves deletes only the note clicked, not the doubles and
  close flams under the mouse. The object selection tool's double click does the same: in the empty grid it inserts,
  with a drag to the right at every step of the grid; on a note it deletes it, with a drag to the right and Snap on
  (Ctrl/Cmd not held) the sound's notes up to the release. A move goes from row to row in the list's order.
  While a drag inserts (p. 1241), up or down changes the new notes' velocity by one step of 127 per dp, with Alt their
  pitch to the sound under the mouse; with Shift a drag moves the note along the grid instead of repeating it; with
  Snap off (Ctrl/Cmd at the press) and Show Note Length it changes the note's length.
- **Map menu** (pp. 1234, 1236, 1239). Choosing a map, or No Drum Map, sends `track.set` for the active track. New Map
  and New Copy add a map that becomes the track's, in one undo step (`drumMap.add track`); Remove removes the track's
  map; Drum Map Setup... opens the setup.
- **Grid Type** (p. 1227): Use Quantize snaps to the quantize grid; Use Snap from Drum Map snaps what the drumstick
  inserts, repeats and moves along, and the notes moved or resized, to the snap of the note's sound.
- **Show Note Length** (p. 1227) shows the notes as boxes of their length, which the object selection tool and the
  drumstick resize at their ends as in the key editor (p. 1242); in its middle the drumstick still deletes the note.
- **Commands** (Edit menu, `app::AppUi`): `editor.openDrum` (Open Drum Editor, p. 1221) opens the selected MIDI parts
  in the editor window's drum editor; `drumMap.load` (Load Drum Map...) and `drumMap.save` (Save Drum Map...) load a
  `.drm` file into the project and save the drum map of the editor's active track, through the OS's file dialogs. The
  MIDI menu has `drumMap.setup` (Drum Map Setup...) and `midi.convertONotes` (O-Note Conversion, p. 1240), which runs
  `midi.oNoteConversion` on the selected notes or parts.
- **Drum Map Setup** (pp. 1235-1237, `resources/ui/drum-maps.rml`): the project's maps in a list, the editor's active
  track's first, with the selected map's name field (renaming it) and its sounds in the map's order, each with its
  pitch, name, snap, mute, I-note, O-note, channel and output, which change with Enter or when a field loses the focus
  (`drumMap.set`); New Map, New Copy, Remove, Save... (`gnubase.ui.saveDrumMap`, the OS's file dialog for the map) and
  Load... (`drumMap.load`). A change the engine refuses shows its message in `#drum-maps-message`. It follows
  `drumMap.added`, `changed` and `removed`, so a map loaded or changed elsewhere shows.

`resources/styles/keyeditor.rcss` lays the list and the diamonds out; the themes color the list's rows
(`tools/skin/generate.py`).

## Not yet

Outputs per sound in playback and MIDI thru (the sounds play on the track's output), so also the setup's Output pop-up
and auditioning there; name lists without a drum map and drum sound names in the key editor (p. 1239); Pitch
Visibility's Show Drum Sounds with Events and Show Drum Sounds in Use by Instrument; the grid type Adapt to Zoom; Solo
Instrument; showing drum notes in the project window's parts; Create Drum Map from Instrument (p. 1238); reordering the
list by dragging; the score settings (Display Note, Noteheads, Voice, Instrument Type, Technique); importing the
reference's own drum map files.

## Checks

- `tests/drummaps_tests.cpp`: the GM Map of a new project, New Map and New Copy, also for a track in one undo step,
  changing sounds and the order with their errors and undo, a track's map refused for audio tracks and unknown maps,
  removing a map taking it from the track; export and import round trip of a changed map, with errors for a missing
  and a wrong file; maps and the tracks' maps saved and loaded, and a note pitch beyond 0-127 or a channel beyond 0-15
  not loading; playback and MIDI thru through the I-notes, O-notes, channels and mutes, and `drumSounds` playing the
  sounds themselves but not recorded; `inputPitches` giving a shared I-note to the first sound by pitch as `inputPitch` does; step
  input and recording through the I-notes with Drum-Map Link; O-Note Conversion; the drum editor content, its view
  settings and parts of a track with a map opening in it unless the preference is off.
- `tests/drumeditor_ui_tests.cpp` drives the app with `ui::Automation`: the sound list with the map's names, diamonds
  on their sounds' rows, the mute button and typing an O-note, a channel and a name; the drumstick inserting a note
  with Drum-Map Link, deleting it and inserting a row of notes with a drag, and deleting a sound's notes with a drag;
  the Map menu adding a map that becomes the track's, removing it and choosing another; the output typed, Pitch
  Visibility's reverse order, auditioning a sound through the map, also one whose I-note an earlier sound has, Show
  Note Length's boxes, which the object selection tool and the drumstick resize; the drumstick's click deleting only
  the note clicked, a drag deleting when released outside the editor, and a drag to the left not deleting the
  clicked note's doubles; a drag changing the velocity, with Alt the pitch, with Shift the position and with Snap off
  and Show Note Length the length; Use Snap from Drum Map snapping new notes to their sound's snap; the object selection tool's double click and drag inserting and deleting several
  notes; the Drum Map Setup renaming a map, changing each setting of a sound, refusing a wrong snap, adding and
  removing maps, following a map added elsewhere, and Save... and Load... through the file dialogs; the MIDI menu's
  Drum Map Setup and O-Note Conversion.
