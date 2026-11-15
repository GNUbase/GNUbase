# Note expression

Per-note expression (#176, reference manual pp. 1293-1318): a note carries its expression, such as its tuning,
pressure and brightness over time, as one unit with it. MPE input records into it, and playback sends it to the
instrument with the note.

## In the engine

**Expression points.** A note's `expressions` are points `{type, position, value}`, sorted by position: `position` is
in ticks from the note's start, so the points move, quantize, copy and delete with the note. `type` is one of the
plugin API's common expressions (`plugin::NoteExpression`, [plugin-api.md](../plugin-api.md)), each in its unit:

| Type | Value |
|---|---|
| `volume` | A linear gain, 0-4 |
| `pan` | 0 left, 0.5 center, 1 right |
| `tuning` | Semitones, -120 to 120 |
| `vibrato`, `expression`, `brightness`, `pressure` | 0-1 |

`note.add` and `note.set` take them (`note.set` replaces a note's points), `project.get` lists them with each note
that has any, and they are saved in the project's GNUbase entry. A project whose file has points out of range loads
with them clamped, as the notes' other values, and without points of expressions it doesn't know.

**Cutting and resizing.** Splitting a part with `splitNotes` (`event.split`) leaves the first piece of a cut note its
points before the cut, and the rest gets the points from the cut on, moved back by the cut, with each expression's
value at the cut as its initial point. A note whose start `note.set` moves while its end stays (its start resized) keeps
its expression where it is the same way: moved later, the values at the new start become its initial points; moved
earlier, its initial values start at the new start and its other points stay where they were. The key editor's split
tool cuts notes the same way through `note.split` (#1419); a position that cuts none of them changes nothing, and the
project doesn't count as modified. Its glue tool joins a note to the next one of its pitch through `note.glue`: the
glued note keeps its own points up to where the next one started, then has the next one's up to where it ended, and
after a next one that ends inside it its own again, with its values there, so that it plays what the two played (the
points a note has at or past its end, e.g. after the trim tool shortened it, never played). Where one note's points
give way to the other's, an expression that the first one has and the other one has no point of at its start (within
its first sample, as playback takes a note's initial points) gets its default there, as the other note started with it:
MPE's initial values for tuning, pressure and brightness, as playback gives them (one definition,
`defaultExpression`, serves both), unity gain for volume and the center for pan. Vibrato and expression have no default
(the plugin decides), so they keep their value. A point there whose value already plays is left out, so that the
expression editor shows no point that changes nothing: glued again after a split, the note has the expression it had
before, without the points in it that changed nothing (one that repeated the value before it exactly at the cut doesn't
come back). The reference manual's glue keeps the first note's properties (p. 1215), and expression is no property but
what the note plays over time.

**MPE input** (pp. 1295-1298, #1514). Each MIDI input port has its own MPE setup, `mpe` in `midi.setPort` and
`midi.ports`, kept in its entry of the user setting `midi.portSettings`, which the studio setup's Note Expression Input
page edits ([studio-setup.md](studio-setup.md)). Its multi-channel input mode (`mode`) is off (the default), MPE's
lower zone (master channel 1, the notes on channels 2 and up), its upper zone (master channel 16, the notes on 15 and
down) or channel rotation (the notes on the used channels from `firstChannel` on, without a master channel);
`channels` is how many channels the notes have, 15 by default and at most 15 in a zone, and rotation's end at 16. On
the notes' channels, pitch bend is the tuning of the notes that sound on the channel, by the port's pitch range
(`pitchRange`, 48 semitones by default, MPE's), channel pressure their pressure and CC 74 their brightness. Without
Use for Tuning (`tuning` off, for devices that don't glide but play a new note for each key) pitch bend stays pitch
bend. The master channel's messages and the notes' channels' other messages are what they were.

`midi::MpeInput` does this on the audio thread, per port, before the ports' input merges into all MIDI inputs
(`MidiPorts::take`): it keeps each channel's sounding notes and latest values, without allocating, and the port's
setup reaches it as 32 bits (`midi::MpeSetup::pack`); a changed setup starts its state afresh. Expression messages
become note expression events (`graph::ProcessEvent::Type::NoteExpression`) of the channel's sounding notes; without a
note they only set the channel's state. What comes on a note's channel before its note-on is its initial state: after
the note-on follow its initial values, each the channel's latest one. With Send Initial Value (`initialX` for the
pitch bend, `initialY` for CC 74) a note starts at a fixed value instead, for devices whose input is relative: the
center (no bend, or CC 74 at 64), or for CC 74 the minimum or maximum, whatever the channel's previous note left.

So live MPE input reaches the instrument as per-note expression, as playback sends it (below): MIDI thru passes the
events to the tracks that monitor or are record-enabled, and the instrument plays them as note expression, a VST3 plugin
as VST3 note expressions. On the track a note's expression follows the note its key plays, as poly pressure does
(`MidiSequencer::modifyInput`): to the track's MIDI channel and the pitch the MIDI modifiers, Live Input, a drum map or
a sound slot make of it, and nowhere where the note doesn't play. Recording, retrospective recording and the catch range
record what the tracks get: each note expression event is a point of its note where it came (`MidiTakeBuilder`), its
initial values at the note's start. Notes keep their channels. The MIDI record filter filters note expression by its
channel only, as it is no MIDI message. `midi.inject` and the on-screen keyboard are no port and play plain MIDI.

**Playback.** The sequencer sends a note's points that lie within it as note expression events
(`graph::ProcessEvent::Type::NoteExpression`, with the note's pitch and channel) where they are, after the note-on at
the same sample. Plugin slots pass them on as the plugin API's `NoteExpression` events, which the VST3 adapter plays
as VST3 note expressions. As MIDI 1.0, what `midi.capture` shows, they are MPE on the note's channel: tuning as pitch
bend with MPE's pitch range of 48 semitones, pressure as channel pressure and brightness as CC 74; the other types have
no MIDI 1.0 message. As MPE senders do, a note's initial pitch bend, pressure and CC 74, its points at its start, come
before its note-on, and of the three that the notes of its channel express, a note starts at MPE's initial value of
each it has no point of at its start (a centered bend, no pressure, CC 74 at 64), so that it doesn't start at what
the channel's previous note left; plugins get these initial values too. What the channel's notes don't express it
doesn't get: a channel whose notes bend and brighten but never press sends no channel pressure. The sequencer's
snapshot marks which of the three each note has a point of at its start, so this costs no lookup per point. So a
recording of an MPE controller plays back to an MPE instrument as it was played. MIDI sends before the modifiers get
the notes' expression too.

The acceptance test plays MPE into a dummy MIDI port set to MPE's lower zone, records it and plays it back to the
note probe, comparing what the instrument gets with what was played (`tests/mpeinput_tests.cpp`). Its other tests check
`midi::MpeInput` with the lower and upper zone, channel rotation, Send Initial Value and without Use for Tuning; that
`midi.setPort` sets a port's setup, which `midi.ports` lists and `midi.portSettings` keeps; that an MPE port with a
pitch range of 24 plays to the instrument as note expression while another port plays its pitch bend as it is; that
the expression follows its note through a track that transposes and a MIDI track with a MIDI channel; and
that recording and retrospective recording record the upper zone and channel rotation as the notes' expression.

## In the key editor

The key editor (#1512, pp. 1303, 1308-1311, [key-editor.md](key-editor.md)) shows and edits the notes' expression
through `note.set` only. Its view has three settings, which `editor.set` changes and `editor.get` reports:
`showExpression` (Show Note Expression Data), `expressionDoubleClick` (Double-Click Opens Note Expression Editor), both
off by default and toggled by toolbar buttons, and `expressionType`, the expression the editor edits, `tuning` at
first.

- **Overlay** (p. 1303). With Show Note Expression Data each point of a note is a line over the note box, from the
  point to the type's next one (or the note's end) at its value, as playback holds a value until the next point. The
  values go from the box's bottom to its top: tuning from -48 to 48 semitones, MPE's pitch range (values beyond show
  at the edge), volume from 0 to 4 and the others from 0 to 1. A step narrower than a pixel of the note display joins
  the next ones until it is as wide, at the last one's value, so that a recorded note with a point every few
  milliseconds is a line per pixel at most, not hundreds of elements (#1671). The drum editor shows no overlay, as it
  has neither the buttons nor the expression editor, also with Show Note Length.
- **Expression editor** (pp. 1308-1311). With Double-Click Opens Note Expression Editor a double click on a note with
  the object selection tool opens the editor instead of deleting the note: on the note, or on the selected notes if it
  is one of them. It lies over the note display from the first selected note's start to the last one's end, centered
  on the row of the note double-clicked and kept inside the note display at the top and bottom rows, never narrower
  than its button, and shows their points of the chosen expression the same way. Its button at the top left is
  Parameter Selection, a menu of the seven expressions. In it the draw tool draws a point where it clicks, and a drag
  draws points along the way where the value changes (not one per mouse position), replacing those of the expression
  in the range it went over; at the right edge it draws at the last note's last tick. With Shift, tuning goes in
  semitone steps (Vertical Snap). The erase tool deletes the points of the expression it is dragged over, a click those
  within a few dp: it is GNUbase's own, standing in until points can be selected and deleted with Edit > Delete as the
  reference manual does (pp. 1310-1311). An edit reaches every selected note at the time where it happens, each note's
  points from its own start, and is one undo step. A gesture released outside the window ends when the mouse leaves
  it, with what it drew. A press elsewhere in the note display closes the editor, and so does a selection without
  notes. The `.key-expression-step` lines, in the editor and over the notes, take no mouse events: a client of the UI
  tools that clicks one reaches its note or `.key-expression-area`.

The UI tests (`tests/keyeditor_ui_tests.cpp`) check the overlay's lines at their positions and values, volume's range
and tuning beyond 48 semitones at the edge, the steps narrower than a pixel joined, opening the editor with a double
click, Parameter Selection, drawing a point and a drag of points with their values, undo, the erase tool deleting only
the chosen expression's points and a click deleting one, closing it with a click, and that without the setting a
double click deletes the note. With several selected notes of two parts they check the double click opening the
editor on all of them over their span, centered on the note clicked, an edit reaching each note at its time, a drag
leaving a point where the value changes, one undo step for both parts, Vertical Snap and the editor closing when the
selection empties; on the top row the editor inside the note display and wider than its button, and a draw released
outside the window. `tests/inplaceeditor_ui_tests.cpp` checks the draw tool on a line of the editor in an in-place
editor, `tests/drumeditor_ui_tests.cpp` that the drum editor has neither the buttons, the overlay nor the editor, and
`tests/keyeditor_tests.cpp` the three settings as view state.

## Not yet

- In the expression editor (pp. 1309-1313, #1655 until they get tickets of their own): One-Shot Mode, the Horizontal
  Snap button, Change Editor Size, the value at the mouse (Parameter Range), the smart controls on its borders (tilt,
  compress, scale, move, stretch), the Line tool, selecting points with a rectangle, deleting, moving, copying, pasting
  (also to other notes and to another expression) and repeating them, choosing the expression by a click on its curve,
  which needs the other expressions' curves in the editor and a hit test by position (p. 1311), the Left and Right
  Arrow and Tab keys to step through the notes, the release phase (Change Release Length) and a key command for
  Double-Click Opens Note Expression Editor (p. 1308); the overlay's display size slider and Acoustic Feedback (p.
  1303), a color per expression in the overlay, which draws all of them alike, and Trim Note Expression to Note Length
  and Remove Note Expression in the MIDI menu (p. 1314).
- Parameter Selection lists the expressions the note uses and those chosen in the inspector, not all seven (p. 1309,
  #1513).
- The inspector's Note Expression section with its input assignments, MIDI learn, value ranges and bypass (pp.
  1300-1302, #1513).
- The MPE input's MIDI Message assignments of pressure, X and Y, its VST Note Expression Assignments (p. 1298), the
  pages of devices that are detected and set up on their own (p. 1295) and the MIDI activity display.
- Recording MIDI controllers as note expression, overdub and converting controller events into note expression (pp.
  1316-1318, #1515), with the toolbar switch that records expression for a selected note from MIDI input (pp. 1303,
  1308) and the dialog that sets which MIDI messages record as expression (pp. 1314-1315).
- A plugin's own expressions with their names and ranges (VST3 custom note expressions), CLAP and MIDI 2.0 per-note
  controllers in the adapters, MPE output that allocates channels for notes that share one and a pitch range other
  than 48, chasing expression after a jump into a note, per-note expression in DAWproject's own note content, and MIDI
  effects that move notes moving their expression (#1516).
