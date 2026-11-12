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
  track's pitches, which a new track, without either, plays the same. A destination with a drum map or following the
  chord track gets a pitch that plays them (a note's own, if it does, so that a track merged onto itself keeps its
  pitches), or if none does what played, which it then maps. The new part gets the root key and Global Transpose setting
  ([transpose.md](transpose.md)) the parts its notes come from share, or, when they differ, the project root key and
  follow, and then each note the difference its part's global transposition makes played into it, so that it keeps the
  pitch it played with (#954); a note the difference takes beyond 0-127 didn't play where it came from either and is
  left out. With `eraseDestination`, a shared copy that loses events leaves its copies, which keep theirs; its events
  get new ids, so the play probability, velocity variance and random MIDI modifier values of the notes that stay draw
  anew. `midi.dissolve` copies events by channel or by pitch into new parts at the same place and mutes the parts (pp.
  1121-1122): `to: tracks` puts them on new MIDI tracks routed to the same instrument, `to: lanes` on new lanes of the
  track below the lanes its parts are on (`optimize` only works with tracks). Events of no channel or pitch go with the
  first new part of their part, so none is lost; new tracks get the track's MIDI modifiers. The copied events get new
  ids, so play probability and velocity variance, drawn from part and note ids, draw anew. `midi.bounce` combines a
  track's parts into one on the lowest of their lanes, removes muted parts and writes the parts' transpose and velocity
  into the notes (p. 1123), as playback applies them: a note transposed beyond 0-127, which doesn't play, is left out.
  Where the track's drum map or chord track following would then play another pitch, because they map a note before its
  part's transpose, the new part keeps the transpose, and parts with different ones make a part for each, so that
  everything plays as before. The new part gets the root key and Global Transpose setting the parts share, or, when they
  differ, the project root key and follow, and then each note the difference its part's global transposition makes
  played into its pitch, so that it keeps the pitch it played with (#954). The track's MIDI modifiers stay on the track,
  and the copied notes get new ids, so play probability and velocity variance, drawn from part and note ids, draw anew.
- **Extract MIDI Automation** (`midi.extractAutomation`, p. 1129) moves the continuous controllers of parts into controller automation lanes of their tracks (`automationLane.*`, [automation.md](automation.md)), one per controller and channel: a step point where each event plays, its value scaled to 0-1. An existing lane loses its points in the part's range first, except those an earlier part of the same call added, so overlapping parts keep each other's, and reads from then on, so that what was moved plays. The points play at the part's place only, so a shared copy leaves its copies, which keep their controllers; all its events get new ids, so the play probability, velocity variance and random MIDI modifier values of its notes, drawn from part and note ids, draw anew. Pitch bend, pressure, switches such as the sustain pedal, events past a part's end and muted parts stay, as on p. 1129.
- **The MIDI menu** (`resources/ui/project.rml`, `src/app/appmidifunctions.cpp`, reference manual pp. 1118-1131) runs them in the app. As in the reference, they work on what the Edit menu's quantize commands take ([quantize.md](quantize.md)): in an editor, its selected notes, or every note it edits without a selection; from no zone (Lua, MCP, the menu in the project window), an open editor's selected notes first, else the project window's selected MIDI parts. MIDI > Functions: Legato (`midi.functionsLegato`) extends by the user setting `editing.legatoOverlap` (Legato Overlap, in ticks of 480 to a quarter note, negative for a gap) and, with `editing.legatoBetweenSelected` (Legato Mode: Between Selected Notes Only, p. 1599), only to the next selected note; Fixed Lengths sets the editor's length quantize, or the quantize grid's step with its tuplet when that is linked to it, as in the project window and as Edit > Quantize Lengths takes it (swing and grooves, which change from step to step, don't count); Fixed Velocity the editor's insert velocity (100 in the project window); Pedals to Note Length, Delete Overlaps (mono) and (poly), Delete Doubles, Delete Controllers, Delete Continuous Controllers, Thin Out Data, Extract MIDI Automation, Reverse and Mirror (`midi.invert`) run with no settings; Velocity... (Type with Amount, Ratio in percent, Lower and Upper, or Variance in percent; velocities in MIDI 1.0 values, which it converts as playback does (`velocityFromMidi1`), so 64 less 1 is 63; out-of-range values are refused in the dialog's units), Delete Notes... (Minimum Length in ticks of 480 to a quarter note, Minimum Velocity, Remove when under with both; p. 1140) and Restrict Polyphony... ask first. Transpose Setup... asks for the semitones, Scale Correction from a current to a new scale and Keep Notes in Range with the highest and lowest note. Merge MIDI in Loop... (MIDI Merge Options: Include Inserts, on by default, Erase Destination, Include Chase) merges between the locators the tracks that play onto the first selected MIDI or instrument track, or a new one. Dissolve Part... separates the project window's selected parts by channel or by pitch, onto tracks with Optimized Display or onto lanes; Bounce MIDI bounces the selected parts of each track, one undo step for several tracks. The dialogs are one document, `resources/ui/midi-function.rml`, whose Lua runs the engine's command and shows its error if it refuses, and it enables the fields the chosen settings use.
- **One dialog at a time.** Dialogs whose Lua keeps its state in a global table open with `AppUi::openOnce`, which takes the document's attributes: while one is open, opening it again, from another window or through MCP, brings it to the front as it is. The MIDI menu's dialogs, Add Track and Auto Fades work this way; the fade and crossfade editors already open one at a time through `EventDisplay::openFadeDialog`.
- **Not done yet:** an articulation map's transpose, channel and output events in merged notes; playing MIDI effects into the merged part: tracks have MIDI insert and send effects ([midi-effects.md](midi-effects.md)), and on p. 1120 Include Inserts applies the insert effects as well as the MIDI modifiers and Include Sends the send effects, but here Include Inserts applies the MIDI modifiers only, and the dialog has no Include Sends or Convert VST3. Chase options beyond controllers; giving the tracks that Dissolve Part makes the MIDI channel they hold (MIDI tracks have no channel setting yet); note names in Transpose Setup's range fields.

## Checks

`tests/midifunctions_tests.cpp` has a test for each function, including `playingTracks` with solo, mute, a muted group
channel and a disabled track, merge with and without `includeInserts`, dissolve's new tracks with the track's MIDI
modifiers, Extract MIDI Automation on a shared copy, Bounce MIDI of shared copies, Merge with Erase Destination on a
shared copy, the pitches that play before and after a bounce or a merge on tracks with a drum map or following the chord
track, and the root key and Global Transpose setting a bounce or a merge keeps or plays into the notes, of parts whose
root key differs from the project's, that are Independent under a transpose event or that differ from one another.
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
