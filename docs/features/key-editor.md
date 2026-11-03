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
.key-editor[tool=objectSelection|draw|erase|trim|split|glue]
  .toolbar.key-editor-toolbar
    button.key-tool[tool]             the tools; .checked for the current one
    button.key-transpose[semitones]   Move Up, Move Down, Move Up More, Move Down More (+1, -1, +12, -12)
    button.key-input[setting]         Step Input, MIDI Input, Move Insert Mode, Record Pitch, Record Note On and
                                      Note Off Velocity, Keep Existing Notes; .checked while on
    #key-length-quantize              Length Quantize, menu #key-length-menu (Quantize Link, 1/1 to 1/64, triplets)
    valuefield#key-insert-velocity    Insert Velocity, 1-127
  .info-line.key-editor-info          the first selected note: textdisplay[field=start|end|length|pitch|velocity|
                                      releaseVelocity|channel]; .empty without, .multiple with several selected
  .editor-parts                       an .editor-part-row[track] per track, its .editor-part[part] at their time
  .key-editor-body                    scrolls vertically
    .key-editor-keys                  128 .key-editor-key[pitch], .black, C keys named, .selected for selected notes
    .key-editor-grid                  128 .key-editor-row (.black); .key-editor-notes with a .key-note[part][note]
                                      per note (.selected, .inactive outside the part editing mode, .drawing);
                                      .key-editor-step-cursor (.off while step input is off)
.editor-chord-editing                 the inspector's Chord Editing section: .editor-chord-type (the chord's name),
                                      button.editor-chord[chord] and button.editor-voicing[voicing]
```

The note display fits the shown parts' range into its width; each pitch is a row of the height `keyeditor.rcss` gives
it, highest at the top, and it first scrolls to the middle of the notes. Code places the notes, parts and the cursor
in percent of the grid. Positions snap to the project's quantize grid; Ctrl (Cmd) turns snapping off.

| Tool | Gesture | Reference |
|---|---|---|
| Object Selection | A click selects a note, Ctrl (Cmd) or Shift and a click adds or removes it, a click in the empty grid deselects (with step input on, it also moves the step input cursor); a drag moves the selected notes in time and pitch, at a note's ends it resizes them; a double click inserts a note of the length quantize's length, on a note deletes it | pp. 1211-1214, 1220 |
| Draw | A click draws a note of the length quantize's length, a drag a multiple of it | p. 1212 |
| Erase | A click deletes the note | p. 1212 |
| Trim | A click cuts off the note's end from the mouse on, with Alt its start up to the mouse | p. 1215 |
| Split | A click splits the note at the snapped position; on a selected note, every selected note there | p. 1215 |
| Glue | A click glues the note to the next one of its pitch; the first one's values stay | p. 1215 |

- **Info line** (p. 1199). Start, end and length in bars and beats, the pitch as a name such as C3 (typing a number
  works too), velocities 0-127 and the channel 1-16. A value typed moves every selected note by as much as the first
  one's changes; the channel is set for all.
- **Chord Editing** (pp. 1215-1218) is in the editor window's inspector, under Global Tracks: the chord the selected
  notes make ("No Chord" for none), and buttons that send `note.voicing` for each part's selected notes.
- **Clicking a part** in the row of parts activates it, as `MidiEditor` handles clicks on `[part]` (p. 1152); a click
  on a note activates its part too.

## Not yet

Live MIDI keyboard input into `editor.input` (it waits for the MIDI recording input path, #97); Left and Right moving
the step input cursor and lengthening a held chord; the Mute, Range Selection, Zoom, Time Warp and Line tools (notes
have no mute yet); the selection rectangle, the trim tool's drag across several notes and the draw tool's modifiers
for velocity, pitch and position; key commands for the transpose palette and deleting; the nudge buttons, Solo
Editor, Record in Editor, Acoustic Feedback, Auto Select Controllers, pitch visibility, the Scale Assistant,
Indicate Transpositions, event colors, the status line and Set up Window Layout; horizontal zoom and scrolling;
listening to a key's pitch with a click; drum map and articulation names; Add to Chord Track, Match with Chord Track,
Create Chord Events and the chord insert tools; controller lanes (#128), quantizing from the toolbar (#132) and the
Length section's functions (#133).

## Checks

- `tests/keyeditor_tests.cpp`: the selection as view state of the editable parts, refused for notes that don't exist
  or parts that aren't editable, leaving with removed notes and with closing; step input of notes and chords with
  the length quantize, the quantize grid's step, the insert velocity, a rest by moving the cursor, one undo step,
  replacing and keeping existing notes, Move Insert Mode and a cursor outside the part; MIDI input with and without
  Record Pitch, a chord, Keep Existing Notes and no selection; chord recognition, fitting and voicings; `note.chord`
  and `note.voicing` with undo and their errors.
- `tests/keyeditor_ui_tests.cpp` drives the app with `ui::Automation`: the keyboard and notes at their time and pitch,
  keys lit for selected notes; the object selection tool selecting, deselecting, moving, resizing, inserting and
  deleting; the draw tool with Quantize Link and an eighth from the Length Quantize menu; the erase, trim (and with
  Alt), split and glue tools; the transpose palette and typing a pitch on the info line; step input with its cursor,
  a click moving it, and MIDI input selecting the next note; the Chord Editing section naming and changing a chord.
