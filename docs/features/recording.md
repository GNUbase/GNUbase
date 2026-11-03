# Audio recording

Recording audio onto record-enabled tracks (#96, reference manual pp. 335–346). It builds on how input is placed and
monitored ([latency-and-monitoring.md](latency-and-monitoring.md)); MIDI recording (#97) shares record enable, the
transport's record state and the record mode setting.

- **What records.** An audio track records while the transport records (`transport.record`, the transport bar's and
  the Transport menu's Record) if its record enable button is on (`channel.set` with `recordEnable`), the track is
  enabled and it has an input bus. It records its input bus, not its channel, so monitoring, the fader and plugins
  never change what is recorded. Each channel of the track's file records the input bus's channel of the same number;
  a bus with fewer channels gives the others its last one, so a mono input fills both channels of a stereo track. The
  tracks are those armed when recording starts; arming one during a recording takes effect with the next one.
- **Where each frame goes.** The audio thread places input as `audio.captureInput` does: an input frame belongs where
  the transport played the frame the record offset before it, and it is recorded only if the transport recorded then
  (`PlayedPositions::recordedAt`). So input still on its way when recording stops or punches out goes into the take,
  and input that comes back after a cycle jump goes to the lap it was played in. The acceptance test records a ramp
  through the dummy device's loopback, with latency, and finds it in the file at the very sample it was played
  (`tests/recording_tests.cpp`).
- **Off the audio thread.** The audio thread copies each block's input (all device input channels, interleaved) into a
  chunk taken from a pool allocated when the device opens, and hands it with its device frame and project position
  to the recorder's disk thread through lock-free queues (`Recorder` in `src/engine/audio/recorder.h`). It sends
  chunks only while some track is armed. A block that finds no free chunk is lost and counted; if a later chunk shows
  that the take just went on, the gap is written as silence, so the take keeps its timing. The disk thread polls
  every 2 ms, since the audio thread can't wake it; the pool holds over a second of input.
- **Files.** The disk thread writes one file per track, named after the track (`Audio 1.wav`, `Audio 1-2.wav` if
  that is taken; characters that file systems refuse become `_`), into the project's media folder `Audio`; an unsaved
  project records into `Recordings` in the settings folder (the temporary folder without one), and its files are
  referenced from there. The format comes from one function, `recordFormat`: for now always 32-bit float WAV at the
  project's sample rate, until the project setup has a record file type and bit depth (#78). A file is a complete
  WAV once the recording ends; after a crash the frames written so far are in it, as the reference describes (p. 347).
- **Takes.** A file holds everything recorded from the start of the recording to its end. Each run of consecutive
  project positions is a take: a cycle lap, or what was recorded between punching in and out
  (`transport.record` with `record: false` or a punch out at the right locator ends a recording, and recording again
  or a punch in at the left locator starts a new one, see [playback.md](playback.md); a cycle lap that punches out at
  the right locator and in at the left one goes on in the same file). When the recording
  ends, the engine runs `recording.add` with each track's file and takes, one undo step named Record: a media file and
  a clip per file, and an audio event per take at its position, playing its part of the file. `recording.added`
  announces the media files, clips and events; a file that can't be created or written is announced with
  `recording.failed` and its reason, and the others go on. Undo removes the events, clips and media files; the files
  stay on disk. A recording belongs to the project it was started in (`project.revision`'s `project`, which
  `recording.add` gets as `project`): if another project was opened meanwhile, nothing is added and
  `recording.failed` says so, and a track removed while it recorded is skipped with `recording.failed` while the
  other tracks' takes are added.
- **Audio record modes** (p. 345). `transport.set` with `audioRecordMode`, which `transport.get` reports and the
  Transport menu's Audio Record Mode sets, decides what happens to the unlocked events of the track that the takes
  overlap: `keepHistory` (the default) keeps them, and the newest event plays where they overlap; `cycleHistoryReplace`
  cuts them out where each take overlaps them, before adding any take, and keeps every lap; `replace` cuts out what each take overlaps,
  also of the takes before it, so of a cycle recording the last lap stays wherever laps overlap. Cutting out splits an
  event at the take's edges and removes what lies inside, as `event.split` and `event.remove` do. Locked events stay.
- **Audio pre-record** (p. 346). The user setting `audio.preRecordSeconds` (0 to 60, 0 by default) keeps that much of
  the armed tracks' input from before recording starts, while stopped or playing, on the disk thread, and puts it at
  the start of the file. The event starts where recording started, with its offset after the pre-recorded frames, so
  resizing its start to the left brings them out. Input from before a gap (a lost chunk, a new device) isn't kept.

## Not yet

Record folders per track (p. 343); the common record modes (Punch In/Out, Re-Record, starting at the left locator or
the selection, p. 339); punch points apart from the locators (p. 319); stacking cycle takes in
lanes and comping; drawing the waveform while recording (`peaks::PeakRecorder` is ready for it); the record file
type and bit depth of the project setup (#78); MIDI recording (#97); recording the output of a bus or channel (p. 345);
adjusting for plugin latency on the input (#71); moving undone recordings' files to the pool's trash.
