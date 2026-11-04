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
- **Bit-identical to an offline render.** The export renders exactly as `transport.locate` to the start,
  `transport.play` and `audio.render` would, block by block on the command thread, and after each block copies the
  output buffer of each exported node of the processing graph (`CompiledGraph::audio`), the same buffer the device's
  output gets for a bus. Cycle, pre-roll, post-roll, punch in and out and the click are off meanwhile; the transport,
  its settings and the cursor are as before afterwards. A device that runs stops while the export renders and starts
  again afterwards. The dummy device renders the blocks itself (and captures them, as with `audio.render`); for any
  other device, `AudioEngine::render` calls the audio callback itself with silent inputs, at the positions the device
  would play next.
- **Files.** `folder` is created if needed. One channel's file is `name` with the format's extension (`.wav`,
  `.aif`, `.flac`); several are `name - <channel name>`, the track's or bus's name, with characters that file systems
  refuse replaced by `_`. `conflicts` `unique` (the default) takes `name (2)`, `name (3)` and so on while a name is
  taken, by an existing file or by another file of the same export (p. 1482, Create Unique File Name); `overwrite`
  replaces existing files.
- **Formats** (pp. 1495–1500): `format` `wav`, `aiff` or `flac`, written by `AudioFileWriter`; `sampleFormat`
  `int16`, `int24` (the default), `int32`, `float32` or `float64`, the names `project.set` uses for the record bit
  depth. AIFF takes the integer ones, FLAC 16 and 24 bits. `sampleRate` converts from the project's rate with the
  same r8brain resampler as imports (`Resampler`), so a file at another rate has `Resampler::outputLength` frames.
- **Dither and normalizing.** `dither` adds TPDF dither (the built-in dither's core, `dsp::Dither`, without noise
  shaping) when writing 16 or 24 bits; its noise comes from a fixed seed, so the same export gives the same file.
  `normalize` scales each file so that its peak is that many dBFS (-60 to 0).
- **Two passes per file.** As it renders, each file is written at 32-bit float, after the sample rate conversion,
  into a part file next to it (`<file>.part`), which measures its peak and where its last audible sample is. Then
  the part file is copied into the file's format, with the gain and the dither, and removed. A failed export removes
  its part files and the files it wrote.
- **`mixdown.quickExport`** (pp. 1478–1479) writes the main mix of the whole project into a WAV file at `path`, with
  24-bit samples at the project's rate, replacing an existing file. Its range is from the start of the first audio
  event or note to the end of the last (`transport.locate`'s `projectEnd`), and it renders up to 30 seconds more for
  the tail of the effects, which ends after the last sample that a 24-bit file doesn't hold as 0 (half a step).
- Both block the command thread until they are done, like `audio.render`; nothing on the audio threads changes.

## In the UI

- **File > Export > Audio Mixdown...** (`file.exportMixdown`) opens `resources/ui/export-mixdown.rml`. Its Lua lists
  the output busses and the audio, instrument, group and effect tracks' channels as checkboxes, with the main mix
  checked; the name (the project file's name, or Mixdown), the path (the project's folder, or the home folder, with
  `Mixdown` in it, as the reference's Mixdown folder, p. 1482), the file type, sample rate (the project's), bit depth
  (24 bits), Dither, and Normalize with the peak. Export Audio sends `mixdown.export` with the checked channels over
  the locator range; the dialog closes, or stays and shows why the export failed, or that no channel is checked. The
  export holds the UI until it is done, as every command the UI's Lua sends does ([ui-lua.md](ui-lua.md)).
- **File > Export > Quick Audio Export...** (`file.quickExport`) asks for a WAV file in the OS's file dialog, adds
  `.wav` to a name without an extension, and sends `mixdown.quickExport`.

## Not yet

- MP3 and Ogg Vorbis files, which need encoders as new dependencies (pp. 1498, 1500), and 8-bit files.
- Real-time export (p. 1485), with the meters updating.
- Cycle marker and arranger ranges (p. 1480), and the Effects choices for several channels: dry, and through groups,
  sends and the master (p. 1484).
- Export as split channels, mono downmix and L/R from surround (p. 1483); Broadcast Wave, marker and iXML chunks.
- The export queue, saved with the project (pp. 1490–1494); file format presets (p. 1501); naming schemes
  (pp. 1486–1488); Always Ask for conflicts; After Export (create an audio track, insert into the pool, new project).
- A progress bar with Cancel, which needs the export as a job (`src/engine/jobs`) instead of one long command;
  Keep Dialog Open; a folder chooser and recent paths; Show in Explorer/Finder, and opening the folder after a quick
  export; channel search and syncing the selection with the mixer.
