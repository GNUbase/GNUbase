# Project setup

The project setup holds the project's general settings (#78, reference manual pp. 128–130): its start time and length,
frame rate, time format and bar offset, sample rate, record bit depth and file type, stereo pan law and maximum volume.
The engine keeps them in the project; the dialog `resources/ui/project-setup.rml` edits them through engine commands,
so MCP and Lua scripts can do all it does.

## Engine

- **`project.set`** changes any of them, as one undo step, and `project.get` and `project.changed` have them at the top
  level: `sampleRate`, `panLaw` and `maxVolume` as before ([mixer.md](mixer.md)), and the settings of
  `ProjectSetup` (`src/engine/document/document.h`):
  - `recordFileType`: `wav` (the default), `broadcastWav`, `aiff` or `flac`, and `recordBitDepth`: `int16`, `int24`
    (the default), `int32`, `float32` or `float64`. They are what recording writes ([recording.md](recording.md)). Only combinations the audio
    file writer can write are accepted: AIFF only integers, FLAC 16 or 24 bit, 64 bit float only Wave; a change of
    the type that doesn't fit the bit depth needs both in one `project.set`.
  - `frameRate`: the timecode's, one of the reference's rates with names such as `25` (the default), `29.97`,
    `29.97dfps` and `30dfps` (`time::frameRateNames`).
  - `startTime`: the time that seconds, timecode and user frames show at the project start, in seconds (0 up to 24
    hours). `length`: how long the project is, in seconds (default 600, up to 24 hours). `barOffset`: added to the bar
    numbers shown, -9999 to 9999.
  - `timeFormat`: the primary time format (p. 69): `barsBeats` (the default), `seconds`, `timecode`, `samples` or
    `userFrames`.
- They are saved in the project file's GNUbase entry (`gnubase.json` in the DAWproject container, as `toJson(Project)`
  writes it); files from before have the defaults. Undo and redo restore them with the other project settings
  (`ProjectSettings` in `history.h`) and announce them with `project.changed`.
- **A new sample rate** keeps linear positions at their time, as before; media files keep their sample rate and play
  at the wrong speed until they are conformed with `media.conform` ([pool.md](pool.md)). The dialog does both, as
  one undo step (below).
- **The record bit depth** is in the render snapshot, so `audio.get` has it as `recordBitDepth` next to
  `projectSampleRate`, and `audio.changed` announces a new one, also after undo, for the status line's record format
  ([status-line.md](status-line.md)). The audio thread doesn't read it.

## Where the UI follows them

- **Time displays.** `ui::projectTime` and `ui::readTimeContext` read the frame rate, start time and bar offset into
  the `TimeContext`: timecode runs at the project's frame rate, `formatTime` and `parseTime` add the start time to
  seconds, timecode and user frames and the bar offset to bars, and the ruler's marks do the same
  (`src/ui/timeformat.h`). Lengths (`formatLength`, `parseLength`) count from zero. A typed position before the
  project start is no position. Samples count from the project start.
- **The event display's ruler** follows the project's time format when it changes: `project.set` (and undo and
  redo of it) makes a new time format the snap settings' format, and emits `snap.changed`; the ruler's menu picks one
  of its own meanwhile. The ruler's format is the snap settings' `format`, so a project opens with the one it was saved with
  ([event-display.md](event-display.md), [snap.md](snap.md)). Zoom Full and the overview line show the project's
  length, or up to the end of the last event if that is later.
- **The status line** shows the record format, e.g. "Record: 48 kHz - 24 bit", and a click on it opens the dialog.

## The dialog

- **Opening it.** The UI command `window.projectSetup` opens it, modal, in the window it runs in
  (`app::AppUi::openProjectSetup`): Project > Project Setup... and a click on the status line's record format (p. 71)
  run it. The manual names no key for it, so it has none. As for the studio setup dialog, there is one: running the
  command while it is open brings it to the front.
- **What it shows.** The reference's sections: Project Duration (Start, Length, as timecodes at the project's frame
  rate), Project Frame Rate, Project Time Displays (Time Format, Bar Offset), Record File Format (Sample Rate, Conform
  Audio Files, Bit Depth, Record File Type) and Other Project Settings (Stereo Pan Law, Volume Max). A value the lists
  don't have, such as a sample rate set through the engine, is added to its list.
- **OK and Cancel.** OK sends only what changed, with `project.set`, as one `history.transaction` named Project
  Setup; Cancel and Escape drop it. Timecodes are read at the frame rate chosen in the dialog; a Start or Length that
  isn't a timecode, a Bar Offset that isn't a whole number and what the engine refuses (for example FLAC at 32 bit
  float) keep the dialog open with the message in `#project-setup-message`.
- **Conform Audio Files** (on by default): with a new sample rate and media files at another rate, the dialog's
  transaction has a merge key of its own (`Project Setup <revision>`), and then it starts `media.conform`'s job with
  that key through the UI's binding (`gnubase.ui.command`), so the media files are converted to the new rate (p. 128:
  audio files must be converted to a new sample rate to play back properly) without holding up other commands. The
  pool opens and shows how the job goes, with Stop. The job's undo step joins the dialog's, so undo takes back both,
  unless something else was done before it ended. Conforming needs a saved project: without a project file and with
  a media file at another rate that isn't missing (`pool.get`), the dialog says so and nothing changes. Once the
  dialog closed, the new sample rate stays whatever the job does: if it fails or is stopped, the files keep their
  rate and can be conformed later with the pool's Conform Files; if the dialog's step is undone while it runs, the
  project is no longer at the rate the files were converted to, so the job changes nothing and removes its files,
  and a redo brings back the rate alone. Off, the files keep their rate.
- **Timecodes in Lua.** The dialog's Lua formats and reads timecodes with `gnubase.ui.formatTimecode{seconds,
  frameRate}` (returns `text`) and `gnubase.ui.parseTimecode{text, frameRate}` (returns `seconds`, or nothing for text
  that isn't a timecode), which `app::AppUi` adds to the UI's Lua next to `gnubase.ui.command`, so it converts exactly
  as the time displays do.

## Not yet

- The reference's Get Frame Rate From Video, Time Offset for the other formats, Author and Company, Hermode tuning,
  the project location with Show in Explorer, the "Run Setup on Create New Project" preference, and sample rates the
  audio device doesn't support shown in another color.
- The transport's primary time display still keeps its format in the user setting `ui.timeFormats`; that it and the
  project's time format set each other (p. 69) is a follow-up.

## Tests

`tests/projectsetup_tests.cpp`: the settings are set, announced, read by the audio engine, undone and saved and loaded
with the project; combinations recording can't write and values out of range are refused; the time displays follow the
frame rate, start time and bar offset. In the app, the dialog shows the defaults and OK sends a changed bit depth,
frame rate, start, length, bar offset, pan law and maximum volume as one undo step; Cancel drops a change and an
unreadable length keeps it open with the message; a new sample rate keeps the dialog open on an unsaved project
with media, and conforms a saved project's audio file with the pool's job, which the pool shows, undone in one step,
and doesn't with Conform Audio Files off; an unsaved project whose only file at another rate is missing, the others at the new rate, takes the new rate without asking for a save; the status line shows a new record bit depth, follows undo and opens
the dialog with a click. `tests/eventdisplay_tests.cpp` checks Zoom Full and the overview line with the project's length.
