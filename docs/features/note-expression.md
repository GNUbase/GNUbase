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
that has any, and they are saved in the project's GNUbase entry. `note.replaceExpression` replaces only the points of
one expression in a span of each note it names, from `from` to `to` (ticks from the note's start, both included), with
the points it gives there, and keeps the note's other points as the project has them (#1795): an edit that a client
makes before it has seen its last one keeps that one. Its points end up sorted by position, a new one after the old
ones at its position; it is one undo step, and changes that leave every note's points as they were change nothing,
so the project doesn't count as modified (run on its own: in a `history.transaction` the transaction counts as a
change). The commands refuse a point more than the longest project's end after its note's start (#1831), so that the two
can't overflow together (#1906); past the note's end is fine, as a note shortened by the trim tool keeps its points
there. A project whose file has points out of range loads with them clamped, as the notes' other values, their positions
cut at the longest project's end, and without points of expressions it doesn't know.

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
before, without a point at the cut (within the rest's first sample) that only repeated the value before it (a point
elsewhere that repeats the value before it stays one). The reference manual's glue keeps the first note's properties
(p. 1215), and expression is no property but what the note plays over time.

**MPE input** (pp. 1295-1298, #1514). Each MIDI input port has its own MPE setup, `mpe` in `midi.setPort` and
`midi.ports`, kept in its entry of the user setting `midi.portSettings`, which the studio setup's Note Expression Input
page edits ([studio-setup.md](studio-setup.md)). Its multi-channel input mode (`mode`) is off (the default), MPE's lower
zone (master channel 1, the notes on channels 2 and up), its upper zone (master channel 16, the notes on 15 and down),
channel rotation (the notes on the used channels from `firstChannel` on, without a master channel) or both zones at once
(`both`, #1675), e.g. for a keyboard split into two zones that play two instruments; `channels` is how many channels the
notes have, 15 by default, and rotation's end at 16; `midi.setPort` makes it at most 15 in a zone, which has no more
member channels, so a zone set up with 16 lists and keeps 15. With both zones `channels` is the lower zone's and
`upperChannels` the upper zone's (7 by default, at most 13), together at most 14, as MPE has them: the upper zone keeps
its channels and the lower zone gets at most the rest, so a lower zone of 15 switched to both lists and keeps 7. Both
zones go past the reference manual, whose MPE mode has one zone with channel 1 as its base (p. 1298), for devices that
need them. On the notes' channels, the messages of the three dimensions of the device's input movements, Horizontal/X,
Pressure and Vertical/Y, are the expression of the notes that sound on the channel (#1674, pp. 1296-1298). Each
dimension's MIDI Message (`xMessage`, `pressureMessage`, `yMessage`) is `pitchBend`, `aftertouch` (channel pressure) or
a controller, `cc0` to `cc127`: by default pitch bend, aftertouch and CC 74. Its Note Expression Assignment
(`xExpression`, `pressureExpression`, `yExpression`) is the note expression it plays, one of the seven, MIDI
PolyPressure (`polyPressure`, p. 1299, #1829), which plays the message's value as the poly pressure of the channel's
notes, which then follows each note on the track as poly pressure does, or `none`, which passes the message as it is: by
default none for X, pressure and brightness. Tuning is by the port's pitch range (`pitchRange`, 48 semitones by default,
MPE's), from any message, a controller's center being no bend; volume goes from 0 to its gain of 4, the others from 0 to
1. Use for Tuning (`tuning`, on by default) sets X's assignment to tuning (p. 1297): X plays tuning over its own
assignment (`xExpression`), which it keeps and plays again with Use for Tuning off (for devices that don't glide but
play a new note for each key), `none` by default, so pitch bend stays pitch bend. X's own assignment is never tuning: as
on the page, `midi.setPort` with `xExpression` `tuning` turns Use for Tuning on and keeps X's own (with `tuning` false
it is rejected), and an earlier GNUbase's `tuning` in `midi.portSettings` reads as `none` (#1749). Two dimensions of one
message both play it. The master channel's messages and the notes' channels' other messages are what they were.

**Detected devices** (p. 1295). A MIDI input whose name is a known MPE device's (`midi::detectMpeDevice`: a Seaboard,
LinnStrument, Sensel Morph or Osmose) is listed with the device's name (`mpeDevice` in `midi.ports`) and gets a page of
its own in the studio setup. Until `midi.setPort` changes it, its MPE setup is the device's preset, its factory
settings: MPE's lower zone with its pitch range of 48 semitones. What a call leaves out stays as the preset has it, and
the port's entry in `midi.portSettings` keeps what differs from the preset, so it can be off as well.

**MIDI activity** (p. 1298). While `midi.activityListen` is on, which the studio setup dialog turns on while it is open
(#1749), the audio thread queues what the open inputs receive (up to 256 messages, without system real-time messages
such as clock and active sensing, 0xF8 to 0xFF) for `midi.activity`, which the engine runs itself at most ten times a
second while input arrives and a device processes blocks (the audio engine's cue thread notices it); it takes the
messages with their ports and announces them as the event `midi.activity`, which the Note Expression Input page
shows. A listener that gives `post` has each run post the notes the tracks got from all MIDI inputs and the chord pads,
how many didn't fit after them and whether a device start dropped some before them, to that command, as the key editors
do for step and MIDI input (`editor.played`, [key-editor.md](key-editor.md)). Its runs stay
out of the command log. Each `listener` (the dialog's is `studioSetup`, calls without one share one) turns
it on and off for itself, and it is on while any has it on, so the dialog closing leaves a script's or an MCP session's
on, and their off leaves the dialog's (#1832). Turning `midi.activityListen` on while no listener has it on, and
starting a device, drop what was queued before, so nothing old is announced as new; a second listener turning it on
keeps the queue, which the first one still reads. The cue thread counts the input from before the device started, so
input that arrives before it first runs is announced too (#1886); a test holds it back at its start until `midi.monitor`
has the input (#1889). Rendered offline (`audio.render`), nothing runs it: a client calls it. The user settings
`midi.mpeInput` and `midi.mpePitchRange`, which before #1514 made MIDI recording take every input as MPE's lower zone,
are gone without a migration: a port's MPE input is set up again on the Note Expression Input page.

`midi::MpeInput` does this on the audio thread, per port, before the ports' input merges into all MIDI inputs
(`MidiPorts::take`): it keeps each channel's sounding notes and latest values, without allocating, and the port's setup
reaches it as 64 bits (`midi::MpeSetup::pack`); a changed setup starts its state afresh. Expression messages become note
expression events (`graph::ProcessEvent::Type::NoteExpression`) of the channel's sounding notes; without a note they
only set the channel's state. A released note, or one the sustain pedal holds, still sounds while it fades, so the
channel's messages still apply to it, as MPE has them, until the channel's next note-on: the device gives the channel to
a new note once the old one has faded as far as it is concerned. What comes on a note's channel before its note-on is
its initial state: after the note-on follow its initial values, each the channel's latest one. With Send Initial Value
(`initialX` for the pitch bend, `initialY` for CC 74) a note starts at a fixed value instead, for devices whose input is
relative: the center (no bend, or CC 74 at 64), or for Y the minimum or maximum, whatever the channel's previous note
left, each as the dimension's message would carry it. The fixed value is the channel's from then on, so the channel's
other sounding notes get it too.

So live MPE input reaches the instrument as per-note expression, as playback sends it (below): MIDI thru passes the
events to the tracks that monitor or are record-enabled, and the instrument plays them as note expression, a VST3 plugin
as VST3 note expressions; the VST3 adapter keeps a released note's id for its expression until its key plays again.
The input transformer's modules and the transformer insert (`processors::LogicalStage`) send a note's expression where
its note-on went: transposed, on another channel, to both notes where they insert one, and nowhere where they filter
the note out; the expression of a key whose note-on came before the rules plays as it came. On the track a note's
expression follows the note its key plays, as poly pressure does (`MidiSequencer::modifyInput`): to the track's MIDI
channel and the pitch the MIDI modifiers, Live Input, a drum map or a sound slot make of it, also while the note fades
after its note-off, until the next note-on on its channel, or until another key's note-on starts its note again (a
track with a MIDI channel, Live Input or a drum map that puts keys of several channels on one note): its release has
ended then, so its expression doesn't change the new note (#1685). The input transformer forgets such a released note
the same way. Where the note doesn't play (a muted drum sound, a velocity
or pitch the MIDI modifiers filter out), and for a key whose note-on didn't reach the track (a sound slot's remote
trigger key, or a key held before the track monitored or was record-enabled), its expression and poly pressure go
nowhere. Recording, retrospective recording and the catch range record what the tracks get: each note expression
event is a point of its note where it came (`MidiTakeBuilder`), its initial values at the note's start; what comes
after the note-off isn't recorded, as playback sends only the points within a note. Notes keep their channels. The MIDI
record filter filters note expression by its channel only, as it is no MIDI message. The on-screen keyboard is no port
and plays plain MIDI; `midi.inject` does too, unless its `mpe` gives it an MPE setup to play through, as a port with it
would, so MCP and Lua reach MPE input without a MIDI port. The track keeps that setup's state of the channels from one
call to the next, and a message waits for the next block unless the track's input has room for all it adds, a point
for each note of its channel (`MpeInput::maxEvents`, #1685). `direct` and `mpe` together are refused: `direct`
skips the track's input, where the setup plays (#1819).

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

The acceptance test plays MPE into a dummy MIDI port set to MPE's lower zone, records it and plays it back to the note
probe, comparing what the instrument gets with what was played (`tests/mpeinput_tests.cpp`). Its other tests check
`midi::MpeInput` with the lower and upper zone, both zones at once, channel rotation, Send Initial Value and without Use
for Tuning, the dimensions' MIDI Messages and Note Expression Assignments (tuning from a controller, volume, pan, none,
MIDI PolyPressure, two dimensions of one message) and the room a message needs; the setup's JSON and its 64 bits, also
with both zones and MIDI PolyPressure; a detected LinnStrument's input listed with its preset, which plays, and its
settings kept against the preset; `midi.activity` taking each port's messages once while `midi.activityListen` is on,
without system real-time messages, announcing them and staying out of the log; with a running dummy device, the engine
announcing input as `midi.activity` on its own, dropping what came before the device started and announcing nothing
while nobody listens; that `midi.setPort` sets a port's setup, which `midi.ports` lists and `midi.portSettings` keeps
(whose schema lists every `mpe` member), with X's own assignment kept under Use for Tuning and Tuning for X turning it
on; that an MPE port with a pitch range of 24 plays to the instrument as note expression while another port plays its
pitch bend as it is; that `midi.setPort` sets both zones, whose notes play to the instrument as note expression while
the masters' messages play as they are; that a port whose pressure is MIDI PolyPressure plays to the instrument as the
notes' poly pressure; that the expression follows its note through a track that transposes and a MIDI track with a MIDI
channel, also after its note-off, also on the track's MIDI channel, and is dropped for a key whose note is filtered by
velocity, a muted drum sound, a remote trigger key and a key held before the track got the input, while another
channel's note that plays gets its own; that a released key's pressure reaches nothing once another key started its note
again; that `midi.inject` waits for room for a message's points to 120 notes; that a changed setup starts a port's state
afresh while the note that sounds still ends; that `midi.inject` plays through an MPE setup; and that recording and
retrospective recording record the upper zone and channel rotation as the notes' expression. `tests/logical_tests.cpp`
checks the expression through the input transformer, also that it drops a released key's expression once another key
starts its note, and `tests/vst3_adapter_tests.cpp` a released note's expression in the VST3 adapter.

## In the key editor

The key editor (#1512, pp. 1303, 1308-1311, [key-editor.md](key-editor.md)) shows and edits the notes' expression
through `note.replaceExpression` only. Its view has three settings, which `editor.set` changes and `editor.get` reports:
`showExpression` (Show Note Expression Data), `expressionDoubleClick` (Double-Click Opens Note Expression Editor), both
off by default and toggled by toolbar buttons, and `expressionType`, the expression the editor edits, `tuning` at
first.

- **Overlay** (p. 1303). With Show Note Expression Data each point of a note is a line over the note box, from the point
  to the type's next one (or the note's end) at its value, as playback holds a value until the next point. The values go
  from the box's bottom to its top: tuning from -48 to 48 semitones, MPE's pitch range (values beyond show at the edge),
  volume from 0 to 4 and the others from 0 to 1. Expressions that the note's track hides (#2120,
  [note-expression-setup.md](note-expression-setup.md)) have no lines. A step narrower than a pixel of the note display
  joins the next ones until it is as wide, at the last one's value, so that a recorded note with a point every few
  milliseconds is a line per pixel at most, not hundreds of elements (#1671). The lines are built again when a pixel has
  a quarter more or fewer ticks than they were built with: after an editor's first layout, as the editor window or the
  lower zone resizes and as the project window zooms, but not on every frame of a scroll across a tempo change, which
  changes the in-place editor's ticks per pixel a little (#1716). The quarter counts once the window's update has laid
  the lines out with the ticks they were built for; before that, as when the lines first show, they are built again for
  any other ticks. They are built for the box the last layout gave the notes, which need not be the window's: the editor
  window's first layout, before the left zone has its size, gives the notes about a tenth more room (#1768). Each
  editor's lines follow the update of its own window. The drum editor shows no overlay, as it has neither the buttons
  nor the expression editor, also with Show Note Length.
- **Expression editor** (pp. 1308-1311). With Double-Click Opens Note Expression Editor a double click on a note with
  the object selection tool opens the editor instead of deleting the note: on the note, or on the selected notes if it
  is one of them. It lies over the note display from the first selected note's start to the last one's end, centered on
  the row of the note double-clicked and kept inside the note display at the top and bottom rows, never narrower than
  its button, and it stays there as the pitches zoom; it shows their points of the chosen expression the same way. On
  notes narrower than its button it spreads their time over its whole width, so that its lines no longer lie over the
  notes' time behind it and join their steps by the note display's coarser pixels, and only where it would end past
  the note display's right edge it moves left to stay inside (#1713), so that an in-place editor's keeps still as the
  project window scrolls (#1746); a window or zone that changes its width places it again. Its button at the top left
  is Parameter Selection, a menu of the expressions that the selected notes have points of or their tracks' note
  expression setups show (p. 1309, [note-expression-setup.md](note-expression-setup.md)), all seven by default. In it
  the draw tool draws a point where it clicks, and a drag draws points along the way where the value changes (not one
  per mouse position), replacing those of the expression in the range it went over; at the right edge it draws at the
  last note's last tick. With Shift, tuning goes in semitone steps (Vertical Snap). The erase tool deletes the points
  of the expression it is dragged over, a click those within a few dp: it is GNUbase's own, standing in until points can
  be selected and deleted with Edit > Delete as the reference manual does (pp. 1310-1311). An edit reaches every
  selected note at the time where it happens, each note's points from its own start, and is one undo step, named "Change
  note expression" as the engine names a lone `note.replaceExpression`, also when it changes notes of several parts
  (#1831). It sends each note the gesture reached only the range it swept and the points it drew there, not the note's
  whole list: the editor reads the whole project after every change and shows the reads in order, so an edit made before
  the read of the one before it arrived starts from the project as it was before that one, and the whole list would send
  that one's points back (#1795). While the project shown may lack an edit of the editor's (one is out, or the last one
  that changed points was answered after the read of the project shown), a note that the gesture reached goes also when
  the editor's copy has no change there, as the edit before may have drawn a point there. Otherwise an edit that changes
  nothing stays in the editor, so that it doesn't count as a change of the project, as a `history.transaction` of
  several parts would. An edit that changed nothing brings no read, and the project shown lacks nothing of it, so it
  doesn't keep the editor behind (#1831). A gesture follows the mouse outside the window and ends where the button
  goes up, with what it drew. A press elsewhere in the note display closes the editor, and so does a selection without
  notes. The `.key-expression-step` lines, in the editor and over the notes, take no
  mouse events: a client of the UI tools that clicks one reaches its note or `.key-expression-area`.

The UI tests (`tests/keyeditor_ui_tests.cpp`) check the overlay's lines at their positions and values, volume's range
and tuning beyond 48 semitones at the edge, the steps narrower than a pixel joined, opening the editor with a double
click, Parameter Selection, drawing a point and a drag of points with their values, undo, the erase tool deleting only
the chosen expression's points and a click deleting one, closing it with a click, and that without the setting a double
click deletes the note. With several selected notes of two parts they check the double click opening the editor on all
of them over their span, centered on the note clicked, an edit reaching each note at its time, a drag leaving a point
where the value changes, one undo step for both parts, Vertical Snap and the editor closing when the selection empties;
the editor centered on its note while the pitches zoom; on the top row the editor inside the note display and wider than
its button, and a draw released outside the window; on a short note right of the note display's middle the editor
starting at the note; a dense note's lines a pixel each after the window's layout and a resize, a resize by less than a
quarter keeping them, and lines that first show after the window narrowed by less than a quarter, also on the box of the
layout before, built for the narrower width. `tests/inplaceeditor_ui_tests.cpp` checks the draw tool on a line of the
editor in an in-place editor, an erase over nothing in two parts of its track leaving the project unmodified after a
draw (the in-place editor hands its key editor the read it shows, #1831), and a dense note's lines a pixel each after
its layout, kept as the project window zooms by a tenth once the window has laid them out (the app's update task fits
the in-place editors' lines, #1782) and built again as it zooms in a step, `tests/drumeditor_ui_tests.cpp` that the drum
editor has neither the buttons, the overlay nor the editor, and `tests/keyeditor_tests.cpp` the three settings as view
state. `tests/noteexpression_ui_tests.cpp` makes four edits on notes of two parts before the UI hands on any answer of
the engine, an erase of a point, two draws and an erase of the last point drawn, and checks that the engine keeps all of
them, as four undo steps of one name, and that an erase where there is nothing to erase then leaves the project
unmodified; that an erase made after a draw was answered, while the editor still shows the project read before it,
deletes the point drawn (#1795); and that an erase that changed nothing, answered after the read shown, doesn't keep the
editor behind (#1831). `tests/noteexpression_tests.cpp` checks `note.replaceExpression`: the span with both ends, the
other expressions and the points outside it kept, the order, edits made from the same state all kept, a note named
twice, one undo step, a change of nothing leaving the project unmodified, its refusals changing nothing and the shared
copies following it; and the bound of point positions in `note.add`, `note.set` and `note.replaceExpression`, and
loading cutting them there.

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
- What the inspector's Note Expression section doesn't have yet: [note-expression-setup.md](note-expression-setup.md).
- The Note Expression Assignment's Preferred and Alternative rules (Expression Name Contains, Note Expression Number, As
  Recommended by Instrument, p. 1299), and renewing the assignments when the instrument or its preset changes (p. 1298,
  #1829): they choose among a plugin's own expressions and need to know which expressions an instrument has, which need
  #1516; with the seven common ones and MIDI PolyPressure the assignment is what the page sets. More known MPE
  devices, which are detected by their port's name only.
- Overdubbing note expression and the release phases of Convert to Note Expression (pp. 1316-1318, #1515; recording
  the assigned controllers, `noteExpression.convert` and its MIDI menu item are in
  [note-expression-setup.md](note-expression-setup.md)), with the toolbar switch that records expression for a selected
  note from MIDI input (pp. 1303, 1308) and the dialog that sets which MIDI messages record as expression (pp.
  1314-1315).
- A plugin's own expressions with their names and ranges (VST3 custom note expressions), CLAP and MIDI 2.0 per-note
  controllers in the adapters, MPE output that allocates channels for notes that share one and a pitch range other
  than 48, chasing expression after a jump into a note, per-note expression in DAWproject's own note content, and the
  arpeggiator and chord generator moving the expression of the notes they play (#1516).
