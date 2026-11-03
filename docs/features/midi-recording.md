# MIDI recording

Recording MIDI onto record-enabled MIDI and instrument tracks (#97, reference manual pp. 347-355 and the MIDI and
Record - MIDI preferences, pp. 1609-1616). It shares record enable, the transport's record state, takes and
`recording.add` with audio recording ([recording.md](recording.md)), so a recording of audio and MIDI tracks is one
undo step.

- **Record enable, monitor and MIDI input.** MIDI and instrument tracks have the record enable and monitor buttons
  (`channel.set` with `recordEnable` and `monitor`, in the track list and the inspector) and a MIDI input, `midiInput`:
  `all` (all MIDI inputs, the default: every input port whose `inAllInputs` is on, see `midi.setPort`) or `none`.
  While a track monitors or is record-enabled, it plays what comes from its MIDI input (MIDI thru), after what
  `midi.inject` played into it ([midi-injection.md](midi-injection.md)); injected MIDI always reaches it, as if it came
  from its input.
- **What records.** A MIDI or instrument track records while the transport records if it is record-enabled and
  enabled: everything its MIDI input plays, injected or from the ports. The tracks are those armed when recording
  starts. Input belongs where the transport plays the frame it comes at, without the record offset that audio input
  has: the instrument plays it there, and the recording puts it there (`PlayedPositions::recordedAt` at its frame).
  The acceptance test injects notes through `midi.inject`, records them, and plays them back to find each note at the
  frame it was played at, for several block sizes (`tests/midirecording_tests.cpp`).
- **Off the audio thread.** After `midi.inject`'s messages and the MIDI thru are in the sequencers' inputs, the audio
  thread hands them to the recorder's disk thread in MIDI chunks (`Recorder` in `src/engine/audio/recorder.h`), one
  per run of consecutive positions, through the same lock-free queue as the audio chunks, so the disk thread sees
  MIDI and audio in the order they happened. Chunks come from a pool of 256 allocated when the device opens, each
  with room for 256 events. A chunk goes only where input comes, where a recording starts or ends and where its
  positions jump (a cycle lap); the disk thread counts the device frames in between, so even an offline render, which
  runs far ahead of the disk thread, doesn't run out of chunks. The disk thread pairs
  each note-on with the next note-off of its pitch and channel into a note (`MidiTakeBuilder`); a note-on of a pitch
  that sounds ends it first, and notes that still sound when a cycle lap or the recording ends end there. Other
  channel messages become controller events. SysEx doesn't come from the inputs yet.
- **Takes and parts.** Each run of consecutive positions is a take, as for audio. When the recording ends, the
  recorder hands each track that recorded anything to `recording.add` with its takes, notes and controller events in
  project samples. A recording of MIDI tracks only runs `recording.add` at once; with audio tracks, the MIDI tracks
  join the audio recording's `recording.add`, which ends later by the record offset. A cycle recording mixes every
  lap into one part (the reference's Mix cycle mode, p. 352).
- **MIDI record modes** (p. 352). `transport.set` with `midiRecordMode`, which `transport.get` reports and the
  Transport menu's MIDI Record Mode sets: `newParts` (the default) puts the recording into a part of its own from the
  first take's start to the last one's end; `merge` adds it to the first unlocked part on the track that this range
  overlaps, which grows to hold it (normal sizing: its events stay where they are); `replace` does too, after it
  removed the notes that start and the controller events that lie in the range from every unlocked part it
  overlaps.
- **Preferences.** User settings: `midi.snapPartsToBars` (on by default) makes recorded parts start and end at whole
  bars (p. 1616); `midi.recordCatchRange` (ms, 0 by default) records input that came that long before recording
  started at its start (p. 1616); `midi.recordFilter` leaves message types (notes, controllers, pitch bend, channel
  and poly pressure, program changes), MIDI channels and controller numbers out of recordings, while they still play
  (MIDI Filter, Record, Channels and Controller, p. 1613).
- **Auto quantize** (p. 353). With the project's auto quantize switch (`quantize.set` with `autoQuantize`),
  `recording.add` quantizes the recorded notes with the project's quantize settings, as `quantize.apply` does, so
  `quantize.reset` brings back what was played.
- **Retrospective recording** (pp. 353-356). MIDI input that a track doesn't record, in stop mode or during playback,
  goes into its retrospective buffer on the disk thread, which keeps the last `midi.retrospectiveEvents` events (10000
  by default; 0 keeps none). `midi.insertRetrospective` inserts a track's buffer as one part through `recording.add`
  in the new parts mode: input from playback where it played, input from stop mode at the project cursor with its
  timing. Input after an insert, or in stop mode after a pause of over 30 seconds, starts the buffer afresh.
  `midi.clearRetrospective` empties one track's buffer or all of them.

## Not yet

Choosing a single MIDI input port for a track; the cycle record modes other than Mix (Overwrite, Keep Last, Stacked,
p. 352), which need lanes; Record in Editor (p. 349); Length Adjustment, Insert Reset Events after Record and the
latency compensation for live playing (p. 350, p. 1616); the MIDI thru filter and the reference's Record-Enable allows
MIDI Thru switch; retrospective recording from all MIDI inputs (p. 354) and its menu items; SysEx from the inputs.
