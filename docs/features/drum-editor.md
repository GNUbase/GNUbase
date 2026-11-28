# Drum editor and drum maps

The drum editor (#129, #882, reference manual pp. 1221-1242): the MIDI editors' content
([midi-editors.md](midi-editors.md)) for drum parts, with the drum sound list of the track's drum map, notes as
diamonds, the drumstick and the controller lanes ([controller-lanes.md](controller-lanes.md)); and drum maps, which
change how the notes of the tracks that use them play and come in.

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
| `track.set gmNames` | The track's name list while it has no map: `true` for GM Default (General MIDI's percussion names), `false` for No Names; saved with the project and an undo step | p. 1239 |
| `midi.oNoteConversion` | O-Note Conversion: the notes of tracks with a map (`parts`, `notes`) get their sound's O-note as their pitch | p. 1240 |

They emit `drumMap.added`, `drumMap.changed` and `drumMap.removed` with the map (its id for removed).

- **Playback** (`src/engine/document/rendersnapshot.cpp`): the render snapshot plays each note of a track with a map
  as its sound: the O-note on the sound's channel, or the note's own with Any; a muted sound's notes don't play. The
  notes keep their pitch, so switching maps switches the instrument's sounds.
- **Input**: notes played into a track with a map take the pitch of the first sound, by pitch, whose I-note they are:
  `editor.input`'s step and MIDI input and `recording.add`.
- **MIDI thru** (pp. 1239-1240): what is played live into a track with a map, from its MIDI input or `midi.inject`,
  plays as recording it would: through the I-notes to the sound, then the sound's O-note on its channel, not at all if
  the sound is muted. The render snapshot gives each such channel the 128 notes (`RenderChannel::drumInput`), and the
  sequencer maps the notes it gets with them on the audio thread (`MidiSequencer::modifyInput`) before the track's MIDI
  modifiers, instead of Live Input. With `midi.inject`'s `drumSounds` the notes' keys are the sounds' pitches, not
  I-notes: they play their sounds as the track's notes do (the second 128 entries of `drumInput`), so that the drum
  editor auditions a sound whose I-note an earlier sound has too. The notes the built-in MIDI inserts above the divider
  play for them keep the flag (`ProcessEvent::drumSound`, `plugin::Event::DrumSound`), and the sequencer clears it once
  it has mapped a note. The auditioned sounds are no MIDI input: the recorder, which reads the sequencer's input after
  the MIDI inserts above the divider, leaves the flagged events out of recordings and the retrospective buffer
  (`Recorder::sendMidi`). The MIDI insert slots take only built-in effects today (`effectRackFromJson` refuses others,
  and the sequencer makes only `builtin:` ones); a hosted MIDI effect there, once the slots take one, would play events
  of its own that its format adapter makes without the flag: behind one, an auditioned sound would go through the
  I-notes, and the recorder would record those events.
- **Solo Instrument** (p. 1223): while a drum editor is open with `soloInstrument` on, of the tracks with a map that its
  selected notes are on only the notes of the selected notes' sounds play; with no note selected every sound plays. The
  editor views give `Project::soloNotes` (`ProjectModel::setSoloNotesSource`), the selected notes as `editor.get` has
  them, which every render snapshot takes and turns into the soloed pitches of each track with a map, so a selected note
  moved to another sound solos that one, a map assigned to the track afterwards solos its selected notes' sounds, and a
  selected note whose sound Show Drum Sounds in Use by Instrument hides after a change of the pads or the map no longer
  solos it. The notes it silences select no sound slot of an articulation map either. What is played live through the
  I-notes (MIDI thru, `midi.inject`) plays only the soloed sounds too, as a muted sound's notes don't play; the drum
  editor's auditions (`drumSounds`) play every sound that isn't muted. `ProjectModel::updateSoloNotes` asks the editor
  views once per change of a view. It changes what plays, not the project: no undo step, not saved, and the project
  stays as saved, though `project.revision` advances. Render in place, freeze and export render what plays, so while it
  is on they take only the soloed sounds, and a frozen track keeps them after it is off: as with the mixer's solo, what
  is heard is what is rendered, so switch it off before rendering the whole kit.
- **Loading**: a note's pitch must be 0-127 and its channel 0-15 in the GNUbase entry of a project (as the DAWproject
  import clamps them), since the maps and the editors take the pitch as an index; an entry that has others is
  damaged, and the project loads from `project.xml`.
- **Drum map files** are XML: `<DrumMap>` with the name, the snap values (`Quantize`), the sounds by pitch (`Map`),
  the `Order` and the outputs the sounds' `PortIndex` names, the first being the track's. Settings a file doesn't have
  are Empty Map's; an order that isn't each pitch once is the pitch order.
- **The editor views** ([midi-editors.md](midi-editors.md)) have the content `drum`, and `lengthQuantize` also takes
  `drumMap` (Drum-Map Link): new notes are as long as their sound's snap, or without a map the quantize grid. A closed
  editor opens the parts of a track with a drum map in the drum editor while the user setting `editing.useDrumEditor` is
  on, as by default (Use Drum Editor when Drum Map is assigned, pp. 1221, 1603; the preferences' Editors page). The drum
  editor's view also has `pitchVisibility` (Pitch Visibility On/Off) with its options `reverseSounds` (Reverse Drum
  Sound List) and `visibleSounds` (`all`, `withEvents` for Show Drum Sounds with Events, or `inUse` for Show Drum Sounds
  in Use by Instrument, p. 1234), with `soundsInUse`, the pitches of the active track's sounds whose pads its
  instrument plays through the map's O-notes (`playingDrumPads`): the drum machine of a drum track or of the instrument
  track a MIDI track plays to, `null` for an instrument that can't tell. With `inUse` the view's selection leaves out
  the notes of the sounds it hides, so that neither the editor's edits nor Solo Instrument take them. The view also
  has `soloInstrument` (Solo Instrument, p. 1223), `showNoteLength` (Show Note Length On/Off, p. 1227) and `gridType`
  (Grid Type, p. 1227): `quantize` (Use Quantize), `zoom` (Adapt to Zoom, a note value the editor's zoom chooses), or
  `drumMap` (Use Snap from Drum Map), each note snapping to its sound's snap.
- **Sound names** (p. 1239): every editor view has `soundNames`, the names of its active track's drum sounds by pitch:
  its map's, else with the GM Default name list (`track.set gmNames`) General MIDI's, else `null`. The editors show
  these, so an MCP or Lua client reads the same names.

## In the UI

The drum editor is `ui::KeyEditor` (`src/ui/keyeditor.h`) made with `drum`: `ui::MidiEditor` makes it while the view's
content is `drum`. It is the key editor ([key-editor.md](key-editor.md)) with these differences:

```
.key-editor.drum-editor
  .toolbar.key-editor-toolbar
    button.key-tool[key-tool]       Object Selection, Drumstick (key-tool draw), Line, Erase, Mute (the key editor's,
                                    which mutes on the Articulations lane, #1684, #1799)
    #key-length-quantize            Insert Length, menu #key-length-menu with Drum-Map Link (length drumMap)
    #drum-map                       the active track's map's name, or No Drum Map; menu #drum-map-menu
    .key-input[setting=pitchVisibility], #drum-pitch-options (menu #drum-pitch-menu with
                                    menuitem[visible-sounds=withEvents] and menuitem[setting=reverseSounds]),
                                    .key-input[setting=soloInstrument], #drum-names (menu #drum-names-menu with
                                    menuitem[gm-names=false|true]), .key-input[setting=showNoteLength],
                                    #drum-grid-type (menu #drum-grid-menu with menuitem[grid-type])
  .key-editor-body
    .key-editor-keys                the drum sound list: a .key-editor-key.drum-sound[pitch] per sound in the map's
                                    order (.selected for selected notes) with .drum-sound-pitch, without a map and
                                    with GM Default span.drum-sound-name; with a map also
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
  down, also when an earlier sound has its I-note. With Pitch Visibility and Reverse Drum Sound List the list goes
  from the bottom of the map's order; with Show Drum Sounds with Events it has only the sounds that the shown parts
  have notes of, and the grid a row for each of them (all of them while the parts have none); with Show Drum Sounds in
  Use by Instrument those of `soundsInUse`, an option the menu disables while the instrument can't tell. The notes of
  sounds the list hides aren't shown, neither in the note display nor in the controller lanes, leave the selection,
  and the editor's edits leave them out. The first scroll goes to the middle of the rows of the notes the list shows.
- **Names** (p. 1239): the Names menu chooses the active track's name list, No Names or GM Default (`track.set
  gmNames`). Without a map GM Default names the list's sounds; the key editor's keys show the active track's drum sound
  names (the view's `soundNames`) as their text and title, and the keyboard is wider then (`.key-editor.named-keys`,
  with the parts' row and the controller lanes' header as wide, so that they line up with the notes). In the drum
  editor the parts' row and the controller lanes' header are as wide as the drum sound list.
- **Solo Instrument** (p. 1223) switches the view's `soloInstrument`: the sounds of the selected notes, which the list
  marks, play alone. It requires a drum map: without one the button is disabled and unlit.
- **Drumstick** (pp. 1240-1242). A click inserts a note at the snapped position, as long as Insert Length says; a drag
  to the right inserts one at every step of the quantize grid, also when it is released outside the editor, where the
  drawn note and its velocity follow the mouse, also out of the window until the button goes up there, with the
  modifier keys held then ([key-editor.md](key-editor.md)); a click on a note deletes it, and a drag from it to the
  right (4 dp or more) also the notes of its sound that start up to where the mouse is released, in one undo step,
  also when it is released outside the editor. A click that hardly moves deletes only the note clicked, not the
  doubles and close flams under the mouse. The object selection tool's double click does the same: in the empty grid
  it inserts, with a drag to the right at every step of the grid; on a note it deletes it, with a drag to the right
  and Snap on (Ctrl/Cmd not held) the sound's notes up to the release. A move goes from row to row in the list's
  order. While a drag inserts (p. 1241), up or down changes the new notes' velocity by one step of 127 per dp counted
  from 4 dp once the mouse went up or down that far, so that a click that slips keeps Insert Velocity and every value
  past it can be reached, and `.key-editor-velocity` shows it beside the notes; with Alt up or down changes their
  pitch to the sound under the mouse instead, whose length (with Drum-Map Link) and grid (with Use Snap from Drum Map)
  they then take; with Shift a drag moves the note along the grid instead of repeating it; with Snap off (Ctrl/Cmd at
  the press) and Show Note Length it changes the note's length.
- **Map menu** (pp. 1234, 1236, 1239). Choosing a map, or No Drum Map, sends `track.set` for the active track. New Map
  and New Copy add a map that becomes the track's, in one undo step (`drumMap.add track`); Remove removes the track's
  map; Drum Map Setup... opens the setup.
- **Grid Type** (p. 1227): Use Quantize snaps to the quantize grid; Adapt to Zoom to the finest note value, from a half
  note down to a 64th, that is at least 12 dp wide in the grid, so zooming in makes it finer, while Insert Length's
  link stays the quantize grid's value; the split tool and the step input cursor snap to that note value too. Use Snap
  from Drum Map snaps what the drumstick inserts, repeats and moves along, and the notes moved or resized, to the snap
  of the note's sound: a note moved to another row to that of the sound it goes to, which it then is. With it the
  split tool and the step input cursor snap to the quantize grid: neither belongs to a sound, as the notes step input
  enters there can be any.
- **Show Note Length** (p. 1227) shows the notes as boxes of their length, which the object selection tool and the
  drumstick resize at their ends as in the key editor (p. 1242); in its middle the drumstick still deletes the note.
- **Commands** (`app::AppUi`): `editor.openDrum` (MIDI > Open Drum Editor, p. 1221) opens the selected MIDI parts
  in the editor window's drum editor; in the Edit menu, `drumMap.load` (Load Drum Map...) and `drumMap.save` (Save
  Drum Map...) load a `.drm` file into the project and save the drum map of the editor's active track, through the
  OS's file dialogs. The MIDI menu also has `drumMap.setup` (Drum Map Setup...) and `midi.convertONotes` (O-Note
  Conversion, p. 1240), which runs `midi.oNoteConversion` on the selected notes or parts.
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
and auditioning there: MIDI tracks play to a MIDI output port only as a whole yet ([midi-output.md](midi-output.md)), not per sound; name lists other than
GM Default, such as a MIDI device's, and a drum track's pad names as its list without a map; Show Drum Sounds in Use by
Instrument for plugin instruments, which needs a wrapper extension for the pads they play; showing drum notes in the
project window's parts, once parts show their notes (#1313); Create Drum Map from Instrument (p. 1238); reordering the
list by dragging; the score settings (Display Note, Noteheads, Voice, Instrument Type, Technique); importing the
reference's own drum map files.

## Checks

- `tests/drummaps_tests.cpp`: the GM Map of a new project, New Map and New Copy, also for a track in one undo step,
  changing sounds and the order with their errors and undo, a track's map refused for audio tracks and unknown maps,
  removing a map taking it from the track; export and import round trip of a changed map, with errors for a missing and
  a wrong file; maps and the tracks' maps saved and loaded, and a note pitch beyond 0-127 or a channel beyond 0-15 not
  loading; playback and MIDI thru through the I-notes, O-notes, channels and mutes, and `drumSounds` playing the sounds
  themselves but not recorded, also through a chord generator and an arpeggiator above the divider; `inputPitches`
  giving a shared I-note to the first sound by pitch as `inputPitch` does; step input and recording through the I-notes
  with Drum-Map Link; O-Note Conversion; the drum editor content, its view settings and parts of a track with a map
  opening in it unless the preference is off; Solo Instrument playing only the selected notes' sounds, following a
  selected note to another sound, leaving a saved project unmodified, soloing on a map assigned after it went on, no
  longer soloing a selected note whose pad a reset takes out of the sounds in use and soloing it again on undo, and not
  for nothing selected, another content, a closed editor, a track without a map or with it off; the notes played live
  through the I-notes soloed too but the auditions not, and `updateSoloNotes` asking the views once; `visibleSounds` and
  the grid type `zoom`, refusing other values; `soundsInUse` null without a drum machine, a drum track's kit, and a MIDI
  track's sounds through its map's O-notes to the drum track it plays to, and the selection leaving out the notes of the
  sounds it hides; `gmNames` saved and loaded, undone and refused for audio tracks; the view's `soundNames`, null
  without a map or name list, General MIDI's with GM Default and the map's with one.
- `tests/articulationmaps_tests.cpp`: the notes of a drum sound that Solo Instrument silences sending no output events
  of their sound slot, as a muted sound's.
- `tests/midieffects_tests.cpp`: the sequencer clearing `drumSound` on the notes it maps, so that the inserts below the
  divider and the instrument don't get it.
- `tests/drumeditor_ui_tests.cpp` drives the app with `ui::Automation`: the sound list with the map's names, diamonds on
  their sounds' rows, the mute button and typing an O-note, a channel and a name; the drumstick inserting a note with
  Drum-Map Link, deleting it and inserting a row of notes with a drag, and deleting a sound's notes with a drag; the Map
  menu adding a map that becomes the track's, removing it and choosing another; the output typed, Pitch Visibility's
  reverse order, auditioning a sound through the map, also one whose I-note an earlier sound has, Show Note Length's
  boxes, which the object selection tool and the drumstick resize; the drumstick's click deleting only the note clicked,
  a drag deleting when released outside the editor, and a drag to the left not deleting the clicked note's doubles; a
  drag changing the velocity, with Alt the pitch, with Shift the position and with Snap off and Show Note Length the
  length, a slip of less than 4 dp keeping the insert velocity, which shows while dragging; with Alt the sound under the
  mouse's length and snap; every velocity past the 4 dp reachable; a drag released outside the editor inserting, the
  velocity following the mouse out there, the draw going on out of the window and past its edge until the button goes
  up, with Shift held then inserting its one moved note, and a delete drag released out of the window deleting up to
  there, a lost one nothing; Use Snap from Drum Map snapping new notes, a moved note and a resized one to their sound's
  snap, a note moved to another sound to that one's, and the step input cursor to the quantize grid; the object
  selection tool's double click and drag inserting and deleting several notes; the Drum Map Setup renaming a map,
  changing each setting of a sound, refusing a wrong snap, adding and removing maps, following a map added elsewhere,
  and Save... and Load... through the file dialogs; the MIDI menu's Drum Map Setup and O-Note Conversion; Show Drum
  Sounds with Events listing and gridding only the notes' sounds, the notes on their rows and the drumstick inserting on
  one, Solo Instrument's button, disabled and unlit without a map, and the Names menu's GM Default naming the list's
  sounds without a map and the key editor's keys; Show Drum Sounds in Use by Instrument disabled without a drum machine
  and listing a drum track's kit, the editor opening scrolled to the rows of the notes the list shows, not to a note of
  a sound it hides, hiding the notes of the other sounds from the note display and the velocity lane and
  the transposition palette leaving them as they were; Adapt to Zoom snapping the drumstick to the note value that the
  grid's width chooses, its notes as long as the quantize grid's value; named keys, and the drum editor's sound list,
  lining up the notes with their velocity bars and parts.
