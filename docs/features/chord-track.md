# Chord track

Chord functions (#173, reference manual pp. 165, 1319-1345): the chord track with its chord and scale events,
voicings, converting chord events to MIDI, MIDI tracks that follow the chord track and chord events made from MIDI.

## Engine

- **Music theory** (`engine/midi/harmony.h`): a chord (`midi::ChordSymbol`) has a root, a type (maj, min, dim, aug,
  sus2, sus4), tensions (6, 7, maj7, b9, 9, #9, 11, #11, b13, 13) and a bass note. `parseChord` takes what the chord
  editor's text input takes (p. 1322): such as `Cmaj7`, `Dm7(9)`, `G7(b9,b13)`, `C/E`, `Bbsus4` or `Cm7b5`; 9, 11 and
  13 bring the seventh unless after `add`, `maj` or a sixth or seventh. `chordName` gives the name the events show and
  `parseChord` takes back, with sharps; an undefined chord is `X`. Scales are the transpose function's (`midi::Scale`)
  named like `D dorian`. Each chord has an automatic scale (major, mixolydian with the seventh, lydian with #11, minor,
  dorian with the seventh or sixth, melodic minor with maj7, locrian, whole tone).
- **The chord track** (`TrackType::Chord`, `"chord"`) has no channel, stays at the top level and is there at most once.
  Its events and settings are `Project::chords`; removing the track removes them. Switching its time base and changing
  the sample rate keep the events where they are, as for markers.
- **Chord and scale events**: a position in the track's time base and a chord, or a scale. Each plays from its position
  to the next of its kind, in timeline order, at most one of a kind per position. `chord.add` (default: the cursor and
  an undefined chord, as the draw tool adds them; adds the chord track if there is none), `chord.set` and
  `chord.remove` are undo steps; `chord.changed` announces every change with what `chord.get` returns: each chord
  event with its scale and voicing (`pitches`).
- **Settings** (`chord.setTrack`, p. 1333): Automatic Scales (`automaticScales`, on by default: a chord event's scale
  is its chord's; off, the scale event playing there, or the chord's before the first), Adaptive Voicings
  (`adaptiveVoicings`) and the basic voicings' Octave Offset (`octaveOffset`, -3 to 3).
- **Voicings** (pp. 1333-1335): basic voicings put the bass note (or root) in the octave from C2, the chord's notes
  above it in root position from C3. Adaptive voicings take the inversion of the upper notes, from E2 to C5, that
  moves them the least from the chord before.
- **Chords to MIDI** (`chord.toMidi`, p. 1336): a new MIDI part on a MIDI or instrument track with each chord's voicing
  from its event to the next one, the last one a bar long. With `events` only those (chord events dragged onto a
  track); without, all of them, and the track follows the chord track with Voicings.
- **Follow Chord Track** (`chord.follow`, pp. 1337-1341): a MIDI or instrument track's mode, `off`,
  `chordsAndScales`, `chords`, `scales`, `rootNotes` or `voicings`, and its source: the chords its notes were played
  over. `sync` sets the source as the Follow Chord Track dialog does: `direct` (Follow Directly, the default the first
  time: the chord track as it is), `analyze` (Analyze Chords: the chords of the track's MIDI notes) or `known` with
  `chord` (Apply a Known Chord). The settings are the chord track's, in `chord.get` `follow`, saved with it.
- **Create Chord Events** (`chord.fromMidi`, pp. 1342-1343): chord events from the MIDI notes of parts or tracks.
  Wherever the sounding notes change, the chord they make (at least three pitch classes that a chord type on one of
  them holds, the others as tensions; the fewest tensions, then the lowest note as the root) becomes a chord event if
  it differs from the one before. Include Bass Notes (`includeBass`), Include Tensions (`includeTensions`) and Ignore
  Notes Shorter Than (`minLength`, ticks). The chord events within the notes' range are replaced.

## Playback

The render snapshot maps each note of a track that follows the chord track (`ChordFollower`,
`document/chordtrack.h`) before the MIDI modifiers and global transposition: from the source harmony at the note's
start to the chord event playing there, the source's chords with their automatic scales and voicings. Every mode but
Voicings first moves the pitch by the step between the roots, the nearer way (-5 to 6 semitones); Root Notes stops
there, Chords moves it to the nearest note of the new chord and Scales to the nearest note of the new scale (down on a
tie). Chords & Scales keeps chord notes the same chord notes (root, third and so on) and the others the same degree
of the new scale, as the transpose function's scale correction does. Voicings plays the voice of the new voicing that
the pitch is of the old one, in the same octave; a pitch outside the old voicing maps as Chords does. Nothing moves
before the first chord event, over an undefined chord or on a drum-mapped track. The notes themselves stay as they are.

## UI

- **The chord track's lane** (`ui::GlobalEventLane` with `LaneKind::Chord`, `src/ui/transposetrack.h`, shared with the
  transpose track's lane) shows each chord event's chord from its position (`.chord-row`, `.chord-event`): the draw
  tool adds an undefined chord at the nearest bar and opens it for typing (Define Chord by Text Input, p. 1322), a
  double click opens it too, a drag moves it to the nearest bar and the erase tool removes it.
- **Add Track** has Chord, once per project.
- **Project > Chord Track**: Chords to MIDI (`track.chordsToMidiSelected`: `chord.toMidi` onto the first selected MIDI
  or instrument track) and Create Chord Events (`track.createChordEventsSelected`: `chord.fromMidi` from the selected
  MIDI and instrument tracks).
- **The Inspector's Chords section** has a MIDI or instrument track's Follow Chord Track while the project has a chord
  track ([inspector.md](inspector.md)).

## Checks

`tests/harmony_tests.cpp`: chord names and text input, chord intervals, scales and automatic scales, recognizing
chords, basic and adaptive voicings, and every Follow Chord Track mode over the I-vi-IV-V progression.
`tests/chordtrack_tests.cpp`: chord and scale events (adding the track, timeline order, voicings and scales, the
settings, undo, the track's removal), Chords to MIDI with the part following the chord track, Follow Chord Track with
Apply a Known Chord, Follow Directly and Analyze Chords over reference progressions, Create Chord Events with its
options, and saving and loading. `tests/chordtrack_ui_tests.cpp`: Add Track's Chord page, drawing, typing,
dragging, double-clicking and erasing chord events on the lane, Project > Chord Track's items and the Inspector's
Follow Chord Track.

## Not yet

The chord editor's buttons and keyboard display, the Chord Assistant (pp. 1322-1328), auditioning chord events
(Acoustic Feedback), Show Scales and the scale lane, the info line of chord and scale events, the Musical Scale Setup
dialog, Guitar and Piano voicing libraries and the Custom Voicing panel, Shift Chords, Single Voice with master and
follower tracks, This Track's Voicings and the Follow Chord Track dialog in the Inspector, Live Input, audio and
pitch-corrected audio segments following the chord track (needs pitch shifting in the disk streams, #144), Map to
Chord Track, Assign Voices to Notes, Detect Arpeggios and Interpret Sustain Pedal, chord events from audio events,
recording chord events from a MIDI keyboard, dragging chord events onto MIDI effects or instrument pads, Create Chord
Events from selected parts and its dialog, and Resolve Display Conflicts.
