# Key editor

The key editor (#127, reference manual pp. 1189-1220): the MIDI editors' content ([midi-editors.md](midi-editors.md))
in the editor window and the lower zone, with the note display, its tools, the transpose palette, the info line, step
and MIDI input, and the inspector's Chord Editing section.

## In the engine

The key editor's state lives in the engine's editor views (`src/engine/document/editors.cpp`), so MCP, Lua and the
UI share it and step and MIDI input work without the UI. Like the rest of a view it is view state: no undo steps, not
saved, and a new project closes the editors.

| View field | What it is | Reference |
|---|---|---|
| `selection` | The selected notes, each `{part, note}`, of the editable parts; removed notes and parts leave it, closing the editor clears it | p. 1199 |
| `stepInput`, `midiInput` | What `editor.input` does with notes played | pp. 1219-1220 |
| `stepPosition` | The step input cursor, project ticks; turning step input on puts it at the active part's start | p. 1220 |
| `moveInsert`, `recordPitch`, `recordVelocity`, `recordReleaseVelocity`, `keepNotes` | The Step/MIDI Input section's switches | p. 1197 |
| `insertVelocity` | The velocity of drawn notes and of input that doesn't record it (0-1, default 100/127) | p. 1195 |
| `lengthQuantize` | The length of new notes: a note value (`1/16`, `1/8T`, `1/4.`) or `link`, the quantize grid's | p. 1196 |

`editor.set` changes them; `editor.get` and `editor.changed` report them.

| Command | What it does | Reference |
|---|---|---|
| `editor.input` | Notes played, such as a chord from a MIDI keyboard: with step input they go into the active part at the step input cursor as notes of the length quantize's length, and the cursor moves on by a step of the quantize grid; with MIDI input the first selected note takes the played pitch and velocities, and the next note is selected | pp. 1219-1220 |
| `note.chord` | The chord notes of a part make (query) | p. 1216 |
| `note.voicing` | Fits notes to a chord type, or inverts them or drops notes | pp. 1215-1217 |
| `note.move` | Moves notes into another MIDI part, by `ticks` on the project's timeline and by `semitones` in pitch, keeping their ids and every other field: what a drag between in-place editors sends ([in-place-editor.md](in-place-editor.md)) | p. 1261 |

- **Step input.** Move Insert Mode first moves the part's notes from the cursor on by a step; without Keep Existing
  Notes the notes that start within the step are replaced. The cursor must be within the active part. The inserted
  notes become the selection. One undo step.
- **MIDI input.** The first selected note (by time, then pitch) takes the lowest played note's pitch, velocity and
  release velocity, as far as Record Pitch, Record Note On Velocity and Record Note Off Velocity say; the other notes
  of a chord are added with its position and length. With Keep Existing Notes all played notes are added and the
  selected note stays. Then the next note of its part is selected, for a series of notes.
- **Chords** (`src/engine/midi/chords.h`). The types are the Chord Editing section's: the triads major, minor,
  diminished, augmented, sus2 and sus4, and the 4-note chords major7, minor7, dominant7, halfDiminished7,
  diminished7 and minorMajor7. Recognizing ignores octaves and order, so arpeggiated notes count; of several roots the
  lowest note's wins. Fitting keeps the root of the chord the notes make (or the lowest note's): from the lowest up,
  each note moves to the nearest chord tone no lower note took yet. Inversions move the lowest note up (or the highest
  down) by octaves; Drop 2, Drop 3 and Drop 2 + 4 move the second, third, or second and fourth highest note down an
  octave.

## In the UI

`ui::KeyEditor` (`src/ui/keyeditor.h`) fills the editor of a `ui::MidiEditor`, the editor window's and the lower
zone's. It shows the project and the editor view as `MidiEditor` hands them on and sends `note.*` and `editor.set`;
the edits of several parts go in one `history.transaction`.

```
.key-editor[tool=objectSelection|draw|line|erase|trim|split|glue]
  .toolbar.key-editor-toolbar
    button.key-tool[key-tool]         the tools; .checked for the current one; the chosen line tool opens
                                      #key-line-menu with its modes (controller-lanes.md)
    button.key-transpose[semitones]   Move Up, Move Down, Move Up More, Move Down More (+1, -1, +12, -12)
    button.key-input[setting]         Step Input, MIDI Input, Move Insert Mode, Record Pitch, Record Note On and
                                      Note Off Velocity, Keep Existing Notes, Auto Select Controllers; .checked
                                      while on
    #key-length-quantize              Length Quantize, menu #key-length-menu (Quantize Link, 1/1 to 1/64, triplets)
    valuefield#key-insert-velocity    Insert Velocity, 1-127
  .info-line.key-editor-info          the first selected note: textdisplay[field=start|end|length|pitch|velocity|
                                      releaseVelocity|channel]; .empty without, .multiple with several selected
  .editor-parts                       an .editor-part-row[track] per track, its .editor-part[part] at their time
  .key-editor-body                    scrolls vertically
    .key-editor-keys                  the piano keyboard: 128 .key-editor-key[pitch], the 75 white keys first, then
                                      the 53 .black ones over them; C keys named, .selected for selected notes
      .key-editor-keys-zoom           the keyboard's left part, where a drag zooms and scrolls (below); the pointer
                                      is the four-point arrow over it
    .key-editor-grid                  128 .key-editor-row (.black); .key-editor-notes with a .key-note[part][note]
                                      per note (.selected, .inactive outside the part editing mode, .drawing);
                                      .key-editor-step-cursor (.off while step input is off)
  .controller-lanes                   the controller lanes (controller-lanes.md)
#key-editor-toolbox                   menu.toolbox with a menuitem[key-tool] per tool, [checked] for the current one
.editor-chord-editing                 the inspector's Chord Editing section: .editor-chord-type (the chord's name),
                                      button.editor-chord[chord] and button.editor-voicing[voicing]
```

The note display fits the shown parts' range into its width; each pitch is a row of the height `keyeditor.rcss` gives
it, highest at the top, and it first scrolls to the middle of the notes. Code places the notes, parts and the cursor
in percent of the grid. Positions snap to the project's quantize grid; Ctrl (Cmd) turns snapping off.

The keyboard beside the grid is a piano keyboard (p. 1204), as high as the grid's rows. Each black key is exactly its
pitch's row and narrower than the white keys, drawn over them at the left. The 7 white keys of an octave span the
keyboard's width and share the octave's 12 rows equally, from the bottom of the C's row up, so that the right part of a
white key reaches into the rows of the black keys beside it; the top of the keyboard cuts G8, the highest key. Code
places the keys' tops and heights in percent of the keyboard; `keyeditor.rcss` gives the widths and the themes the look,
white keys light and black keys dark in every theme. A C key's name sits at its right, level with its own row. Over the
keyboard's left part the pointer is the four-point arrow (`.key-editor-keys-zoom`): a drag to the right zooms the
pitches vertically in and to the left out, doubling or halving the rows' height every 100 dp dragged, between a quarter
and four times the height the style sheet gives them, and a drag up or down scrolls the note display by as much as the
mouse went (p. 1261). The pitch row at the top of the view when the drag started stays there. The zoom is the editor's
own state, like its scroll, and the keyboard's height follows the rows'. A press there plays the pitch under it as the
rest of the keyboard does, and a drag of a few dp turns the press into the gesture. Pressing a key plays its pitch on
the active part's track until the mouse is released, wherever that is: `midi.inject` sends a note on (velocity 100) into
the track's channel as if from its MIDI input, so its instrument plays it, and then the note off, also whenever the key
editor goes while the key is held: the editor window closing or being closed, the editor showing the list, pattern or
audio part editor instead, the lower zone's Editor panel going (its tab hidden, the panel opened in a window of its own,
or that window closed or docked again), an in-place editor closing, or the switch to the drum editor, which builds a key
editor of its own. A press on a key selects and deselects no notes.

| Tool | Gesture | Reference |
|---|---|---|
| Object Selection | A click selects a note, Ctrl (Cmd) or Shift and a click adds or removes it, a click in the empty grid deselects (with step input on, it also moves the step input cursor); a drag moves the selected notes in time and pitch, at a note's ends it resizes them; a double click inserts a note of the length quantize's length, on a note deletes it | pp. 1211-1214, 1220 |
| Draw | A click draws a note of the length quantize's length, a drag a multiple of it | p. 1212 |
| Erase | A click deletes the note | p. 1212 |
| Trim | A click cuts off the note's end from the mouse on, with Alt its start up to the mouse | p. 1215 |
| Split | A click splits the note at the snapped position; on a selected note, every selected note there (`note.split`: the rests keep the notes' values and their expression from the cut on; as with `event.split`, both pieces lose their quantize origin, so `quantize.reset` no longer moves them) | p. 1215 |
| Glue | A click glues the note to the next one of its pitch; the first one's values stay, the next one's expression joins its own (`note.glue`) | p. 1215 |

- **Info line** (p. 1199). Start, end and length in bars and beats, the pitch as a name such as C3 (typing a number
  works too), velocities 0-127, the channel 1-16 and the Articulations, the attributes of the track's articulation
  map that the note has, names separated by commas ([articulation-maps.md](articulation-maps.md)). A value typed
  moves every selected note by as much as the first one's changes; the channel and the attributes are set for all.
- **Chord Editing** (pp. 1215-1218) is in the editor window's inspector, under Global Tracks: the chord the selected
  notes make ("No Chord" for none), and buttons that send `note.voicing` for each part's selected notes.
- **Toolbox** (p. 65). A right click in the note display opens `#key-editor-toolbox`, the toolbar's tools at the
  mouse, by the user setting `ui.toolboxOnRightClick` as in the event display ([tools.md](tools.md)): with it on, a
  right click without a modifier key; off, a right click with one. A click on a tool chooses it. The other right click
  opens nothing yet, as the key editor has no context menu: the grid's `contextmenu` or `modifiercontextmenu` is
  empty. A right click doesn't change the selection.
- **Clicking a part** in the row of parts activates it, as `MidiEditor` handles clicks on `[part]` (p. 1152); a click
  on a note activates its part too.

## Not yet

Live MIDI keyboard input into `editor.input` (it waits for the MIDI recording input path, #97); Left and Right moving
the step input cursor and lengthening a held chord; the Mute, Range Selection, Zoom and Time Warp tools (notes have no
mute yet), and the Line tool in the note display (it draws on the controller lanes); the selection rectangle, the trim
tool's drag across several notes and the draw tool's modifiers for velocity, pitch and position; key commands for the
transpose palette and deleting; the nudge buttons, Solo Editor, Record in Editor, Acoustic Feedback, pitch visibility,
the Scale Assistant, Indicate Transpositions, event colors, the status line and Set up Window Layout; horizontal zoom
and scrolling; the key at the mouse's pitch lit while the mouse moves in the note display (p. 1204); drum map names in
the key editor and articulation names; Add to Chord Track, Match with Chord Track, Create Chord Events and the chord
insert tools; quantizing from the toolbar (#132) and the Length section's functions (#133); the key editor's context
menu.

## Checks

- `tests/keyeditor_tests.cpp`: the selection as view state of the editable parts, refused for notes that don't exist
  or parts that aren't editable, leaving with removed notes and with closing; step input of notes and chords with
  the length quantize, the quantize grid's step, the insert velocity, a rest by moving the cursor, one undo step,
  replacing and keeping existing notes, Move Insert Mode and a cursor outside the part; MIDI input with and without
  Record Pitch, a chord, Keep Existing Notes and no selection; chord recognition, fitting and voicings; `note.chord`
  and `note.voicing` with undo and their errors.
- `tests/keyeditor_ui_tests.cpp` drives the app with `ui::Automation`: the keyboard and notes at their time and pitch,
  keys lit for selected notes; the piano keyboard at 1024x600, at dp ratios of 1.5 and 1: white keys of equal height
  across the keyboard, black keys narrower on their rows, the mouse on the right part of a white key beside a black key
  over the white key, and a golden image of two octaves with a black and a white key lit
  (`tests/golden/ui/key-editor-keyboard.png`); pressing the right part of a white key beside a black key and the black
  key playing their pitches on the track's instrument until the mouse is released, also outside the editor, leaving the
  selected notes as they were, and a held key's pitch ending when the editor switches to the drum editor, closes through
  the engine API or has its window closed, and when the lower zone's Editor panel in a window of its own has that window
  closed or is docked again; the object selection tool selecting, deselecting, moving, resizing, inserting and deleting;
  the draw tool with Quantize Link and an eighth from the Length Quantize menu; the erase, trim (and with Alt), split
  and glue tools, and the split tool splitting the selected notes of two parts at one position in one undo step; the
  transpose palette and typing a pitch on the info line; step input with its cursor, a click moving it, and MIDI input
  selecting the next note; the Chord Editing section naming and changing a chord; the toolbox with the setting on and
  off, with and without a modifier key, choosing tools; drags on the keyboard's left part zooming the rows in and out
  between the limits, from the height a style sheet gives them, and scrolling the note display, while a click there
  zooms nothing, and a click and a drag there leaving a note an earlier press selected and the history alone.
