# Track time base

Whether a track's events follow the tempo (musical time base) or keep their time (linear time base), switching between
the two, and the time base of new tracks (#157, reference manual pp. 229-230, 1446, 1595). The positions themselves are
the time model's (#13, `engine/time/tempomap.h`: `time::Position` in ticks or samples, `time::rebase`); the tempo is the
tempo track's ([tempo-track.md](tempo-track.md)).

## Engine

- **A track's time base** (`Track::timeBase`, `track.add` and `track.set` `timeBase`: `musical` or `linear`) is the
  unit of everything on it: audio events, audio and MIDI parts, markers and automation points are in ticks on musical
  tracks, so a tempo change moves them in time, and in samples on linear ones, so they stay.
- **By event type** (p. 1446). Audio events keep their length in samples in either time base: on a musical track only
  their start follows the tempo, not the audio. MIDI parts have their start and length in the track's unit; their notes
  and controllers are in ticks from the part's start. On a linear track a tempo change keeps them at their time too
  (`tempoChanged` in `tempo.cpp`, with the tempo before the change): their ticks are worked out again through seconds,
  as are a quantized note's original and quantized timing. Shared copies that then differ, such as copies before and
  after a tempo event or a linear and a musical copy, split by what they hold: the largest set of copies that hold the
  same notes and controllers as each other (the first of sets as large) keeps the group and the ids of its notes and
  controllers, every other such set shares its notes in a new group with new ids, and a copy that matches no other
  becomes a real copy, as `event.toRealCopy` makes it. A `midiPart.changed` event comes for every part whose notes or
  controllers this moves and for every copy that the split gives new ids or takes out of its group, even if its notes
  stay where they are; not for the other parts, such as those before the change. The shared copies an inactive track
  version holds split the same way, without events ([track-versions.md](track-versions.md)). Each linear part's notes
  and controllers are retimed through the exact time of the part's start and rounded once, so shared copies at the same
  tempo round to the same ticks and stay shared, also at sample positions that aren't on a tick; they don't drift apart
  a tick at a time (#865). Copies that genuinely retime differently still split, however little they differ: a tempo
  event between them, a ramp under one of them, or a musical copy that a change never retimes. Cycle markers that a
  faster tempo makes shorter than a sample get longer.
- **Switching** (`track.set` `timeBase`, `rebaseTrack` in `projectmodel.cpp`) converts every position on the track to
  the new unit at the current tempo, so nothing moves; it is one undo step. Converting rounds to the nearest tick or
  sample, the small loss of precision the reference manual mentions (p. 230).
- **New tracks** (p. 230, Default Track Time Type, p. 1595): `track.add` without `timeBase` takes the user setting
  `editing.defaultTimeBase`: `musical` (the default), `linear`, or `followTimeFormat`, musical while the project's
  primary time format (`project.set` `timeFormat`, the transport's main display) is `barsBeats` and linear with any
  other. It applies to audio, instrument, MIDI, group, effect and marker tracks; folder, ruler, tempo and signature
  tracks hold no events of their own and are musical. Every command that adds tracks goes through `track.add`, so the
  setting reaches them, except MIDI file import, whose new tracks are musical: their parts and notes are in ticks.
  With `singlePart` and `track` on a linear MIDI or instrument track of the project, the part's start and length are
  in samples, as on every linear track, and its notes stay in ticks ([file-import.md](file-import.md)).
  `marker.add` without a marker track adds one that way too, so a numeric `position` is in ticks or samples by the
  setting: a caller that gives numbers adds the marker track first with a `timeBase`, or reads the added track's
  `timeBase` from `project.get`.

## UI

- **Switch Time Base** (p. 229) in the inspector's Basics section (`button[control=timeBase]`) and the track list's
  `timeBase` control ([track-list.md](track-list.md)) on audio, instrument, MIDI, group and effect tracks, and in the
  inspector on VCA fader tracks too (pp. 179–181): `.checked` when the track is linear; a click sends `track.set`
  `timeBase`.

## Not yet

The note and clock symbols of the reference (the button shows "L"), a preferences dialog for the default (#162),
moving linear MIDI parts keeping their notes' time across tempo changes inside the move, and time warp.

## Tests

`tests/tempo_tests.cpp`: musical events moving and linear ones staying under tempo changes, the notes and controllers of
linear parts keeping their time through tempo events, ramps, the project tempo, tap tempo, a MIDI file's tempo and the
tempo mode, and undo, with `midiPart.changed` for exactly the parts that change, each as `project.get` has it; shared
copies of linear parts becoming real copies when their notes retime differently, copies that still match staying shared
in a new group, off-tick copies at the same tempo staying shared through a change, copies whose notes retime onto half a
tick rounding alike, small tempo steps splitting a shared musical copy and linear copies around a tempo event instead of
moving them, and the largest set keeping the group; switching the time base of audio, MIDI and marker tracks with
automation keeping everything in place, then following the tempo or not; `editing.defaultTimeBase` for every track type,
a marker track `marker.add` adds, and the primary time format. `tests/inspector_tests.cpp`: Switch Time Base in the
inspector; `tests/tracklist_tests.cpp`: in the track list, also on group and effect tracks.
`tests/automation_tests.cpp`, `tests/marker_tests.cpp` and `tests/document_tests.cpp` have the automation, markers and
snapshot in both time bases.
