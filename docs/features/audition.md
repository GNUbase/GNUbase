# Auditioning and scrubbing

Listening to one audio track without the transport (#470, reference manual pp. 252-253): the play tool's two modes
in the project window ([tools.md](tools.md)), and the commands behind them, which MCP and Lua have too. The code is
`Audition` in `src/engine/audio/audition.h`; the audio engine owns one.

## Commands

- `audition.start` with `track` and `position` (samples) plays the audio track's events from there until
  `audition.stop`, as the play tool does while the mouse is pressed. With `end` (samples, after `position`) it plays
  up to there and then nothing, as the sample editor's Play back Slice does ([hitpoints.md](hitpoints.md)); its mode
  stays `play` until `audition.stop` or another audition.
- `audition.scrub` with `track` and `position` plays the track from where its last scrub got to, to `position`,
  forwards or backwards, in `seconds` (default: the time since that scrub, from 0.01 to 0.1 seconds). So speed and
  pitch follow how fast new positions come, as the scrub tool sends them while the mouse drags. The first scrub of a
  track only sets where the next one starts. Each scrub also moves the project cursor to `position`
  (`transport.changed`), as the reference's scrubbing does.
- `audition.stop` ends either.
- **A media file on its own** (#730): `audition.start` and `audition.scrub` take `media` instead of `track`, as the
  sample editor's play and scrub tools play its clip (reference manual p. 630): `position` and `end` are then the
  file's sample frames, from 0 to its length, and it plays the file, also where no event plays it, without the
  events' volume, mute and fades. Scrubbing it leaves the project cursor alone; the sample editor moves the cursor
  itself, to where the frame plays in the project. Only the project's media files: not the files that warped events
  and frozen tracks play, which take those events' and channels' ids.
- **A clip** (#860): `clip` instead of `track` or `media` plays the clip's media file the same way, with the clip's
  fades at its start and end, as the sample editor's play and scrub tools and its Play back Slice play it. The
  result's `media` is the clip's media file.

Each returns and announces `audition.changed`: the auditioned `track` or `media` (0 for none) and the `mode`, `off`,
`play` or `scrub`. Starting one ends what ran before. They need an audio device, and an audio track, a media file
or a clip of the project.

## How it plays

- Straight to the main mix's device ports, past the track's channel: its fader, pan, mute and effects don't apply,
  as the reference has it without a control room (p. 252). A mono clip plays on every channel of the main mix, the
  other clips one to one. The control room, and its own audition level, come later.
- **Without the transport.** Auditioning never starts, stops or moves it; while the transport plays, both sound.
- A media file plays as a playlist of its own: one entry with the whole file from the project's sample 0, so that
  positions in its frames are converted to the project's sample rate, at which it plays.
- **Playing** reuses the track's playlist ([playback.md](playback.md)) with a `TrackPlayer` on a `Playback` of its
  own: the command thread cues its disk streams at the position and waits for them before the audio thread starts,
  the cue thread keeps them ahead from where the audio thread got to (or offline rendering cues them before each
  block), and the audio thread plays one block at a time from there, in the device's block size. What it plays is
  bit for bit what the track's player gives during playback.
- **Scrubbing** can't stream from disk, since it goes backwards and at any speed. The command thread reads the
  track's front layer between the two positions from its clips (each clip frame to the nearest, with the events'
  volume and fades), reads that through at the speed it takes, with linear interpolation, and hands the result to
  the audio thread, which plays it once. A scrub that comes while the last one still plays replaces it. One scrub
  reads at most 4 seconds of the track. Crossfaded layers behind the front, the reference's high quality scrub mode
  and its scrub volume are not there yet.
- Audio thread side: an `rt::SnapshotPublisher` hands it the player or the scrub's audio; it adds them to the device
  outputs after the busses, and reports where it plays through an `rt::SeqLockPair` for cueing.

## Checks

- `tests/playback_tests.cpp`: auditioning on the dummy device plays exactly the track's samples from the position,
  and up to an end, for two block sizes, with its channel turned down and muted and another track silent, then
  nothing once stopped, and the transport unchanged; the errors without a device, for a MIDI track, a negative
  position and an end before the position; scrubbing back at normal speed and forwards at twice the speed, sample by
  sample, and the cursor following; a media file played from frames outside its event, past its end and at half the
  project's sample rate, scrubbed without moving the cursor, also at half the project's sample rate, and its
  errors, also for a hand-edited project's file without a sample rate or length; a clip played and scrubbed with its
  fades.
- `tests/renderinplace_tests.cpp`: a frozen channel's freeze file is no media file to audition.
- `tests/eventediting_tests.cpp` and `tests/uiapp_tests.cpp`: the play tool on an audio lane, on a MIDI lane, and
  scrubbing ([tools.md](tools.md)).
