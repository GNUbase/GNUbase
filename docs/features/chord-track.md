# Chord track

Chord functions (#173, #933, #939, reference manual pp. 165, 1319-1345): the chord track with its chord and scale
events, voicings and voicing libraries, converting chord events to MIDI, MIDI tracks that follow the chord track,
Live Input, chord events made from MIDI, from audio and from a recording, Map to Chord Track, Assign Voices to Notes
and auditioning chord events.

## Engine

- **Music theory** (`engine/midi/harmony.h`): a chord (`midi::ChordSymbol`) has a root, a type (maj, min, dim, aug,
  sus2, sus4), tensions (6, 7, maj7, b9, 9, #9, 11, #11, b13, 13) and a bass note. `parseChord` takes what the chord
  editor's text input takes (p. 1322): such as `Cmaj7`, `Dm7(9)`, `G7(b9,b13)`, `C/E`, `Bbsus4`, `Cm7b5` or
  `C7sus4` (a seventh before `sus`, as commonly spelled, or after it); 9, 11 and 13 bring the seventh unless after
  `add`, `maj` or a sixth or seventh. `chordName` gives the name the events show and `parseChord` takes back, with
  sharps, a sus chord's seventh before `sus` (`C7sus4`); an undefined chord is `X`. Scales are the transpose
  function's (`midi::Scale`) named like `D dorian`, with the scales of altered chords: lydian dominant, mixolydian b6,
  phrygian dominant, altered and the two diminished scales (half-whole, whole-half). Each chord has an automatic
  scale: its family's (major, mixolydian with the seventh, lydian with #11, minor, dorian with the seventh or sixth,
  melodic minor with maj7, locrian, the whole-half diminished scale for dim7, whole tone), or, if that misses some of
  the chord's notes, the scale of its family that holds the most of them (#939): `C7(#11)` lydian dominant,
  `G7(b13)` mixolydian b6, `E7(b9)` phrygian dominant, `G7(#9)` half-whole diminished, `Dm7(b9)` phrygian.
- **The chord track** (`TrackType::Chord`, `"chord"`) has no channel, stays at the top level and is there at most once.
  Its events and settings are `Project::chords`; removing the track removes them. Switching its time base and changing
  the sample rate keep the events where they are, as for markers.
- **Chord and scale events**: a position in the track's time base and a chord, or a scale. Each plays from its position
  to the next of its kind, in timeline order, at most one of a kind per position. `chord.add` (default: the cursor and
  an undefined chord, as the draw tool adds them; adds the chord track if there is none), `chord.set` and
  `chord.remove` are undo steps; `chord.changed` announces every change with what `chord.get` returns: each chord
  event with its scale and voicing (`pitches`), and for events made from audio the audio event (`audioEvent`) and the
  chords detected there (`detected`).
- **Settings** (`chord.setTrack`, pp. 165, 1333): Automatic Scales (`automaticScales`, on by default: a chord event's
  scale is its chord's; off, the scale event playing there, or the chord's before the first), Adaptive Voicings
  (`adaptiveVoicings`), the voicing library (`voicing`), Shift Chords (`shiftChords`, ticks, at most a quarter note
  either way: a negative shift lets each chord event take the notes played that much before it), Mute Chord Track
  (`mute`: no track follows it, no Live Input, no auditioning), Show Scales (`showScales`), Record Enable
  (`recordEnable`), Select Track for Auditioning (`audition`, a MIDI or instrument track) and Acoustic Feedback
  (`acousticFeedback`).
- **Voicings** (pp. 1333-1335, `midi::voiceChord`): the library `basic` puts the bass note (or root) in the octave from
  C2, the chord's notes above it in root position from C3, moved by the Octave Offset (`octaveOffset`, -3 to 3).
  Adaptive voicings take the inversion of the upper notes, from E2 to C5, that moves them the least from the chord
  before. `piano` has the styles (the library subset, `style`) triads, triads with maj9 (the major ninth instead of
  the root), triads with maj9 and min9 (a diminished triad with the minor ninth), 4-note chords without the root, 4-note
  chords without root and fifth (open jazz) and 5-note chords with the ninth, and the Custom Voicing panel's options:
  Add Root Note (`addRoot`, from `lowestRoot`), Duplicate Root (`duplicateRoot`), Fatten Up (`fattenUp`, the lowest
  upper note an octave higher) and the range of the upper notes (`lowest`, `highest`). `guitar` voices chords as
  shapes on a guitar in standard tuning within four frets and open strings, on strings next to each other: triads and
  4-note chords on four to six strings with the bass note lowest, 3-string triads and modern jazz (third, seventh and
  tensions, root and fifth where they fit); the lowest position with the most strings, adaptive the nearest shape.
- **Chords to MIDI** (`chord.toMidi`, p. 1336): a new MIDI part on a MIDI or instrument track with each chord's voicing
  from its event to the next one, the last one a bar long from where it starts. With `events` only those (chord
  events dragged onto a track); without, all of them, and the track follows the chord track with Voicings.
- **Follow Chord Track** (`chord.follow`, pp. 1337-1341): a MIDI or instrument track's mode, `off`,
  `chordsAndScales`, `chords`, `scales`, `rootNotes`, `voicings` or `singleVoice` (one voice of the new chord's voicing,
  `voice`: soprano, alto, tenor or bass), and its source: the chords its notes were played over. `sync` sets the
  source as the Follow Chord Track dialog does: `direct` (Follow Directly, the default the first time: the chord track
  as it is), `analyze` (Analyze Chords: the chords of the track's MIDI notes) or `known` with `chord` (Apply a Known
  Chord). This Track's Voicings (`voicings`): the chord track's or a library's. A Single Voice track with a `master`
  takes the master track's voicings. The settings are the chord track's, in `chord.get` `follow`, saved with it; they
  go when their track goes, with undo bringing them back, and a duplicated track takes its original's.
- **Live Input** (`chord.follow` `liveInput`, p. 1338): `chords` or `scales` moves the notes played into a MIDI or
  instrument track to the nearest note (down on a tie) of the chord or scale playing at the transport position. The
  render snapshot carries the chord track's pitch classes by position to the audio thread (`RenderChannel::liveInput`,
  `LiveHarmony`), where the sequencer maps each note-on without allocating; its note-off ends what it played, once no
  other held key plays that note too.
- **Create Chord Events** (`chord.fromMidi`, pp. 1342-1343): chord events from the MIDI notes of parts or tracks.
  Wherever the sounding notes change, the chord they make (at least three pitch classes that a chord type on one of
  them holds, the others as tensions; the fewest tensions, then the lowest note as the root) becomes a chord event if
  it differs from the one before. Include Bass Notes (`includeBass`), Include Tensions (`includeTensions`), Detect
  Arpeggios (`detectArpeggios`: a note sounds on to the end of its bar, and the chord starts where its first note in
  the bar does), Interpret Sustain Pedal (`sustainPedal`: a note that ends while controller 64 is down lasts until it
  is let up) and Ignore Notes Shorter Than (`minLength`, ticks). The chord events within the notes' range are replaced.
- **Chord events from audio** (`chord.fromAudio`, pp. 1343-1344): each quarter note of an audio event is matched against
  the triads, sus chords and sevenths on every root by the strength of its pitch classes (from the power spectrum,
  100 Hz to 4 kHz); a quarter note whose chord differs from the one before starts a chord event, silence makes none.
  The events keep the audio event and the five chords that fit best (`detected`), as the chord assistant's Detected
  tab lists them, and replace the chord events within the audio event. Once the audio event is removed, `chord.get`
  lists neither, until undo brings the audio event back.
- **Recording chord events** (p. 1345): with the chord track record-enabled, a recording's notes (`recording.add`)
  make chord events as Create Chord Events does, each take's (a cycle recording's lap) replacing those over it, take
  after take, so that laps don't mix and the last lap that recorded notes wins. The notes come from the MIDI and
  instrument tracks that record and, while the chord track is record-enabled, from those that only monitor: the
  recorder records their input too (`MidiRecordTrack::chordsOnly`), and `recording.add` takes it (`chordsOnly`) for
  chord events only, without a part. That input also stays in the track's retrospective buffer, marked as recorded
  (`recordedUntil`), so that a recording that starts within the MIDI record catch range of the one before doesn't
  catch it again. Inserting the buffer (`midi.insertRetrospective`) is no recording of the chord track: it makes no
  chord events (`recording.add` `chordEvents` false), so what was edited on the chord track in between stays.
- **Map to Chord Track** (`chord.map`, p. 1341): moves the notes of parts to the chord track's chords for good, by a
  follow mode: each part's own chords (C where none are found) map to the chord events playing at each note.
- **Assign Voices to Notes** (`chord.assignVoices`, p. 1342): moves each note to the nearest pitch of the voicing of
  the chord event playing at its start.
- **Auditioning** (`chord.audition`, pp. 1329-1330): plays a chord event's voicing, a scale event's notes or a chord on
  the audition track as if from its MIDI input (`midi.inject`), until `on` false; a frozen audition track takes no MIDI,
  so it is refused there. The held chord's note-offs are only skipped once its channel is no longer an unfrozen MIDI or
  instrument track's (a frozen track takes no MIDI); if they can't be sent otherwise (too much MIDI waits for the
  device), the command fails and the chord stays held for the next call to end. With Acoustic Feedback the render
  snapshot also plays the chord events' voicings on the audition track (`renderChordTrack`), each to the next, the last
  a bar long.
- **The project file** keeps all of it in GNUbase's entry. It is read as the commands would make it (#939): unknown
  chords are undefined, unknown scales and events before the project start are left out, an unknown follow mode is
  `off` (it would otherwise play like Chords), and other unknown settings are their defaults.

## Playback

The render snapshot maps each note of a track that follows the chord track (`ChordFollower`,
`document/chordtrack.h`, made once per track) before the MIDI modifiers and global transposition: from the source harmony at the note's
start to the chord event playing there (with Shift Chords, at the note's start minus the shift), the source's chords
with their automatic scales and the track's voicings. Every mode but Voicings and Single Voice first moves the pitch
by the step between the roots, the nearer way (-5 to 6 semitones); Root Notes stops there, Chords moves it to the
nearest note of the new chord and Scales to the nearest note of the new scale (down on a tie). Chords & Scales keeps
chord notes the same chord notes (root, third and so on) and the others the same degree of the new scale, as the
transpose function's scale correction does. Voicings plays the voice of the new voicing that the pitch is of the old
one, in the same octave; a pitch outside the old voicing maps as Chords does. Single Voice plays its voice of the new
voicing. Nothing moves before the first chord event, over an undefined chord, on a drum-mapped track or while the
chord track is muted. The notes themselves stay as they are.

## UI

- **The chord track's lane** (`ui::GlobalEventLane` with `LaneKind::Chord`, `src/ui/transposetrack.h`, shared with the
  transpose track's lane) shows each chord event's chord from its position (`.chord-row`, `.chord-event`, `.detected`
  for events made from audio): the draw tool adds an undefined chord at the nearest bar and opens it for typing
  (Define Chord by Text Input, p. 1322), a double click opens it too, a drag moves it to the nearest bar and the erase
  tool removes it. With Acoustic Feedback and a track for auditioning, a press on an event auditions it while the
  button is down.
- **The scale lane** (`LaneKind::Scale`, `.scale-row`, `.scale-event`) shows the scale events in the lower half of the
  chord track's lane while Show Scales is on, the chord events in the upper half; the draw tool adds a C major scale
  event to type over (such as `D dorian`), and it edits, drags, erases and auditions as the chord lane does.
- **The track list's chord track controls** (`ui.trackControls` `chord`): Record Enable (`chordRecord`), Mute Chord
  Track (`chordMute`), Show Scales (`showScales`), Switch Time Base and Select Track for Auditioning
  (`chordAudition`).
- **Add Track** has Chord, once per project: a second one is refused with the engine's message.
- **Project > Chord Track**: Chords to MIDI (`track.chordsToMidiSelected`: `chord.toMidi` onto the first selected MIDI
  or instrument track), Create Chord Events (`track.createChordEventsSelected`: `chord.fromAudio` from audio events,
  and `chord.fromMidi` from MIDI parts with the options of the Create Chord Events dialog, `create-chord-events.rml`),
  Map to Chord Track (`track.mapToChordTrackSelected`, with the mapping mode of `map-to-chord-track.rml`) and Assign
  Voices to Notes (`track.assignVoicesSelected`). They take the selected MIDI parts and audio events (pp. 1341-1344),
  or with none selected those of the selected tracks; audio events in audio parts never count (p. 1344), and selected
  audio parts are a selection of nothing, not a reason to fall back to the tracks.
- **The Inspector's Chords section** has a MIDI or instrument track's Follow Chord Track, the voice and master of
  Single Voice, This Track's Voicings and Live Input while the project has a chord track; the chord track's own
  **Chord Track section** has Adaptive Voicings, Automatic Scales, Acoustic Feedback, the voicing library and its
  subset, the basic voicings' Octave Offset, the piano's Custom Voicing options and range, and Shift Chords in ticks
  of 480 to a quarter note ([inspector.md](inspector.md)).

## Checks

`tests/harmony_tests.cpp`: chord names and text input with `C7sus4`, chord intervals, scales and automatic scales that
hold every note of their chords, recognizing chords, basic, piano and guitar voicings, and every Follow Chord Track mode
with Single Voice over the I-vi-IV-V progression. `tests/chordtrack_tests.cpp`: chord and scale events (adding the
track, timeline order, voicings and scales, the settings, undo, the track's removal), Chords to MIDI with the last chord
a bar long off the bar line, Follow Chord Track over reference progressions, on drum-mapped tracks, with Mute Chord
Track, Shift Chords, voicing libraries, This Track's Voicings and Single Voice with a master, follow settings that go
with their track and come with its copy, Create Chord Events with Detect Arpeggios and Interpret Sustain Pedal, Map to
Chord Track and Assign Voices to Notes, chord events from a synthesized audio file and after its removal, recorded chord
events, also a cycle recording's laps and from a track that only monitors (whose input stays in its retrospective
buffer, which a later recording's catch range and an insert of the buffer don't make chord events from again), Live
Input (also two keys on one note) and auditioning on the dummy device with the note probe, note-offs that don't fit, a
held chord on a removed or frozen audition track and the refusal to audition on a frozen one, Acoustic Feedback, and
saving and loading, also of a hand-edited file. `tests/chordtrack_ui_tests.cpp`: Add Track's Chord page and its refusal
of a second one, drawing, typing, dragging, double-clicking and erasing chord events on the lane, the scale lane, the
track list's chord track controls, Project > Chord Track's items and dialogs, whose options reach the commands, on the
selected parts (a selected audio part counts as one, also for Map to Chord Track and Assign Voices to Notes with a MIDI
track selected) or the selected tracks' parts and loose audio events, pressing chord and scale events with Acoustic
Feedback, and the Inspector's chord settings.

## Not yet

- Audio and pitch-corrected audio segments following the chord track, which need the audio pitch editing of #183.
- The chord editor's buttons, keyboard display and Activate MIDI Input; the chord track's Chord Assistant (the chord
  pads have the List and Circle of Fifths assistants, [chord-pads.md](chord-pads.md)) and its Detected tab, whose
  chords `chord.get` already lists; the info line of chord and scale events with its per-event Voicing; the Musical
  Scale Setup dialog; Resolve Display Conflicts.
- The Follow Chord Track dialog in the Inspector (the engine's `sync` has its choices) and Start Voicing.
- Note voices on the info line after Assign Voices to Notes.
- Dragging chord events onto MIDI effects or instrument pads, and dropping parts or audio events onto the chord track.
- Detect Arpeggios finds arpeggios a bar at a time: two arpeggiated chords in a bar make one or none. Chord detection
  from audio works on quarter notes of the beat grid.
