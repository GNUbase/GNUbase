# Chord pads

The chord pads and the chord assistant (#174, reference manual pp. 1346-1391): pads with chords that play and record
from the mouse or a MIDI keyboard, the root key, voicings, presets, step input and the List and Circle of Fifths
assistants. They build on the chord track's music theory ([chord-track.md](chord-track.md)).

## Engine

- **The pads** (`Project::chordPads`, `engine/document/chordpads.h`) are saved with the project, in GNUbase's entry
  of the project file. A new project has twelve pads with the major preset on C (C, Dm, Em, F, G, Am, Bdim, five
  unassigned). Each pad has a chord (`midi::chordName`, empty while unassigned), a voicing (the inversion of its basic
  voicing, -12 to 12), adaptive voicing and a lock. `chordPads.get` describes them with each pad's degree in the root
  key, in the scale of the loaded preset (`preset`: in D dorian F is `III`), as a roman numeral (`V`, `ii`, `vii°`,
  `bVII`), its pitches and its remote key; `chordPads.changed` announces every change, also undo and redo. Every
  command that changes them is an undo step. A project file without pads keeps the twelve default ones, and an adaptive
  voicing reference that is no pad becomes none.
- **Players** (pp. 1376-1377): the pads have players (`players`, at least one) and play the selected one's voicings
  (`player`). A basic player has an Octave Offset (`octaveOffset`), a piano or guitar player a Chord Voicing Style of
  the chord track's voicing libraries (`midi::voiceChord` with a `midi::VoicingStyle`, [chord-track.md](chord-track.md)).
  `chordPads.addPlayer`, `chordPads.setPlayer` (name, style, octave offset) and `chordPads.removePlayer` (not the last)
  are the Player Settings pane's Add, Rename and Delete; `chordPads.set` `player` selects one. A project file from
  before the players has one basic player with its octave offset.
- **Voicings** (pp. 1376-1377): the selected player's voicing: the basic player's is the chord track's basic voicing
  (bass from C2, the chord from C3) moved by its Octave Offset, and each voicing is inverted by the pad's voicing. A pad with adaptive voicing takes the
  inversion nearest to the adaptive voicing reference pad's voicing (`chordPads.setPad` `reference`), as the chord
  track's adaptive voicings do; without a reference it plays its basic voicing. Changing a pad's voicing by hand turns
  its adaptive voicing off; locking a pad does too.
- **Assigning** (pp. 1356-1362): `chordPads.assign` takes chords as the chord editor's text input does, or pitches
  (Assign from MIDI Input: the chord they make, as Create Chord Events recognizes it), and "" removes an assignment.
  `chordPads.move` swaps two pads with their settings or copies one onto another; the adaptive voicing reference stays
  on its pad. `chordPads.fromChordTrack` assigns the chord track's chords, each once, in their order.
- **Pad modifiers** (pp. 1351-1352): `chordPads.modify` transposes the chosen pads, adds or takes away tensions (the
  type's ladder: major maj7, or 7 on the key's fifth degree, then 9 and 13; minor 7, 9, 11; the others 7) and steps
  through their voicings. Locked and unassigned pads stay as they are.
- **Root key** (pp. 1362-1365): `chordPads.set` `rootKey` changes the degrees only; with `transposePads` the assigned
  unlocked pads move with it, the nearer way.
- **Presets** (pp. 1370-1371): `chordPads.loadPreset` loads the scale-based presets major (Ionian), minor (Aeolian),
  dorian, phrygian, lydian, mixolydian and locrian, the triads on the scale's seven degrees on a root, which becomes the
  root key; `load` chooses chords and players, chords only or players only (the basic player without offset).
  `chordPads.savePreset` keeps the pads as a preset of the user's own in the user setting `chordPads.presets` (each
  pad's chord, voicing and adaptive voicing, the root key, the scale and the player), which `chordPads.loadPreset`
  brings back pad by pad and `chordPads.deletePreset` deletes; saving is no change to the project. `chordPads.presets`
  lists the scale-based presets, then the user's, the order in which Previous and Next Preset step (p. 1348). The pads
  keep the loaded preset's name (`preset`) and its scale (`scale`), which the degrees and the assistant's levels are
  in.
- **Symbols** (p. 1390): each pad has its degree as a roman numeral (`degree`) and in the Nashville Number System
  (`nashville`: the degree as a number with `m` for minor, `°` for diminished and `+` for augmented, such as `2m`).
- **Playing and recording** (pp. 1366-1368): `chordPads.trigger` plays a pad, or an assistant's chord, until its off,
  as the chord pads' notes (`midi.keyboard` `chordPads`, [onscreen-keyboard.md](onscreen-keyboard.md)). Where they go
  is Chord Pad Output Mode (`outputMode`, p. 1347): on (the default), into every MIDI and instrument track that
  monitors or is record-enabled; off, only into those whose MIDI input is the chord pads (`channel.set`
  `midiInput` `chordPads`, the Inspector's Input Routing, p. 1367), which take nothing else and get the pads in
  either mode. While the transport records, a pad triggered with the mouse or a remote key puts its chord on a
  record-enabled chord track (`chord.setTrack` `recordEnable`) where the cursor is (p. 1368, `chordPads.recordChord`,
  an undo step for each chord). The audio engine keeps the pads' notes in a buffer of their own beside all MIDI inputs' and plays it
  into the tracks its thru list marks (`RenderChannel::padThru`). The tracks record what they play. The highest voice
  goes on MIDI channel 1, the next on channel 2 and so on, as the reference records pad chords. The off ends what the
  on played, even when the pad changed meanwhile. `chordPads.triggered` tells the UI, also about pads whose chords
  another one ended.
- **Overlaps, Latch Chords and Filter Notes** (pp. 1379, 1386): `midi::PadVoices` keeps which trigger (a pad, an
  audition, a remote key) holds which pitches; a pitch sounds once, on the channel of the voice that started it, until
  nobody holds it. A chord played over another treats the other's notes as `overlaps` says: `holdFirst` (the default:
  they go on until their own off, the shared ones don't start again), `legato` (they end but for the shared ones) or
  `stopFirst` (they all end, and the new chord's all start). With `latch` a pad plays until it is triggered again,
  and its off ends nothing. With `filterNotes` the remote keys of pads without a chord play nothing, instead of
  themselves.
- **Remote keys** (pp. 1385-1388): with `remote` on, notes from MIDI input ports on the keys from `remoteStart` to
  `remoteEnd` (C1 to B1 by default) play the pads' chords instead, the first key the first pad, at the note's
  velocity, the voices on channels as above. The render snapshot carries each key's pitches to the audio thread
  (`ChordPadKeys`), where `MidiPorts::take` maps the notes without allocating; a key's note-off ends the pitches its
  note-on played. Keys of unassigned pads and keys outside the range play themselves, and so do the on-screen
  keyboard's. The remote is off in a new project, so that low notes, such as drums from C1, play as they are until the
  user turns it on. The reference manual's pads have no such switch (p. 1386): GNUbase keeps it on purpose, since a
  remote that takes C1 to B1 from every MIDI input as soon as a project has chord pads would take those keys from
  every drum kit. The remote keys go through `midi::PadVoices` on the audio thread too, with the overlaps, latch and
  Filter Notes settings in `ChordPadKeys`, and play into the pads' buffer, so Chord Pad Output Mode routes them.
  `remotePort` picks the MIDI input the remote keys come from (p. 1386, its `midi.portSettings` key; the port's
  `chordPadPortHash` reaches the audio thread), also one outside all MIDI inputs; empty is all MIDI inputs.
- **What the remote keys did** reaches the command thread: the audio thread queues each key that triggered or
  released its pad, each chord modifier key and, while listening, each note (`MidiPorts::PadEvent`); the audio
  engine's cue thread posts them as `chordPads.remoteInput`, and offline `audio.render` runs it when it is done. It
  announces the pads with `chordPads.triggered` (`remote` true), so they light up (p. 1355), and records their chords
  on the chord track as above.
- **Chord modifiers** (p. 1387): with `modifiers`, the keys of `modifierKeys` (previous and next voicing, fewer and more
  tensions, transpose down and up; C2 to F2 by default) play nothing and change the pads whose remote keys are down,
  or else the pad triggered last. The reference changes the selected pads; the engine has no pad selection, so it
  takes the pad that is played. Modify All Pad Voicings, Tensions and Transpose All Pads by aftertouch, pitch bend
  or a controller are not there yet.
- **Assign from MIDI Input and MIDI Learn** (pp. 1360, 1388): `chordPads.listen` `assign` (a pad) takes the next notes
  from the MIDI inputs, which play as they are meanwhile, and once they are all released assigns their chord
  (`chordPads.assign` `pitches`); `learn` `range` takes the next two keys as the remote range, `learn` with a
  modifier's name the next key as its key. While it listens (`midi.padListen`), no remote key triggers a pad.
  `chordPads.listening` and `chordPads.get` `listening` say what it listens for.
- **Step input** (p. 1369): with `stepInput`, `chordPads.step` puts a pad's chord on the chord track at the start of
  the bar of the step position (`stepPosition`, ticks; the chord track is added if there is none) and moves the
  position to the next bar; with `moveInsert` the chord events from there on move a bar later first, without it a
  chord event already there gets the pad's chord. It finds the chord event at the bar's start in the chord track's
  time base, as `chord.add` compares them: in linear time base a bar's start in ticks doesn't always come back from
  its samples. Moved events go a bar on in ticks, and an event on a bar's start onto the next bar's start in the track's
  time base, whatever the tempo there. `chordPads.set` `stepBars` moves the position by bars, as Left and Right Arrow
  move the step input cursor. Moving the cursor is navigation, like moving the project cursor: a `chordPads.set` that
  changes only the step position adds no undo step, and undoing an edit leaves the cursor where it is, but an edit
  that moves it, such as a step, takes it back on undo (the history keeps it apart from the other settings, as it
  keeps the time format, `ProjectChange::stepPositionBefore`). Step input writes chords up to the bar the longest
  project ends in, at the project's tempo (`longestProjectTicks`: below 8 kHz as long as at 8 kHz, so that the ticks
  fit at any sample rate): the position goes from 0 to that bar's end, where a step at it leaves it, and a
  `stepPosition` past it is refused, or clamped in a project file; `stepBars` of any size stops at that bar's start,
  and a step from its end is refused. Move Insert refuses a step while a chord event to move lies past that end, so
  that moving it a bar on can't overflow.
- **Pads onto tracks** (p. 1370): `chordPads.toTrack` puts a pad's chord on the chord track, or its voicing in a MIDI
  part a bar long on a MIDI or instrument track.
- **The chord assistant** (pp. 1372-1375): `chordPads.assistant` with `kind` `list` suggests at most 24 chords after a
  reference chord (major, minor, diminished, 7, maj7 and m7 on every root). Each has a level by how many of its notes
  are outside the scale of the loaded preset on the root key: 0 common (green), 1 adventurous (orange), 2 far (red).
  They come by level, the common ones first; of one level, `cadence` orders them by how strongly they follow (a fifth
  down, a fifth up, the relative chords, a second, a third, then by the distance on the circle of fifths), and
  `commonNotes` keeps those that share that many notes with it. `kind` `circle` gives the twelve major chords in
  fifths from the root key's (in minor, from its relative major's) and their relative minors, with the degrees of the
  key's chords in the Major/Minor choice's scale.

## UI

**Project > Show/Hide Chord Pads** (`window.chordPads`) shows the chord pads zone, a panel of the lower zone
(`ui::ChordPadsPanel`, `src/ui/chordpads.h`), or hides the lower zone while it shows. Its toolbar has Step Input and
Move Insert Mode, the root key with Transpose Pads, the presets with what to load and Load, Assign from Chord Track, the
velocity of mouse clicks, the pad remote control with its range, the basic player's Octave Offset and Show/Hide
Editor/Assistant. The extended toolbar has Select All, Open Chord Editor, Remove Assignment, Transpose Down/Up,
Fewer/More Tensions, Previous/Next Voicing, Lock, Adaptive Voicing and Adaptive Voicing Reference, on the selected pads;
Lock and Adaptive Voicing on several pads are one undo step (`history.transaction`). Each pad (`.chord-pad`) shows its
chord, its degree and its remote key, with the state classes `.selected`, `.triggered`, `.unassigned`, `.locked`,
`.adaptive` and `.reference`; in its upper right corner (`.chord-pad-indicators`) the adaptive voicing reference
indicator (`.chord-pad-reference`) shows, besides the reference pad's frame color, followed by the adaptive voicing
indicator (`.chord-pad-adaptive`) or the padlock (`.chord-pad-lock`) (pp. 1353-1354). A toolbar value the engine
refuses, such as a remote start above the end, goes back to the one the pads have.

- A press on a pad selects it (Ctrl adds or removes it) and plays it until the button goes up, also outside the panel;
  with step input on it also inserts its chord. Released over another pad, it swaps the two; with Alt it copies onto it.
- A double click on a pad, or Open Chord Editor, opens the chord field (`#chord-pads-field`), where Enter assigns the
  chord typed.
- The wheel over a pad: down the next voicing, up the previous one; with Shift it transposes, with Alt it changes the
  tensions. Over the root key it moves the key a semitone, with Transpose Pads as it says (p. 1364).
- With step input on, Left and Right Arrow move the step position a bar back or on wherever the focus is in the zone,
  such as on the Step Input button right after turning it on, except in its fields and menus, which take the keys
  (p. 1369), and with Ctrl, Shift, Alt or Cmd, where they are other key commands (Cmd+Left and Right nudge on
  macOS).
- The assistant shows the List assistant's suggestions after the first selected pad's chord, colored by level
  (`.level-0` to `.level-2`), with an Algorithm menu, or the Circle of Fifths (outer and inner rings, the key's chords
  `.in-key`) with a Major/Minor menu. A press auditions a chord; a double click assigns it to the first unassigned pad.

## Checks

`tests/chordpads_tests.cpp`: presets, degrees in a preset's scale, the tension ladder, inversions and the assistant's
suggestions with their levels in a preset's scale; the default pads, assigning by name and by pitches, the modifiers,
locks, adaptive voicing with a reference, swapping and copying, undo with `chordPads.changed`; the root key with and
without Transpose Pads, presets with what they load, Assign from Chord Track and the pad count and remote range; the
List and Circle of Fifths assistants in major and minor; triggered pads played into a monitoring track on one channel
per voice, ended as they were played, and recorded; a dummy MIDI input's notes on the remote keys playing the pads only
while the remote is on, ended by their note-offs; step input with Move Insert Mode, at a bar's start, over a chord that
is there and moved by bars, also on a chord track in linear time base at a tempo whose bars start between samples and
with Move Insert across a tempo change, `stepBars` of any size and a step position past the longest project's end,
cursor moves without undo steps and steps undone with their chord, moved events, chord track and position, the step
range at 100 Hz and Move Insert refused for a far event, and pads onto the chord track and a MIDI track; a file with no pads or a reference below -1; and saving and loading.
`tests/keycommandsdialog_tests.cpp`: the key commands don't list `chordPads.trigger` without parameters.
`tests/chordpads_ui_tests.cpp` drives the zone: shown and hidden from the Project menu's command, a pad held with the
mouse recorded on a record-enabled track, a chord typed after a double click, pads swapped by dragging, the extended
toolbar, the wheel with and without Shift, Select All with Remove Assignment past a locked pad, Load and the root key
with Transpose Pads, the assistant's suggestions and circle with a double click that assigns, the padlock and adaptive
voicing indicators with Lock on two pads as one undo step and its undo, the reference indicator beside them inside the
pad and in its upper right corner, a refused remote start going back, the wheel over the root key, and Left and Right
with step input, on a pad and on the Step Input button, as no undo step, but not in the velocity field, the chord field
or the root key menu, or with Cmd.

## Not yet

Chord Pad Output Mode off with the Chord Pads input routing (pads always play into all MIDI inputs); the Guitar and
Piano players, several players on several tracks and the Player Settings pane; the Pattern and Sections player modes
with Overlaps and Filter Notes; Latch Chords, the chord modifiers' remote keys, MIDI Learn and the MIDI Input choice of
the remote; Snap Playback to Musical Grid; recording chord events on the chord track; saving your own presets and the
preset browser; the Proximity assistant and the List assistant's cadence types; the zone's keyboard, the voicing and
List assistant color indicators and the Chord Pads Display Settings pane (roman numerals and Nashville numbers as the
primary symbol, the grid layout); the chord editor's buttons (the zone types chords as text); dragging pads onto
tracks and assistant chords onto pads with the mouse (the engine commands exist); the pads' key commands (Ctrl+A,
Delete, Return, Ctrl+Shift+L); auditioning a pad when a modifier changes it; the pads lighting when a MIDI
keyboard triggers them; Assign from MIDI Input with its button and the pad's red frame (pp. 1350, 1360: like the key
editor's MIDI input, live MIDI keyboard notes don't reach a command yet; `chordPads.assign` takes their pitches); the
Root Key pane's Enh. Shift (p. 1363: chord names have one spelling); and the step input cursor in the ruler.
