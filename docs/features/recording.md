# Audio recording

Recording audio onto record-enabled tracks (#96, reference manual pp. 335–346). It builds on how input is placed and
monitored ([latency-and-monitoring.md](latency-and-monitoring.md)); MIDI recording ([midi-recording.md](midi-recording.md)) shares record enable, the
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
  every 2 ms while a track is armed or records, since the audio thread can't wake it; the pool holds over a second of
  input. Otherwise only MIDI input for the retrospective buffers comes, and it polls every 50 ms (a new setup wakes
  it); without a device it sleeps until one opens (#531). An offline render (`audio.render`) makes the blocks much
  faster than a device: before each block it waits for the disk thread once half the pool is in use
  (`Recorder::waitForChunks`), so a recording made offline of any length loses no input. What the next recording
  records and where (`Recorder::setup`) is numbered when the engine hands it over, not applied: each chunk carries the
  latest number, and the disk thread applies a setup when a chunk sent after it comes, or when it finds none. So a
  recording that the audio thread started before keeps the setup it started with, and handing a setup over never
  waits for the disk thread. The disk thread lists the folder and creates a recording's files without its lock, so
  `midi.insertRetrospective` and `midi.clearRetrospective` don't wait for that either; the chunks behind the one
  that started the recording wait meanwhile, and so do a device change and the engine's end. Those two commands
  handle the chunks that came in only up to one that starts a recording, which they leave to the disk thread
  (#1356).
- **Files.** The disk thread writes one file per track, named after the track (`Audio 1.wav`, `Audio 1-2.wav` if that is
  taken; characters that file systems refuse become `_`, and `_` follows a name that Windows reserves for a device,
  whatever its case and extension: `CON_.wav`, `lpt1_.take.wav`), into the project's media folder `Audio`; an unsaved
  project records into `Recordings` in the settings folder (the temporary folder without one; `unsavedMediaFolder`,
  where its imported MP3 and Ogg Vorbis files are converted too, [file-import.md](file-import.md)), and its files are
  referenced from there until it is saved: `project.save` then copies them into its media folder as a job and points the
  media at the copies as an undo step of its own (#611), as it does with the files the project rendered, flattened and
  froze meanwhile. One save's copies at a time: a save while they run starts none, and a project saved into that folder
  itself copies nothing. Without a settings and a temporary folder, or when the folder can't be made, the recording
  fails with `recording.failed` once and writes no file. A name that differs only in case from a file in the folder, or
  from another track's, counts as taken on every file system, also on those that tell them apart, so that the files can
  be copied to one that doesn't (`createFreeFile`). One listing of the folder when a recording starts finds the free
  names of all tracks; a file made after it is still found when the file is created, and a file whose name can't be read
  as text (an invalid UTF-16 name on Windows) doesn't count. The format comes from one function, `recordFormat`: the
  project setup's record file type and bit depth ([project-setup.md](project-setup.md), p. 342) at the project's sample
  rate, read when a recording starts. Wave and Broadcast Wave files end in `.wav`, AIFF in `.aif`, FLAC in `.flac`. A
  Broadcast Wave file has a bext chunk with the Broadcast Wave strings of the preferences (p. 1615): the description
  `audio.broadcastDescription`, or the track name while it is empty, the author `audio.broadcastAuthor` (GNUbase by
  default) as its originator and the reference `audio.broadcastReference` as its originator reference, each cut to its
  field's 256 or 32 bytes between two UTF-8 characters; then the local date and time the recording started, and as its time reference
  the position of the file's first frame (pre-recorded input included) plus the project's start time, in samples since
  midnight. The time reference doesn't wrap at midnight: a file that starts past 24 h counts on, as the 64-bit field
  allows, and a pre-record that reaches back before 0:00:00 gives 0. Wave and AIFF files carry the same three strings
  as text (p. 1615: "not only Broadcast Wave files"), the description again the track name while it's empty: a Wave
  file in its LIST INFO chunk as ICMT, IART and ISRC, an AIFF file as its NAME, AUTH and ANNO chunks (`FileText`);
  an empty string is left out. File names are cut to 200 bytes in the same way, so
  a long track name still fits the file systems' 255. A file is complete once the recording ends; after a crash the
  frames written so far are in it, as the reference describes (p. 347): until a WAV or AIFF file is closed, its RIFF or
  FORM size and its data or SSND size are 0xFFFFFFFF, which readers take as "up to the end of the file", and a FLAC
  file's STREAMINFO has no sample count, for which the FLAC reader counts the frames that decode. A writer opens its
  file for others to read along. A Wave file larger than 4 GB (p. 1615) becomes RF64 when it is closed, or with the
  preference `audio.waveOver4GB` `split` (Record > Audio, When Recording Wave Files Larger than 4GB: Split Files)
  goes on in a new file, named as a second file of the track (`Audio 1-2.wav`), which FAT32 needs. An AIFF file can't
  be RF64, so it is always split at 4 GB. The writer refuses the write that would pass 4 GB with `AudioFileFull`, less
  the pad byte that closing adds to a file of odd size, and the recorder writes that block into the next file; a next
  file that can't be created or written is removed again (#2096). Each file lands with the parts of the takes in it, as
  events of their own, so a take that the split cuts becomes two events that meet (#580); the piece in the next file
  says so with `continues`. `recording.add` takes a track's files as one recording: the record mode's cuts and the
  takes' lanes cover the takes of all of them, the two pieces of a cut cycle lap share a lane, and the clips' regions
  are numbered by lap across the files (#2102). A Broadcast Wave file's time reference counts on from the file before.
  A write that fails otherwise ends the file there. The recording fails
  for it once (`recording.failed`), the other tracks record on, and the file still lands, with its takes ending
  where the file ends (#580). Tests: `tests/audiofile_tests.cpp` (a file that was never closed),
  `tests/recording_tests.cpp` (formats, the Broadcast Wave chunk and the time reference, the text of Wave and AIFF
  files, a full file that goes on in the next one, `audio.waveOver4GB` `split`, the lanes and regions of a cut lap;
  `detail::setFileSizeLimit` lowers the 4 GB for them).
- **Takes.** A file holds everything recorded from the start of the recording to its end. Each run of consecutive
  project positions is a take: a cycle lap, or what was recorded between punching in and out (`transport.record` with
  `record: false` or a punch out at the right locator ends a recording, and recording again or a punch in at the left
  locator starts a new one, see [playback.md](playback.md); a cycle lap that punches out at the right locator and in at
  the left one goes on in the same file). When the recording ends, the engine runs `recording.add` with each track's
  file and takes, one undo step named Record: a media file and a clip per file, and an audio event per take at its
  position, playing its part of the file. `recording.added` announces the media files, clips and events; a file that
  can't be created or written is announced with `recording.failed` and its reason, and the others go on. Undo removes
  the events, clips and media files; the files stay on disk. The takes come after `transport.stop` or a punch out has
  returned, once the input still on its way has come in: a client waits for `recording.added` before `project.save`, or
  the project is saved without them, and `project.load`, `project.new` or `project.revert` before it drop them (#546,
  the command descriptions say so). `recording.added` follows unless no file could be written, which only
  `recording.failed` reports. A recording that adds nothing, because its project is no longer open or all its tracks are
  gone, leaves the project unmodified, without an undo step. A recording belongs to the project it was started in
  (`project.revision`'s `project`, which `recording.add` gets as `project`): if another project was opened meanwhile,
  nothing is added and `recording.failed` says so, and a track removed while it recorded is skipped with
  `recording.failed` while the other tracks' takes are added.
- **Audio record modes** (p. 345). `transport.set` with `audioRecordMode`, which `transport.get` reports and the
  Transport menu's Audio Record Mode sets, decides what happens to the unlocked events of the track that the takes
  overlap: `keepHistory` (the default) keeps them, and the newest event plays where they overlap; `cycleHistoryReplace`
  cuts them out where each take overlaps them, before adding any take, and keeps every lap; `replace` cuts out what each take overlaps,
  also of the takes before it, so of a cycle recording the last lap stays wherever laps overlap. Cutting out splits an
  event at the take's edges and removes what lies inside, as `event.split` and `event.remove` do. Locked events stay.
- **Audio pre-record** (p. 346). The user setting `audio.preRecordSeconds` (0 to 60, 0 by default) keeps that much of
  the armed tracks' input from before recording starts, while stopped or playing, on the disk thread, and puts it at the
  start of the file. It keeps only the device inputs that the armed tracks record, so its memory grows with them, not
  with the device's inputs, and arming another track starts it afresh. If it can't be allocated, there is no pre-record
  and `recording.failed` says so; recording works as without it (#531). The event starts where recording started, with
  its offset after the pre-recorded frames, so resizing its start to the left brings them out. Input from before a gap
  (a lost chunk, a new device) isn't kept.
- **Lock Record** (#104, p. 356) keeps a recording from ending by accident. `transport.set` with `lockRecord`, which
  the Transport menu's Lock Record and Unlock Record set, turns it on and off; it stays on for later recordings until
  it is turned off. The key commands editor lists both in the transport area without default keys, as the reference
  does (#645). While it is on and the transport records, `transport.stop`, `transport.toggle` and `transport.record`
  refuse to stop or punch out unless they get `confirmed`, and their reason says which they refused; playback that
  doesn't record stops as usual. The check reads the audio thread's state (`Transport::recordingNow`), so a stop just
  after the punch in at the left locator, before the command thread has taken it over, is refused too. The audio
  thread ignores the automatic punch out at the right locator, so with `audio.stopAfterPunchOut` playback doesn't stop
  there either. In the UI, a stop, Start/Stop or Record that the engine refused this way opens
  `resources/ui/lock-record.rml` (`app::AppUi` hears the refusal with `EngineBinding::onResult` and checks
  `transport.get`) in the window whose key, button or menu item sent the command (`KeyCommands::sentFrom`, kept by
  request until its result is in, #645, #2096), so a refusal in a mixer or editor window of its own asks there, and in
  the main window for a command from elsewhere, such as UI Lua; it asks to confirm what was refused: Stop sends
  `transport.stop` with `confirmed`, Punch Out `transport.record` with `record` false and `confirmed`, so playback goes
  on; Cancel goes on recording. Tests: `tests/transport_tests.cpp` (the punch out ignored for any block size, the
  commands, a stop right after the punch in) and `tests/lockrecord_tests.cpp` (the menu and the dialog in the app's UI).
- **Remaining record time** (#104, p. 356). `recording.remainingTime` divides the space available on the disk of the
  record folder (the nearest folder above it that exists, while it doesn't yet) by what the tracks that would record
  write a second: the project's sample rate times the record bit depth's bytes times their channels, uncompressed,
  so a FLAC recording lasts longer. It is null while no track would record (record-enabled, enabled, with an input
  bus). The disk is measured on a thread of its own (`DiskSpace`, #645), so a slow or unreachable record folder, such
  as one on a network drive, doesn't hold up the engine's commands: the command starts a measurement and answers
  with the folder's last one at once; only while the folder has none yet does it wait a quarter of a second for it,
  and `seconds` and `availableBytes` are null while there is none. It starts no second measurement while one runs. Tests: `tests/statusline_tests.cpp`. The
  status line shows it, and the Max. Record Time window ([status-line.md](status-line.md)).

## Not yet

Record folders per track (p. 343); the common record modes (Punch In/Out, Re-Record, starting at the left locator or
the selection, p. 339); punch points apart from the locators (p. 319); drawing the waveform while recording (`peaks::PeakRecorder` is ready for it); recording the output of a bus or channel (p. 345);
adjusting for plugin latency on the input (#71); moving undone recordings' files to the pool's trash; with record
folders per track, the remaining record time of the fullest disk (p. 356).
