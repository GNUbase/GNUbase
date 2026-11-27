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
| `acousticFeedback` | Acoustic Feedback: the editor plays a note while it is moved, at the pitch it is dragged to, a note while it is drawn, and a note whose velocity the velocity lane edits, at the velocity a value drag sets or the draw tool paints (controller-lanes.md), until the mouse is released, on the active track's instrument as a key of the keyboard does (off by default) | pp. 1192, 1259 |
| `indicateTranspositions`, `transpositions` | Indicate Transpositions: the note display shows each note at the pitch it plays, its part's own transpose, the root key's step and the transpose track's added as playback adds them (transpose.md). While it is on, `transpositions` gives each MIDI part the view shows as `{part, steps}`, each step `{position, semitones}` in project ticks from the part's start and where a transpose event changes it, and nothing while it is off. A note drawn gets the pitch that plays at the row under the mouse, a move changes a note's pitch by the rows dragged; the drum editor shows the notes' own pitches | pp. 419, 1260 |
| `showExpression`, `expressionDoubleClick`, `expressionType` | Show Note Expression Data, Double-Click Opens Note Expression Editor and the expression the expression editor edits ([note-expression.md](note-expression.md)) | pp. 1303, 1308-1309 |

`editor.set` changes them; `editor.get` and `editor.changed` report them.

| Command | What it does | Reference |
|---|---|---|
| `editor.input` | Notes played, such as a chord from a MIDI keyboard: with step input they go into the active part at the step input cursor as notes of the length quantize's length, and the cursor moves on by a step of the quantize grid; with MIDI input the first selected note takes the played pitch and velocities, and the next note is selected | pp. 1219-1220 |
| `note.chord` | The chord notes of a part make (query) | p. 1216 |
| `note.voicing` | Fits notes to a chord type, or inverts them or drops notes | pp. 1215-1217 |
| `note.move` | Moves notes into another MIDI part, by `ticks` on the project's timeline and by `semitones` in pitch, keeping their ids and every other field, with the selected controller events (`controllers`) shifted in time: what a drag between in-place editors sends ([in-place-editor.md](in-place-editor.md)) | pp. 1188, 1261 |

- **Step input.** Move Insert Mode first moves the part's notes from the cursor on by a step, or by the longest
  inserted note where that is longer, so that they don't overlap; without Keep Existing Notes the notes that start
  within the step are replaced. The cursor must be within the active part, and inserted notes end at its end at the
  latest. They take the MIDI channel of the part's last note before the cursor that stays, else of its first note
  after it; in an empty part channel 1, as drawn notes. The inserted notes become the selection. One undo step, "Note
  input", as for MIDI input.
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
.key-editor[tool=objectSelection|draw|line|erase|trim|split|glue|mute]
  .toolbar.key-editor-toolbar
    button.key-tool[key-tool]         the tools; .checked for the current one; the chosen line tool opens
                                      #key-line-menu with its modes (controller-lanes.md)
    button.key-transpose[semitones]   Move Up, Move Down, Move Up More, Move Down More (+1, -1, +12, -12)
    button.key-input[setting]         Step Input, MIDI Input, Move Insert Mode, Record Pitch, Record Note On and
                                      Note Off Velocity, Keep Existing Notes, Auto Select Controllers, Acoustic
                                      Feedback; .checked while on
    #key-length-quantize              Length Quantize, menu #key-length-menu (Quantize Link, 1/1 to 1/64, triplets)
    valuefield#key-insert-velocity    Insert Velocity, 1-127
    button.key-input[setting=showExpression|expressionDoubleClick]
                                      Show Note Expression Data and Double-Click Opens Note Expression Editor
                                      (not in the drum editor); .checked while on
    button.key-input[setting=indicateTranspositions]
                                      Indicate Transpositions (not in the drum editor); .checked while on
  .info-line.key-editor-info          the first selected note: textdisplay[field=start|end|length|pitch|velocity|
                                      releaseVelocity|channel]; .empty without, .multiple with several selected
  .editor-parts                       an .editor-part-row[track] per track, its .editor-part[part] at their time
  .key-editor-body                    scrolls vertically
    .key-editor-keys                  the piano keyboard: 128 .key-editor-key[pitch], the 75 white keys first, then
                                      the 53 .black ones over them; C keys named, .selected for selected notes. With
                                      the active track's drum sound names (the view's soundNames, drum-editor.md)
                                      the keys show them as their text and title, and the editor's root has
                                      .named-keys, which makes the keyboard, the parts' margin and the controller
                                      lanes' header 128dp wide
      .key-editor-keys-zoom           the keyboard's left part, where a drag zooms and scrolls (below); the pointer
                                      is the four-point arrow over it
    .key-editor-grid                  128 .key-editor-row (.black); .key-editor-notes with a .key-note[part][note]
                                      per note (.selected, .inactive outside the part editing mode, .drawing);
                                      .key-editor-step-cursor (.off while step input is off)
                                      with Show Note Expression Data a .key-expression-step per point in a .key-note
                                      (steps narrower than a pixel joined; the lines take no mouse events);
      .key-expression-editor          the expression editor (.off while closed): button.key-expression-type (Parameter
                                      Selection, menu #key-expression-menu) and .key-expression-area with the
                                      selected notes' .key-expression-step, which take no mouse events either
                                      (note-expression.md)
  .controller-lanes                   the controller lanes (controller-lanes.md)
#key-editor-toolbox                   menu.toolbox with a menuitem[key-tool] per tool, [checked] for the current one
.editor-chord-editing                 the inspector's Chord Editing section: .editor-chord-type (the chord's name),
                                      button.editor-chord[chord] and button.editor-voicing[voicing]
```

The note display fits the shown parts' range into its width; each pitch is a row of the height `keyeditor.rcss` gives
it, highest at the top, and it first scrolls to the middle of the notes, after the first layout that gives the note
display its size (`fitLines`, after its window's update). Code places the notes, parts and the cursor in percent of the
grid. Positions snap to the project's quantize grid; Ctrl (Cmd) turns snapping off.

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
own state, like its scroll. Only the style sheet gives the rows' height, and the keyboard stretches to the rows; the
zoom scales that height as the style sheet gives it at each press there, so a theme or style change reaches an editor at
zoom 1 at once and a zoomed one at its next press there, and a height that is no length (a percentage or auto) has
nothing to scale. Within 5 dp of zoom 1 a drag snaps to it, since a drag back rarely ends on exactly the dp it started
from. No theme gives the rows a height of its own yet; one that does would need the zoom applied again where the theme
is activated. A press there plays the pitch under it as the rest of the keyboard does, and a drag of a few dp turns
the press into the gesture. Pressing a key plays its pitch on the active part's track until the mouse is released,
wherever that is: `midi.inject` sends a note on (velocity 100) into the track's channel as if from its MIDI input, so
its instrument plays it, and then the note off, also whenever the key editor goes while the key is held: the editor
window closing or being closed, the editor showing the list, pattern or audio part editor instead, the lower zone's
Editor panel going (its tab hidden, the panel opened in a window of its own, or that window closed or docked again), an
in-place editor closing, or the switch to the drum editor, which builds a key editor of its own. A press on a key
selects and deselects no notes.

A press in the note display gives the editor the keyboard focus. Its element is the focus zone `keyEditor`, where Up
and Down transpose the selected notes by a semitone, with Shift by an octave, and Delete and Backspace delete them
(p. 1214): the key commands `keyEditor.transposeUp`, `keyEditor.transposeDown`, `keyEditor.transposeOctaveUp`,
`keyEditor.transposeOctaveDown` and `keyEditor.delete` ([key-commands.md](key-commands.md), #679), which the Key
Commands dialog binds to other keys. In the lower zone they come before the project window's keys, so Delete deletes
the notes, not the project window's selected events.

| Tool | Gesture | Reference |
|---|---|---|
| Object Selection | A click selects a note, Ctrl (Cmd) or Shift and a click adds or removes it, a click in the empty grid deselects (with step input on, it also moves the step input cursor); a drag moves the selected notes in time and pitch, at a note's ends it resizes them; a double click inserts a note of the length quantize's length, on a note deletes it, or with Double-Click Opens Note Expression Editor opens the expression editor ([note-expression.md](note-expression.md)) | pp. 1211-1214, 1220, 1308 |
| Draw | A click draws a note of the length quantize's length, a drag a multiple of it | p. 1212 |
| Erase | A click deletes the note | p. 1212 |
| Trim | A click cuts off the note's end from the mouse on, with Alt its start up to the mouse | p. 1215 |
| Split | A click splits the note at the snapped position; on a selected note, every selected note there (`note.split`: the rests keep the notes' values and their expression from the cut on; as with the notes that `event.split` cuts with `splitNotes`, both pieces lose their quantize origin, so `quantize.reset` no longer moves them) | p. 1215 |
| Glue | A click glues the note to the next one of its pitch; the first one's values stay, the next one's expression joins its own (`note.glue`) | p. 1215 |
| Mute | On the Articulations lane, a click mutes a direction or attribute, or unmutes it ([articulation-maps.md](articulation-maps.md), #1684); in the note display nothing yet, as notes have no mute (#679) | pp. 1193, 1289 |

- **Undo steps.** A gesture sends each part's `note.*` command, the commands of several parts in one
  `history.transaction` named as the engine names the lone command's step, so that the step has the same name either
  way (#2014): "Change notes" for a move or resize in the editor, transposing, trimming and the articulations, "Move
  notes" for a move onto another in-place editor, "Remove notes" for Delete, Backspace, the erase tool and the
  drumstick, "Split notes" and "Glue notes".
- **Gestures that leave the editor.** While a press in the note display or on the keyboard's left part goes on, the grid
  or the keyboard's left part is an RmlUi drag element (`.key-editor.in-gesture`): its drag events come wherever the
  mouse is, also outside the window, where the platform captures the mouse while the button is held and nothing is
  hovered, and its dragend comes where the button goes up, with the modifier keys held then. The draw tool's and the
  drumstick's drags, the expression editor's draw and erase and the drag on the keyboard's left part follow the mouse
  out there and end at the release as in the editor; the drumstick's delete drag deletes up to it. A move or resize
  released outside the editor ends without its effect, except onto another in-place editor's grid
  ([in-place-editor.md](in-place-editor.md)). A gesture whose drag never started, as when the window loses the focus
  before the mouse moved, gets no dragend: it ends without its effect as the mouse leaves the window (the document's
  mouseout; dragged out of the window, the drag events come first), so its notes don't follow the mouse back (#1756).
  One whose release never comes at all ends without its effect at the next press anywhere in the editor's window (the
  document's mousedown in the capture phase), before the press reaches its target. The class goes when the gesture ends,
  as RmlUi doesn't scroll on a touch move during a drag, and the document's computed values follow it both ways, as the
  list editor's do: the grid is a drag element only during a gesture. The display shown again while a move or resize
  goes on, as when the engine's answers to an earlier edit come in, keeps the notes where the mouse took them, also when
  the mouse holds still (#1789), and an in-place editor's range changing during it, as the project window scrolls or
  zooms, takes them to the time now under the mouse, where the release puts them (#1808). A click that selects shows its
  selection at once, and the views that come in before the engine answered it, which can be older, such as the
  editor.changed of the selection before, leave it as it is: a drag right after the click moves the notes it selected
  (#1817), and the controller lanes show that selection too, so that a lane edit acts on the same notes. Once the engine
  has answered them all, the view has the selection shown, unless the last one was refused, as when another client had
  just deleted the note: then the view's selection shows again (#1897).
- **Info line** (p. 1199). Start, end and length in bars and beats, the pitch as a name such as C3 (typing a number
  works too), velocities 0-127, the channel 1-16 and the Articulations, the attributes of the track's articulation map
  that the note has, names separated by commas ([articulation-maps.md](articulation-maps.md)). A value typed moves every
  selected note by as much as the first one's changes; a start before a note's part's start moves them only as far as
  that. The channel and the attributes are set for all.
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
the step input cursor and lengthening a held chord; the Mute tool on notes (notes have no mute yet; it mutes the
Articulations lane's articulations), the Range Selection, Zoom and Time Warp tools, and the Line tool in the note
display (it draws on the controller lanes); the selection rectangle, the trim tool's drag across several notes and the
draw tool's modifiers for velocity, pitch and position; the nudge buttons, Solo Editor, Record in Editor, pitch
visibility, the Scale Assistant, event colors, the status line and Set up Window Layout;
horizontal zoom and scrolling; the key at the mouse's pitch lit while the mouse moves in the note display (p. 1204);
drum map names in the key editor and articulation names; Add to Chord Track, Match with Chord Track, Create Chord
Events and the chord insert tools; quantizing from the toolbar (#132) and the Length section's functions (#133); the
key editor's context menu.

## Checks

- `tests/keyeditor_tests.cpp`: the selection as view state of the editable parts, refused for notes that don't exist
  or parts that aren't editable, leaving with removed notes and with closing; step input of notes and chords with
  the length quantize, the quantize grid's step, the insert velocity, a rest by moving the cursor, one undo step,
  replacing and keeping existing notes, Move Insert Mode (also by a note longer than a step) and a cursor outside the
  part, the channel of a note that stays and notes ending at the part's end; MIDI input with and without Record Pitch,
  a chord, Keep Existing Notes and no selection; the undo step's name; chord recognition, fitting and voicings;
  `note.chord` and `note.voicing` with undo and their errors; Acoustic Feedback off at first, set as view state
  without an undo step; Indicate Transpositions off at first, as view state, its steps from transpose events within
  the part (not before or after it) with the root key's step and the part's own transpose, muted transpose events and
  an Independent part.
- `tests/keyeditor_ui_tests.cpp` drives the app with `ui::Automation`: the keyboard and notes at their time and pitch,
  keys lit for selected notes; the piano keyboard at 1024x600, at dp ratios of 1.5 and 1: white keys of equal height
  across the keyboard, black keys narrower on their rows, the mouse on the right part of a white key beside a black key
  over the white key, and a golden image of two octaves with a black and a white key lit
  (`tests/golden/ui/key-editor-keyboard.png`); pressing the right part of a white key beside a black key and the black
  key playing their pitches on the track's instrument until the mouse is released, also outside the editor, leaving the
  selected notes as they were, and a held key's pitch ending when the editor switches to the drum editor, closes through
  the engine API or has its window closed, when the lower zone's Editor panel in a window of its own has that window
  closed or is docked again, and when that panel's tab is hidden or it is opened in a window of its own; the object
  selection tool selecting, deselecting, moving, resizing, inserting and deleting, and a view older than a click that
  deselects leaving the notes and the controller lanes deselected, so a drag right after it moves one note, and a press
  whose selection the engine refuses showing the view's selection again; the draw tool with Quantize Link and an eighth
  from the Length Quantize menu; a move released outside the editor ending without its effect, the note not following
  the mouse back and a later click moving nothing, and a draw released there drawing; a move and a draw whose release
  was lost ending as the mouse leaves the window, and a draw whose mouseup never came at the next press; a resize
  released there ending without its effect, its note keeping the length the mouse gives it while the display is shown
  again during it; a move's notes at the time under the mouse as the range changes during it; the erase, trim (and with
  Alt), split and glue tools, and the split tool splitting the selected notes of two parts at one position in one undo
  step; each gesture's undo step named the same over notes of one part and of two, also the drumstick's delete drag
  (`tests/drumeditor_ui_tests.cpp`); the transpose palette, Up and Down (with Shift), Delete and Backspace, and
  Transpose Up bound to another key, and typing a pitch and a start before the part's on the info line; step input with
  its cursor, a click moving it, and MIDI input selecting the next note; the Chord Editing section naming and changing a
  chord; the toolbox with the setting on and off, with and without a modifier key, choosing tools; drags on the
  keyboard's left part zooming the rows in and out between the limits, from the height a style sheet gives them, also
  after a style change at a zoom and back at 1, the keyboard as high as the rows of any height, nothing zoomed or
  scrolled without a row height to scale and no height left on the rows for a later style, the zoom snapping to 1 near
  it, and scrolling the note display, while a click there zooms nothing, and a click and a drag there leaving a note an
  earlier press selected and the history alone; the lower zone's key editor at its default size giving the note display
  more room than the controller lanes, at dp ratios 1 and 2, and the lanes whole in a lower zone of 400 dp; the editor
  window's lane keeping its 64 dp and the note display its 40%, and the in-place editor's lanes keeping their height
  with its toolbar shown; Indicate Transpositions from the toolbar showing a note after a transpose event on the row
  it plays at, a note drawn there getting the pitch that plays at that row, a move changing the pitch by the rows
  dragged, and every note on its own row again once it is off.
