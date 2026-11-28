# Audio mixdown export

Exporting what busses and channels play into audio files (#115, reference manual pp. 1478–1502): the Export Audio
Mixdown dialog and Quick Audio Export. The offline rendering it builds on is `audio.render`
([architecture.md](../architecture.md#audio-processing)).

## In the engine

`src/engine/audio/mixdown.cpp` registers both commands on the audio engine.

- **`mixdown.export`** renders a range of the project, in project samples (by default from the left to the right
  locator), and writes a file per bus or channel in `channels`: output busses, and audio, instrument, group and effect
  channels after their fader and panner, so a mono track panned into a stereo output gives a stereo file (the
  reference's "Inserts and Strip", p. 1484). By default it exports the main mix. A MIDI channel has no audio and is
  refused, as are channels named twice.
- **Background rendering.** `mixdown.export` returns a cancellable job immediately, using the existing jobs machinery
  ([backup.md](backup.md)). It copies the starting render snapshot, render settings, locator range and target names on
  the command thread. Its job owns a separate audio engine, disk streams, sampler instruments and plugin instances,
  sharing only immutable sample reads; the live device, transport and graph keep running and other clients can send
  commands throughout. No tracks are locked for export: subsequent project edits never change the job's starting
  snapshot. The private graph renders block by block as `audio.render` does, with cycle, pre-roll, post-roll, punch and
  click disabled, and its plugins in offline mode. Its effects start without the echoes and reverb that playback left in
  the live ones, and no external MIDI input reaches it, as with the reference's Deactivate External MIDI Inputs
  (p. 1486) always on. `job.progress` reports rendering and file conversion, and `job.finished` reports completion,
  failure or cancellation. Cancellation and failure remove the files made by the job.
- **What a private render keeps.** Its snapshot is a copy of the starting one with only the selected channels or
  busses and their upstream audio and MIDI dependencies, so a field added to `RenderSnapshot` later is kept as well.
  Routing indices are remapped within that snapshot; unrelated plugins and sampler reads are left out. A slot that was
  active in the live engine must become active in the private renderer too, or the job fails rather than silently
  exporting without that plugin; so must one still loading live, which is about to play. The private plugins are made
  in offline mode, so that their own `setProcessMode` is offline before they are activated, and the live plugins never
  leave real-time mode for an export.
- **Render points.** `point` renders channels from inside their strip (before the slots, after them, before the
  fader, or after the panner without the fader) instead of after the fader and panner, and `tail` adds up to 60 s that end where the audio does, for render
  in place and freeze ([render-in-place.md](render-in-place.md)).
- **The device's sample rate.** The processing graph runs at the device's rate and audio events play at the
  project's, so both export commands refuse while the device runs at another rate than the project.
- **Files.** `folder` is created if needed, once the command has checked the device, the recording, the sample rate
  and the channels, so a refusal leaves no new folder. One channel's file is `name` with the format's extension (`.wav`,
  `.aif`, `.flac`); several are `name - <channel name>`, the track's or bus's name, with characters that file systems
  refuse replaced by `_`. `conflicts` `unique` (the default) takes `name (2)`, `name (3)` and so on while a name is
  taken, by an existing file or by another file of the same export (p. 1482, Create Unique File Name); `overwrite`
  replaces existing files.
- **Formats** (pp. 1495–1500): `format` `wav`, `aiff` or `flac`, written by `AudioFileWriter`; `sampleFormat`
  `int16`, `int24` (the default), `int32`, `float32` or `float64`, the names `project.set` uses for the record bit
  depth. AIFF takes the integer ones, FLAC 16 and 24 bits. `sampleRate` converts from the project's rate with the
  same r8brain resampler as imports (`Resampler`), so a file at another rate has `Resampler::outputLength` frames.
- **Broadcast Wave chunk** (#580, p. 1615). `broadcastWave` gives WAV files a bext chunk. Its `description`, `author`
  and `reference` default to the Broadcast Wave strings of the preferences (`audio.broadcastDescription`,
  `audio.broadcastAuthor`, `audio.broadcastReference`, [recording.md](recording.md)) and are cut to their fields' 256,
  32 and 32 bytes between two UTF-8 characters (`broadcastNow`, shared with recording). The origination date and time
  are the export's, and the time reference is where the file starts plus the project's start time, in samples of the
  file's rate since midnight. Other formats refuse it. Tests: `tests/mixdown_tests.cpp`.
- **Dither and normalizing.** `dither` adds TPDF dither (the built-in dither's core, `dsp::Dither`, without noise
  shaping) when writing 16 or 24 bits; its noise comes from a fixed seed, so the same export gives the same file.
  `normalize` scales each file so that its peak is that many dBFS (-60 to 0).
- **Two passes per file.** As it renders, each file is written at 32-bit float, after the sample rate conversion, into a
  part file next to it (`<file>.part`), which measures its peak and where its last audible sample is. Then the part file
  is copied into the file's format, with the gain and the dither, and removed. A failed export removes its part files
  and the files it wrote. Converted outputs are staged until every file is complete; cancelling an overwrite export
  keeps the existing destination files.
- **`mixdown.quickExport`** (pp. 1478–1479) writes the main mix of the whole project into a WAV file at `path`, with
  24-bit samples at the project's rate, replacing an existing file. Its range is from the start of the first audio event
  or note to the end of the last (`transport.locate`'s `projectEnd`), and it renders up to 30 seconds more for the tail
  of the effects, which ends after the last sample that a 24-bit file doesn't hold as 0 (half a step). It runs
  `mixdown.export`'s private render as a job of its own (command version 2): it returns `{job}` at once, and the job's
  `result` is the file's absolute `path` and `frames`. The live graph, plugins and transport, playing or not, are left
  as they are. It is refused as `mixdown.export` is, and its job fails if a plugin that plays or still loads live can't
  be made for the render; the error names the plugin and its channel. All 30 seconds are rendered: the processors'
  `tail()` could end it sooner, but instruments and samplers don't report their release in it.

## In the UI

- **File > Export > Audio Mixdown...** (`file.exportMixdown`) opens `resources/ui/export-mixdown.rml`. Its Lua lists the
  output busses and the audio, instrument, group and effect tracks' channels as checkboxes, with the main mix checked;
  the name (the project file's name, or Mixdown), the path (the project's folder, or the home folder, with `Mixdown` in
  it, as the reference's Mixdown folder, p. 1482), the file type, sample rate (the project's), bit depth (24 bits),
  Dither (only for 16 and 24 bits, which the engine dithers), and Normalize with the peak. Export Audio sends
  `mixdown.export` with the checked channels over the locator range; the dialog closes, or stays and shows why the
  export failed, or that no channel is checked. The render progress appears in a separate, nonmodal dialog with Cancel;
  editing stays available.
- **File > Export > Quick Audio Export...** (`file.quickExport`) asks for a WAV file in the OS's file dialog, adds
  `.wav` to a name without an extension, and sends `mixdown.quickExport`. Its render progress appears as the Export
  Audio Mixdown's does, with Cancel. A refused or failed export shows why in the project error dialog
  (`project-error.rml`); a finished one opens the file's folder with the OS, as the reference does (p. 1479).

## Not yet

- MP3 and Ogg Vorbis files, which need encoders as new dependencies (pp. 1498, 1500), and 8-bit files.
- Real-time export (p. 1485), with the meters updating.
- Cycle marker and arranger ranges (p. 1480), and the Effects choices for several channels: dry, and through groups,
  sends and the master (p. 1484).
- Export as split channels, mono downmix and L/R from surround (p. 1483); marker and iXML chunks; the Broadcast Wave
  chunk in the export dialog.
- The export queue, saved with the project (pp. 1490–1494); file format presets (p. 1501); naming schemes
  (pp. 1486–1488); Always Ask for conflicts; After Export (create an audio track, insert into the pool, new project).
- Keep Dialog Open; a folder chooser and recent paths; Show in Explorer/Finder; channel search and syncing the
  selection with the mixer.
- AIFF files with 32- and 64-bit float samples (p. 1497).
- Rendering from before the range, so that the effects of events before it are heard in it (p. 1490).

Empty output names are refused by both `mixdown.export` and `track.renderInPlace`. The export dialog asks for a name
before sending an empty Name field. Command tests cover both refusals, and the dialog test keeps the dialog open until a
name is entered.

## API result and tests

`mixdown.export` is command version 2: it keeps its name and parameters, and returns `{job}` instead of `{files}`. The
job's completed `result` has the same `{files}` shape as the former synchronous result. `mixdown.quickExport` is version
2 as well: its job's `result` is the `{path, frames}` it returned before. Lua and MCP callers use `job.get` or
`job.finished` to obtain it, and `job.cancel` to stop the job.

`tests/mixdown_tests.cpp` checks formats, render points, naming, conversion and cleanup through the job result, and that
MIDI played into the live engine with `midi.inject` stays out of an export. `tests/renderinplace_tests.cpp` also checks
that a second client can edit and remove an exported track while the job is held, without changing its audio, and that
exports take no track locks. `tests/mixdown_ui_tests.cpp` drives the real export dialog, the render progress and Cancel
through the automation API.

`tests/renderinplace_tests.cpp` checks that a group export instantiates its upstream plugin without unrelated slots,
that a private plugin failure fails freeze and render in place without changing history or leaving tracks locked, that
an export with a live plugin still loading fails without its private one, naming the plugin and its channel, that the
private plugins are activated offline and process only offline blocks, and that a held render refuses changes of the
root key, pan law, tempo and sample rate.
