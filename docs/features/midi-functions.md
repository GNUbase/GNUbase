# MIDI functions

Functions that edit MIDI parts as a whole (#133, #838).

- **MIDI functions** (`src/engine/midi/functions.h`, #133) edit parts as a whole: `midi.transpose` (semitones, scale correction from one scale to another, keeping notes in a range), `midi.legato`, `midi.fixLengths`, `midi.fixVelocities`, `midi.velocity` (add, compress or expand, limit, fixed, variance; reference manual pp. 1126-1127), `midi.pedalToNoteLength`, `midi.deleteOverlaps` (mono and poly), `midi.deleteDoubles`, `midi.deleteControllers`, `midi.deleteContinuousControllers`, `midi.deleteNotes`, `midi.restrictPolyphony`, `midi.thinOut`, `midi.reverse` and `midi.invert`. Each takes `parts` and, where notes matter, the selected `notes` (all of them if left out), since the selection is not in the document. Notes on different MIDI channels are separate voices: they never shorten, delete or limit each other. Variance lowers each note by a random part that comes from the ids of its part and itself, so it is the same every time. The functions are plain functions on the document; `functioncommands.cpp` registers them through `ProjectModel::addEdit`, so each is one undo step and can be part of a transaction.
- **Functions that make parts.** `midi.merge` copies what starts between two positions (in samples, like the locators)
  on some tracks into a new part, leaving out muted parts (p. 1119). Without `tracks` it takes the MIDI and instrument
  tracks that play, which `playingTracks` finds by playback's rules: it compiles a render snapshot, and a track plays if
  neither its channel nor a channel its output leads into is silent there (muted, silenced by another channel's solo, or
  on a disabled track); sends aren't followed, so a track heard only through a send doesn't count. With none, it
  refuses. The merged notes get their parts' transpose and velocity, which the new part doesn't have, as playback
  applies them, and with `includeInserts` (Include Inserts, pp. 1096 and 1120) also their tracks' MIDI modifiers
  ([midi-modifiers.md](midi-modifiers.md)), with the random values playback draws for them; notes the modifiers filter
  out or move out of the range are left out. Merged onto a track whose modifiers are on, they play twice: turn them off
  or merge onto another track. Merged notes are written as their tracks' drum maps and the chord track play them:
  playback transposes a note after its track's drum map has played its O-note and after the chord track has mapped its
  pitch, so the notes get the O-notes and channels of their sounds (those of muted sounds are left out) and the chord
  track's pitches, which a new track, without either, plays the same. Notes and controllers of a MIDI track set to a
  MIDI channel are written on that channel, as it played them (a drum sound's own channel still wins), so they play
  there on a track set to Any or to that channel ([mixer.md](mixer.md)). A destination set to a channel plays every
  channel on its own, so there they keep their own channels, as a destination's drum map keeps a note's own pitch: a
  track merged onto itself keeps the channels its events were recorded with, and set back to Any plays them so
  (pp. 1120-1121). A destination with a drum map or following the chord track gets a pitch
  that plays them (a note's own, if it does, so that a track merged onto itself keeps its
  pitches), or if none does what played, which it then maps. The new part gets the root key and Global Transpose setting
  ([transpose.md](transpose.md)) the parts its notes come from share, or, when they differ, the project root key and
  follow (#954). Each note is written as it played, with its own part's global transpose at its position, which playback
  adds before the track's MIDI modifiers, so that `includeInserts` applies their Note Limit and Note Filter ranges to
  the pitch that plays; the new part's global transpose where the note lands then comes off it, so that it plays as
  before under the project root key and the transpose track. Without `includeInserts` the track's modifiers stay on the
  track and play after the stored pitch, so nothing lies between the two global transposes: only their difference goes
  into the note, with the part's transpose and velocity. What the notes can't hold stays on a new part of its own, for
  each source part on its own, as bounce does (#1240, #1253). A pitch that plays and that this takes beyond 0-127: with
  `includeInserts`, whose note ranges lie between the part's transpose and the new part's global transpose, in an
  Independent part as it played; without, in a part with its part's transpose and root key and Global Transpose setting,
  where the destination's MIDI modifiers bring it back as the source track's did (one they don't bring back plays
  nowhere and is left out). The part's velocity: where a note has velocity variance, which playback applies before the
  part's velocity, and without `includeInserts` where the destination's modifiers compress the velocity, or bring back
  one beyond 1-127, before playback adds the part's; with `includeInserts` such a note's variance then applies to the
  velocity the track's modifiers made, and the part's velocity stays on the new part only where the track's modifiers
  play every note of the part alike with it added after them (their compression, Velocity Limit and Velocity Filter and
  the clamp to 1-127 come before it in playback), else it goes into the notes. The first new part, with the controllers,
  is on the first lane: the one with the setting the parts share, else the one of the earliest source part. The others,
  in the order of their earliest source parts as bounce orders its parts, with the articulation events only (playback
  resolves directions per part and never sends them), are on new lanes below the destination's parts. Playback sends a
  sound slot's output events across a track's parts ([articulation-maps.md](articulation-maps.md)), so the new parts
  play the slots the source parts did. A part of its own that no note goes into (all of them left out, or playing
  nowhere) is left out (#1491). With `eraseDestination`, a shared copy that loses events leaves its copies, which keep
  theirs; its events get new ids, so the play probability, velocity variance and random MIDI modifier values of the
  notes that stay draw anew. `midi.dissolve` copies events by channel or pitch into new parts at the same place and
  mutes the parts (pp. 1121-1122): `to: tracks` puts them on new MIDI tracks routed to the same instrument, `to: lanes`
  on new lanes of the track below the lanes its parts are on (`optimize` only works with tracks). Events of no channel
  or pitch go with the first new part of their part, so none is lost; new parts keep their part's mute, transpose,
  velocity, root key and Global Transpose setting, and new tracks get the track's MIDI modifiers and are set to the MIDI
  channel they hold by channel (p. 1122), or to the track's (`channel.set` `midiChannel`) by pitch, so that they play as
  the part did. The copied events get new ids, so play probability and velocity variance, drawn from part and note ids,
  draw anew. `midi.bounce` combines a track's parts into one on the lowest of their lanes, removes muted parts and
  writes the parts' transpose and velocity into the notes (p. 1123), as playback applies them. The new part gets the
  root key and Global Transpose setting the parts with notes share, or, when they differ, the project root key and
  follow, as merge does (#954, #1240), and each part's difference in global transpose goes into its notes with its
  transpose, so that it keeps the pitch it played with. Playback checks the range after the track's MIDI modifiers'
  transpose and random pitch, so a pitch that the part's transpose takes beyond 0-127 and the track's brings back plays;
  a note that plays nowhere, also so, stays, transposed, where the track's drum map or chord track following make of
  that what they make of the note, so that it comes back alike when the modifiers change; where they don't, its part
  keeps its transpose (#1240), and a pitch beyond 0-127 is left out. What the notes can't hold stays on a part, for each
  part on its own. Its transpose: where a pitch that plays would go beyond 0-127, or where the track's drum map or chord
  track following, which map a note before its part's transpose and the global transposition, would play another pitch,
  asked at each note's position; such a part keeps its root key and Global Transpose setting, with its transpose in the
  notes if they hold that alone, else on the part. Its velocity: where the track's MIDI modifiers, which compress a
  note's velocity before playback adds the part's, or a velocity beyond 1-127 that their shift or random velocity brings
  back, would play another velocity (checked for every random value), and where a note has velocity variance, which
  playback applies before the part's velocity. Parts with the same settings make one new part; a new part that overlaps
  another one, or a part of the track that stays, on its lane moves to the next lane free of them. So everything plays
  as before. The track's MIDI modifiers stay on the track, and the copied notes get new ids, so play probability and
  velocity variance, drawn from part and note ids, draw anew.
- **Extract MIDI Automation** (`midi.extractAutomation`, p. 1129) moves the continuous controllers of parts into controller automation lanes of their tracks (`automationLane.*`, [automation.md](automation.md)), one per controller and channel: a step point where each event plays, its value scaled to 0-1. An existing lane loses its points in the part's range first, except those an earlier part of the same call added, so overlapping parts keep each other's, and reads from then on, so that what was moved plays. The points play at the part's place only, so a shared copy leaves its copies, which keep their controllers; all its events get new ids, so the play probability, velocity variance and random MIDI modifier values of its notes, drawn from part and note ids, draw anew. Pitch bend, pressure, switches such as the sustain pedal, events past a part's end and muted parts stay, as on p. 1129.
- **The MIDI menu** (`resources/ui/project.rml`, `src/app/appmidifunctions.cpp`, reference manual pp. 1118-1131) runs them in the app. As in the reference, they work on what the Edit menu's quantize commands take ([quantize.md](quantize.md)): in an editor, its selected notes, or every note it edits without a selection; from no zone (Lua, MCP, the menu in the project window), an open editor's selected notes first, else the project window's selected MIDI parts. MIDI > Functions: Legato (`midi.functionsLegato`) extends by the user setting `editing.legatoOverlap` (Legato Overlap, in ticks of 480 to a quarter note, negative for a gap) and, with `editing.legatoBetweenSelected` (Legato Mode: Between Selected Notes Only, p. 1599), only to the next selected note; Fixed Lengths sets the editor's length quantize, or the quantize grid's step with its tuplet when that is linked to it, as in the project window and as Edit > Quantize Lengths takes it (swing and grooves, which change from step to step, don't count); Fixed Velocity the editor's insert velocity (100 in the project window); Pedals to Note Length, Delete Overlaps (mono) and (poly), Delete Doubles, Delete Controllers, Delete Continuous Controllers, Thin Out Data, Extract MIDI Automation, Reverse and Mirror (`midi.invert`) run with no settings; Velocity... (Type with Amount, Ratio in percent, Lower and Upper, or Variance in percent; velocities in MIDI 1.0 values, which it converts as playback does (`velocityFromMidi1`), so 64 less 1 is 63; out-of-range values are refused in the dialog's units), Delete Notes... (Minimum Length in ticks of 480 to a quarter note, Minimum Velocity, Remove when under with both; p. 1140) and Restrict Polyphony... ask first. Transpose Setup... asks for the semitones, Scale Correction from a current to a new scale and Keep Notes in Range with the highest and lowest note. Merge MIDI in Loop... (MIDI Merge Options: Include Inserts, on by default, Erase Destination, Include Chase) merges between the locators the tracks that play onto the first selected MIDI or instrument track, or a new one. Dissolve Part... separates the project window's selected parts by channel or by pitch, onto tracks with Optimized Display or onto lanes; Bounce MIDI bounces the selected parts of each track, one undo step for several tracks. The dialogs are one document, `resources/ui/midi-function.rml`, whose Lua runs the engine's command and shows its error if it refuses, and it enables the fields the chosen settings use.
- **One dialog at a time.** Dialogs whose Lua keeps its state in a global table open with `AppUi::openOnce`, which takes the document's attributes: while one is open, opening it again, from another window or through MCP, brings it to the front as it is. The MIDI menu's dialogs, Add Track and Auto Fades work this way; the fade and crossfade editors already open one at a time through `EventDisplay::openFadeDialog`.
- **Not done yet:** an articulation map's transpose, velocity, channel and output events in merged and bounced notes; playing MIDI effects into the merged part (#954): tracks have MIDI insert and send effects ([midi-effects.md](midi-effects.md)), and on p. 1120 Include Inserts applies the insert effects as well as the MIDI modifiers and Include Sends the send effects, but here Include Inserts applies the MIDI modifiers only, and the dialog has no Include Sends or Convert VST3. The effects run in time, so this needs an offline run over the range that gives the same result every time, on the sends' routing and `pre` as they settle in #992. Chase options beyond controllers; note names in Transpose Setup's range fields.

## Checks

`tests/midifunctions_tests.cpp` has a test for each function, including `playingTracks` with solo, mute, a muted group
channel and a disabled track, merge with and without `includeInserts`, merge writing the channels a MIDI track set to a
channel plays on (a drum sound's own one winning), dissolve's new tracks with the track's MIDI modifiers and set to
their channel or the track's, Extract MIDI Automation on a shared copy, Bounce MIDI of shared copies, Merge with Erase
Destination on a
shared copy, and the pitches that play before and after a bounce or a merge on tracks with a drum map or following the
chord track: bounce across a chord change, a pitch the transpose takes beyond 0-127 whose sound plays, notes that play
nowhere, a merge onto another track that follows the chord track through the nearest pitch it maps to what played or,
for a pitch it maps nothing to, the pitch itself. Bounce is also compared before and after with the track's MIDI
modifiers' transpose, their velocity compression and shift, parts that keep different transposes on overlapping lanes
and past a part that stays, parts whose root key alone stays on the part, and, with merge, the root key and Global
Transpose setting the new part keeps or plays into the notes, of parts whose root key differs from the project's, that
are Independent under a transpose event or that differ from one another, including the part for each root key a bounce
makes where a drum map or the chord track keeps them out of the notes; bounce a pitch the global transpose brings back
or takes out of 0-127, merge with Include Inserts' Note Filter and Note Limit on the pitch that plays, a note the new
part's global transpose can't hold, notes above 127 and below 0 with the global transpose that the track's modifiers
bring back, merged without Include Inserts onto the track itself, and a transpose event within a part; merge keeping on
a part of its own a pitch that plays and that the new part's global transpose or the part's transpose takes beyond
0-127, with and without Include Inserts, the part's velocity with a note's velocity variance, and without Include
Inserts onto the track itself a pitch the part's transpose takes beyond 127 that the track's modifiers bring back and
the part's velocity under their compression; merge with Include Inserts putting the part's velocity into the notes where
a Velocity Filter lets a velocity a note's variance gives it through only with the part's velocity, and where a velocity
range of the track (Velocity Filter or Velocity Limit) that the part's velocity changes puts it in the notes, and
keeping it on the part under a velocity shift and a random velocity; a part of its own that no note goes into left out,
also with Include Inserts where a Note Filter leaves all of its notes out, and where it would have come first, which
leaves the first lane and the controllers to the next (#1521); without Include Inserts onto a new track, a pitch that
only the source track's modifiers bring back left out; parts of their own on a destination with a drum map or following
the chord track; the first new part being the earliest part's where none goes into the one with the shared setting, and
the one with the shared setting first, before a part of its own whose source part starts earlier, with two parts of
their own after it in the order of their starts, not their settings; undo and redo of a merge into several parts, and
Erase Destination with a part that stays on a higher lane; merge and bounce taking the global transpose at each note's
position (an Independent part, then one that follows the transpose track across an event, with a Note Filter in merge),
and merge with Include Inserts taking the new part's where a random position moves a note across a transpose event; the
new part's setting coming only from parts with notes (in merge's range); bounce with a Velocity Limit, a Velocity Filter
and a Note Filter that leaves out pitch 60 under a compression, a note that plays nowhere with its part's transpose in
it and one that keeps it on the part under a drum map, the one new part moving past a part that stays, and undo and redo
of a bounce into parts with different root keys; dissolve to lanes and to tracks of a part in another root key and an
Independent part under a transpose event, playing as before; with random pitch and velocity and with velocity variance,
which draw anew, it checks what stays on the part.
`tests/midifunctions_ui_tests.cpp` drives the menu with `ui::Automation`: every item of MIDI > Functions without a
dialog, once each, run on a part through the menu and the engine's command with the expected settings on a copy of it on
another track, comparing notes, controllers and automation, so that a wrong command or mode fails; the item list is
checked against the submenu, and no two items may give the same result. It also runs Legato with and without an overlap
and between selected notes, Fixed Lengths with a tuplet grid, Fixed Velocity, Reverse and Mirror on the project window's
selected parts and on an editor's selected notes, and the dialogs: Transpose Setup by semitones, with scale correction,
keeping notes in range and with a value the engine refuses, Velocity (Add/Subtract by 1 from 64, values out of range,
Compress/Expand, Variance, Fixed To and Limit), Delete Notes (by length, and by velocity, keeping a note exactly at
Minimum Velocity), Restrict Polyphony, Dissolve Part by channel, by pitch and to lanes, Merge MIDI in Loop onto a new
track, refused with no track playing and onto the selected one with Erase Destination and Include Inserts, Bounce MIDI,
and that a second dialog brings the open one to the front.
