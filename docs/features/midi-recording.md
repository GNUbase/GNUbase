# MIDI recording

Recording MIDI onto record-enabled MIDI and instrument tracks (#97, reference manual pp. 347-355 and the MIDI and
Record - MIDI preferences, pp. 1609-1616). It shares record enable, the transport's record state, takes and
`recording.add` with audio recording ([recording.md](recording.md)), so a recording of audio and MIDI tracks is one
undo step.

- **Record enable, monitor and MIDI input.** MIDI and instrument tracks have the record enable and monitor buttons
  (`channel.set` with `recordEnable` and `monitor`, in the track list and the inspector) and a MIDI input, `midiInput`:
  `all` (all MIDI inputs, the default: every input port whose `inAllInputs` is on, see `midi.setPort`, and the
  on-screen keyboard, [onscreen-keyboard.md](onscreen-keyboard.md)) or `none`.
  While a track monitors or is record-enabled, it plays what comes from its MIDI input (MIDI thru), after what
  `midi.inject` played into it ([midi-injection.md](midi-injection.md)); injected MIDI always reaches it, as if it came
  from its input. When its MIDI thru goes off (record enable and monitor off, or `midiInput` set to `none` or
  `chordPads`), the keys and pedals held through it are released (`MidiSequencer::thru`), so that no note hangs until
  the next stop.
- **What records.** A MIDI or instrument track records while the transport records if it is record-enabled and enabled:
  everything its MIDI input plays, injected or from the ports. The tracks are those armed when recording starts. With
  the chord track record-enabled, the tracks that only monitor record too, but only into chord events
  ([chord-track.md](chord-track.md), reference manual p. 1345). Input belongs where the transport plays the frame it
  comes at, without the record offset that audio input has: the instrument plays it there, and the recording puts it
  there (`PlayedPositions::recordedAt` at its frame). The acceptance test injects notes through `midi.inject`, records
  them, and plays them back to find each note at the frame it was played at, for several block sizes
  (`tests/midirecording_tests.cpp`).
- **Off the audio thread.** After `midi.inject`'s messages and the MIDI thru are in the sequencers' inputs, the audio
  thread hands them to the recorder's disk thread in MIDI chunks (`Recorder` in `src/engine/audio/recorder.h`), one per
  run of consecutive positions, through the same lock-free queue as the audio chunks, so the disk thread sees MIDI and
  audio in the order they happened. Chunks come from a pool of 256 allocated when the device opens, each with room for
  256 events; a block with more input for a run goes on in further chunks with the same frames. Input that finds no free
  chunk is lost and counted (`Recorder::droppedMidi`). A chunk goes only where input comes, where a recording starts or
  ends and where its positions jump (a cycle lap); the disk thread counts the device frames in between, so even an
  offline render, which runs far ahead of the disk thread, doesn't run out of chunks. The disk thread pairs each note-on
  with the next note-off of its pitch and channel into a note (`MidiTakeBuilder`); a note-on of a pitch that sounds ends
  it first, and notes that still sound when a cycle lap or the recording ends end there. Other channel messages become
  controller events; with MPE input, those of the notes' channels become their per-note expression
  ([note-expression.md](note-expression.md)). SysEx doesn't come from the inputs yet.
- **Takes and parts.** Each run of consecutive positions is a take, as for audio. When the recording ends, the
  recorder hands each track that recorded anything to `recording.add` with its takes, notes and controller events in
  project samples. A recording of MIDI tracks only runs `recording.add` at once; with audio tracks, the MIDI tracks
  join the audio recording's `recording.add`, which ends later by the record offset. A cycle recording mixes every
  lap into one part by default; the MIDI cycle record modes decide otherwise ([cycle-recording.md](cycle-recording.md)).
- **MIDI record modes** (p. 352). `transport.set` with `midiRecordMode`, which `transport.get` reports and the
  Transport menu's MIDI Record Mode sets: `newParts` (the default) puts the recording into a part of its own from the
  first take's start to the last one's end; `merge` adds it to the first unlocked part on the track that this range
  overlaps, which grows to hold it (normal sizing: its events stay where they are); `replace` does too, after it
  removed the notes that start and the controller events that lie in the range from every unlocked part it
  overlaps.
- **Preferences.** User settings: `midi.snapPartsToBars` (on by default) makes recorded parts start and end at whole
  bars (p. 1616); `midi.recordCatchRange` (ms, 0 by default) records input that came that long before recording started
  at its start, unless it came before or during an earlier recording of the track, whether that one caught it or not
  (outside its catch range, left out by the record filter, with a catch range of 0, or to a track that only monitors
  while the chord track records). Input from before the audio device was set again is left out too (p. 1616);
  `midi.recordFilter` leaves message types (notes, controllers, pitch bend, channel and poly pressure, program changes),
  MIDI channels and controller numbers out of recordings, while they still play (MIDI Filter, Record, Channels and
  Controller, p. 1613).
- **Auto quantize** (p. 353). With the project's auto quantize switch (`quantize.set` with `autoQuantize`, the transport
  bar's MIDI Auto Quantize section), `recording.add` quantizes the recorded notes with the project's quantize settings,
  as `quantize.apply` does, so `quantize.reset` brings back what was played.
- **Retrospective recording** (pp. 353-356). MIDI input that a track doesn't record, in stop mode or during playback,
  also that of a track that only monitors while the chord track records (it makes chord events, no part), goes into its
  retrospective buffer on the disk thread, which keeps the last `midi.retrospectiveEvents` events (10000 by default; 0
  keeps none; then only the catch range's input is kept, for the next recording's start). `midi.insertRetrospective`
  inserts a track's buffer as one part through `recording.add` in the new parts mode, without chord events
  (`chordEvents` false): input from playback where it played, input from stop mode at the project cursor with its
  timing. Playback that jumps back ends the notes held across it, at a cycle lap at the right locator. Input after an
  insert (one that succeeded: a failed one keeps the buffer as it was) starts the buffer afresh, and so does input in
  stop mode while the buffer holds input in stop mode, when it comes over 30 seconds after the last input, from playback
  or not (p. 354). On the next audio device, the first input in stop mode leaves out only the earlier devices' input in
  stop mode, which the insert can't place against the new device's: the recorder counts the buffers' frames on across
  devices, as each device counts its own from 0, which leaves out how long the old device ran. Input during playback
  stays, before or after the device change, as the insert places it where it played (`tests/midirecording_tests.cpp`).
  `midi.insertRetrospective` with `cycle` is Insert as Cycle Recording (p. 355): only the input from playback, one take
  per cycle lap, inserted as stacked parts on lanes of their own, all but the last muted (`recording.add`'s `cycleMode`
  stacked). Laps after the first start at the left locator, and those playback left end at the right one; it needs input
  from at least two laps. A lap is where the audio thread jumped from the right to the left locator: the transport marks
  the segment that starts there (`graph::TransportSegment::lap`), `PlayedPositions::lapAt` counts the laps per device
  frame, and each input event keeps its count (`RetrospectiveEvent::lap`), so a locate back while playing, with cycle
  mode or without, starts no lap (#2067). Each start of playback after a stop is a playback pass (`PlayedLap::pass`),
  which ends the notes held where the pass before stopped (`PlayedLap::stopped`), or at the right locator if that pass
  cycled after its last input (`PlayedLap::stoppedLap`, `PlayedLap::stoppedRight`). That is the pass of the track's
  input before, also when passes without input on the track came in between, e.g. one played just to listen: the audio
  thread keeps where the last 16 passes stopped (`PlayedPositions::stops`), and the recorder gives an event that comes
  after such passes the stop of its input's own pass, or ends at that input once the pass is no longer known (#2157).
  A pass begins a lap too when it and
  the pass before played in cycle mode (`graph::TransportBlock::cycle`, `PlayedLap::cycle`), from where playback starts,
  not at the left locator, and the lap before ends where playback stopped; linear passes are one lap, as Insert as
  Cycle Recording is for input captured with cycle mode on (#2096). `midi.getRetrospective` says how many events a track's buffer holds and
  from how many cycle laps of playback (`cycleLaps`); the inspector's pop-up asks it when it opens and enables Insert as
  Cycle Recording only for two or more, as the answer to that opening says (#2096). The laps' locators are the ones each lap jumped between, which the audio
  thread keeps with it (`PlayedLap`), not the ones when the insert runs (#2096). `midi.clearRetrospective` empties one track's buffer or all of them; the Transport menu's MIDI
  Retrospective Recording > Empty All Buffers runs it for all (p. 356). The Retrospective Recording button among the
  inspector's top buttons of a MIDI or instrument track opens its pop-up menu (pp. 354-356): Insert as Linear Recording,
  Insert as Cycle Recording and Empty Retrospective Record Buffer, for that track (`tests/inspector_tests.cpp`).
- **Insert from All MIDI Inputs** (#641, p. 354). What came in at all MIDI inputs, before the tracks' routing (the
  audio engine's block of all inputs, which MIDI thru plays to the tracks), goes into a retrospective buffer of its own
  (`Recorder::process`, the buffer of channel 0), kept as the tracks' buffers are, whatever the tracks' MIDI inputs,
  monitor and record enable. `midi.insertRetrospective` with `allInputs` inserts it into the track instead of the
  track's own buffer; Empty All Buffers empties it too. Transport > MIDI Retrospective Recording > Insert from All MIDI
  Inputs runs it for the first selected MIDI or instrument track (`track.insertFromAllMidiInputs`). Tests:
  `tests/midirecording_tests.cpp`, `tests/mainmenu_tests.cpp`.

## Not yet

Choosing a single MIDI input port for a track (p. 348); Record in Editor (p. 349); Length Adjustment, Insert Reset
Events after Record and the latency compensation for live playing (p. 350, p. 1616); the MIDI thru filter and the
reference's Record-Enable allows MIDI Thru switch; SysEx from the inputs.
