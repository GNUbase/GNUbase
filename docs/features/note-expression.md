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
tool cuts notes the same way through `note.split` (#1419). Its glue tool joins a note to the next one of its pitch
through `note.glue`: the glued note keeps its own points up to where the next one started, then has the next one's,
and after a next one that ends inside it its own again, with its values there, so that it plays what the two played.
Where one note's points give way to the other's, an expression that the first one has and the other one has no point
of at its start gets its default there, as the other note started with it: MPE's initial values for tuning, pressure
and brightness, as playback gives them, unity gain for volume and the center for pan. Vibrato and expression have no
default (the plugin decides), so they keep their value. Glued again after a split, the note has an extra point at
the cut, with the value it had there. The reference manual's glue keeps the first note's properties (p. 1215), and
expression is no property but what the note plays over time.

**Recording MPE** (pp. 1295-1298). With the user setting `midi.mpeInput` on, MIDI recording and retrospective
recording take their input as MPE's lower zone: channel 1 is the master channel, whose messages record as before, and
channels 2 to 16 are the notes' own. On those, pitch bend records as the tuning of the note that sounds on the channel,
by the pitch range `midi.mpePitchRange` (48 semitones by default, MPE's), channel pressure as its pressure and CC 74
as its brightness, each a point where it came. What comes on a note's channel before its note-on is its initial state:
the channel's latest value of each of the three becomes a point at the note's start. They don't record as controller
events, also not while no note sounds on the channel; the channel's other messages do. Notes keep their channels.

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

The acceptance test records MPE through `midi.inject` and plays it back to the note probe, comparing what the
instrument gets with what was played (`tests/noteexpression_tests.cpp`); retrospective recording takes MPE input with
`midi.mpePitchRange` as recording does, and both are tested with a range of 24.

## Not yet

- The note expression editor and the expression overlay in the event display (pp. 1303-1315, #1512).
- The inspector's Note Expression section with its input assignments, MIDI learn, value ranges and bypass (pp.
  1300-1302, #1513).
- The Note Expression Input Device page with its MIDI input, initial values, Use for Tuning and channel rotation, and
  the upper MPE zone (pp. 1295-1298, #1514).
- Recording MIDI controllers as note expression, overdub and converting controller events into note expression (pp.
  1316-1318, #1515).
- A plugin's own expressions with their names and ranges (VST3 custom note expressions), CLAP and MIDI 2.0 per-note
  controllers in the adapters, MPE output that allocates channels for notes that share one and a pitch range other
  than 48, chasing expression after a jump into a note, per-note expression in DAWproject's own note content, and MIDI
  effects that move notes moving their expression (#1516).
