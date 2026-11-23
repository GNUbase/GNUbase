# Audio functions

Analyzing the audio of events (#147, reference manual pp. 603-609): Detect Silence, the Spectrum Analyzer and
Statistics.

## In the engine

The measurements are in `src/engine/analysis` ([analysis.md](analysis.md)), the commands in
`src/engine/document/audiofunctions.cpp`. The analysis commands return a job immediately (version 2 for the original
four commands, version 1 for `clip.estimatePitch`): the source media and range are copied on the command thread, and the
job reads and measures the file in blocks. `job.progress` reports progress, `job.cancel` cancels between blocks, and
`job.finished` and `job.get` contain the measurements in `job.result`. File errors fail the job. Positions are clip
sample frames; `start` and `end` default to the whole clip. Read-only analysis results describe the captured source even
if the document changes meanwhile.

| Command | What it does | Reference |
|---|---|---|
| `clip.detectSilence` | Analyze: the sections of `clip` from `start` to `end` that aren't silent, by start | pp. 603-606 |
| `event.stripSilence` | A job analyzes audio `events`, then applies Strip Silence, Add as Regions and Apply Fades in one edit | pp. 603-606 |
| `event.applySilenceSections` | Applies measured `sections`, one array per event, only at the given project `revision`; no file I/O | |
| `clip.spectrum` | The average spectrum from `start` to `end`, per channel | pp. 607-608 |
| `clip.statistics` | The statistics from `start` to `end`, per channel, and its loudness | p. 609 |
| `clip.estimatePitch` | A monophonic fundamental in Hz per channel, or null without a reliable period | p. 609 |

- **Detect Silence** (`analysis::SilenceDetector`). A section opens at the first frame where a sample of any channel
  reaches the open threshold and closes after the last frame that reaches the close threshold (at most the open one),
  once 20 ms have stayed below it, so that a tone's zero crossings don't close it. Once open, a section stays open
  for at least Minimum Time Open, joining what sounds within it; once closed, none opens before Minimum Time Closed
  has passed. Pre-roll and Post-roll widen the sections, which join where they then overlap. Thresholds are dBFS (-120 to 0, defaults -20 and -30), times milliseconds
  (defaults 10, 100, 0 and 0).
- **Process** (`event.stripSilence`, one undo step, "Detect silence"): `regions` adds a region of the clip per section
  ([sample-editor.md](sample-editor.md)), named `regionName` and a number counting from `autoNumberStart` across the
  events; `strip` (the default) splits each event at the sections' edges (`event.split`) and removes the silent parts
  (`event.remove`); `fades` gives the events that remain a fade in and out of that many milliseconds, at most half of
  what each plays for in frames at the clip's sample rate (its length, unless it plays warped: `fadeRoom`,
  [warp.md](warp.md)) each (`audioEvent.setFades`). The job posts `event.applySilenceSections` to the command thread
  with its measured sections and the project's starting revision. That edit runs those commands as part of it
  (`ProjectModel::runEdit`). An edit, undo, redo or project replacement during analysis fails the job rather than
  applying stale sections. Stripping a silent or locked event is an error. `event.stripSilence` cannot be part of a
  history transaction; the apply command can.
- **Spectrum** (`analysis::PowerSpectrum`, `analysis::spectrumCurve`): Welch's average power spectrum, as
  `audio.analyze` measures it, at points 1/24 octave apart from 20 Hz up to the Nyquist frequency. Each point has the
  level of the strongest bin within 1/48 octave of it, or between the bins next to it where they are farther apart,
  scaled by the window's noise bandwidth so that a sine reads its peak level: a full-scale sine 0 dBFS, up to 0.8 dB
  less between bins.
- **Statistics** (`analysis::StatisticsMeter`): per channel the lowest and highest sample, the peak, the DC offset (the
  mean, also in percent), the bit depth (the fewest bits b for which every sample times 2^(b-1) is whole, 32 for
  floating point beyond 24), the estimated fundamental pitch, the RMS of every sample and the highest RMS of 50 ms
  windows 10 ms apart, both as AES17 has them (a full-scale sine is 0 dB). Loudness and true peak come from the loudness
  meter ([metering.md](metering.md)), which the audio is fed through offline: maximum momentary and short-term loudness,
  integrated loudness, loudness range and the true peak per channel, 4 times oversampled, of up to 8 channels; channels
  beyond have their sample peak as true peak. Loudness uses the default [channel format](channel-formats.md) for the
  file's channel count: front speakers weigh 1, surrounds 1.41 and LFE 0. LFE still contributes to true peak and its
  channel statistics. Unknown channel counts keep unit weights for the first eight channels; files do not yet report a
  speaker mask.
- **Pitch** (`analysis::PitchEstimator`, also used by Statistics): scans each channel in fixed memory, averaging samples
  down to about 12 kHz and keeping the loudest 2048-sample window by its energy without DC. Cumulative mean normalized
  squared differences identify the first periodic minimum; interpolation estimates its frequency, from 40 to 2000 Hz.
  Louder overtones can therefore retain their fundamental. Silence, DC, nonperiodic audio and windows shorter than 50 ms
  return null. This estimates a single note, not polyphonic transcription. The job reports `clip`, `frames` and
  `channels`, each with its zero-based `channel` and nullable `pitch`; it supports the usual clip range and
  cancellation.

## In the UI

The Audio menu has Advanced > Detect Silence..., Spectrum Analyzer and Statistics, the UI commands
`audio.detectSilence`, `audio.spectrumAnalyzer` and `audio.statistics` of the event display. They open separate,
nonmodal windows for the selected audio events; each function has one window, and reopening it raises the existing
window with its captured selection. Their Lua works through the engine API. Closing the document also closes its native
window; replacing the project closes all three. Each window follows its job's progress without waiting on the UI thread,
shows failures, and cancels pending work when closed. A new Analyze or Auto request cancels the previous job and ignores
its later results. Statistics analyzes selected events successively, showing each table as it finishes.

```
#detect-silence-dialog          detect-silence.rml
  #ds-event                     the event's name, and "(1 of 2)" with more than one
  #ds-display.ds-display        a .ds-section per section along the event
  valuefield#ds-open-threshold, #ds-close-threshold, #ds-minimum-open, #ds-minimum-closed, #ds-pre-roll, #ds-post-roll
  #ds-linked                    Linked: the thresholds stay the same
  #ds-sections                  Number of Sections
  #ds-add-regions, #ds-region-name (the clip's name), #ds-auto-number, #ds-strip, #ds-process-all (disabled for one
  event), #ds-apply-fades with #ds-fade-length (ms)
  #ds-analyze, #ds-auto (Auto: every change analyzes), #ds-cancel, #ds-process, #ds-message (why it failed)
#spectrum-analyzer-dialog       spectrum-analyzer.rml
  select#sa-channel             the channel shown, or All Channels (separate curves in theme palette colors)
  slider#sa-zoom                horizontal zoom from 1 to 8; #sa-view scrolls the enlarged graph
  #sa-value                     channel, frequency, note and level under the mouse, and D, the difference to the
                                reference; clip.spectrum's error for a clip whose file can't be read, as #ds-message
                                and the Statistics tables show theirs
  #sa-graph.sa-graph            a .sa-bar[index] per point from -120 dB at the bottom to 0 dB at the top;
                                .reference for the one right-clicked, until a click
  #sa-close
#statistics-dialog              statistics.rml
  #st-tables                    per event its name (.st-name), then .st-row rows of .st-label and a .st-value per
                                channel: Channel (.st-header), Min. and Max. Sample Value, Peak Amplitude, True Peak,
                                DC Offset, Bit Depth, Estimated Pitch, Sample Rate, Average RMS (AES17), Max. RMS;
                                Max. RMS All Channels; Loudness (EBU R 128)
  #st-close
```

Process with Process All Selected Events processes every event that is left at once; without it, the next event
shows with the same settings, as in the reference (p. 606). Notes count C3 as middle C, as the MIDI editors do.
The themes color the sections, the graph and its bars (`tools/skin/generate.py`).

Detect Silence draws each channel's waveform from `media.peaks`, updating when its peaks become ready. The squares at
the left and right adjust Open Threshold and Close Threshold; their heights show the amplitude in the first channel.
Linked and Auto apply to these drags too. Pressing the waveform previews the clip from that frame until release, also
when released outside the waveform; closing the window stops its preview. Preview sets the level from zero to unity.
Mute Gaps plays only the measured sections, leaving the gaps silent without skipping time (`audition.start` with
`sections`, [audition.md](audition.md)). Changing detection settings clears the measured sections and cancels pending
analysis, so Mute Gaps requires Analyze again, or Auto.

The sample editor's Process section opens all three functions for its event and captures its range, or the event's range
without a selection, independently of the project selection. Detect Silence's Process passes `start` and `end` clip
frames to `event.stripSilence`: both must be supplied together and lie within the event. Audio outside that range stays;
regions use only the measured sections. With Strip Silence, new fades apply only to pieces inside the range.
`event.applySilenceSections` accepts the same bounds, with measured sections inside them. Analysis and preview can also
use clip ranges outside the event.

## Not yet

The Detect Silence window's zoom; entry points from the pool; polyphonic pitch estimation; speaker masks from files for
Statistics' surround weighting.

## Checks

- `tests/audiofunctions_tests.cpp`: Strip Silence within a range preserving both outside pieces, adding only the
  measured region, undo and invalid bounds.
- `tests/audiofunctions_ui_tests.cpp`: waveform peaks, threshold dragging, Mute Gaps preview and stopping on close; the
  sample editor's three analysis entries using its range independently of the project selection.

- `tests/audiofunctions_ui_tests.cpp`: separate nonmodal windows, reopening an existing function, closing through the
  window system, and project replacement closing every function; held jobs cancel when their native windows close.

- `tests/audiofunctions_tests.cpp`: Detect Silence on tones whose edges are known, with both thresholds, the minimum
  times, pre-roll and post-roll; `clip.detectSilence` in a range and its errors; `event.stripSilence` splitting,
  regions, fades, one undo step, regions alone and its errors; the statistics of reference signals (a sine's levels, RMS
  and pitch, the true peak of a sine at a quarter of the sample rate, DC offset, 16 and 8 bit samples, the highest RMS,
  EBU Tech 3341 and 3342's loudness signals, silence and a range); the spectrum of sines in two channels and of silence;
  all five jobs held at their first progress to check command responsiveness, cancellation, progress and completion;
  Strip Silence refusing results after an edit, undo, event removal or project replacement; missing files and invalid
  measured sections; surround speaker weights and LFE loudness exclusion with true peak retained; fundamental pitch
  beneath louder harmonics, independent stereo pitches, range selection, silence, DC, noise and short audio.
- `tests/audiofunctions_ui_tests.cpp` drives the dialogs with `ui::Automation`: Analyze and its sections along the
  event, Add as Regions and Process for the first event, Auto, Linked and the open threshold for the second; Cancel, and
  Process All Selected Events with Apply Fades; the Statistics window's values; the Spectrum Analyzer's bars, the values
  under the mouse, the reference of a right click and D, a click dropping it, the error of a clip whose file can't be
  read, and the channel of a stereo clip that its selector chooses; all three dialogs remaining responsive during held
  analysis jobs and cancelling pending analysis or processing when closed; All Channels with a reference across two
  curves, and zoom preserving that reference.
