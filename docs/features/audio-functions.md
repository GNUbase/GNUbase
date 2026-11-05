# Audio functions

Analyzing the audio of events (#147, reference manual pp. 603-609): Detect Silence, the Spectrum Analyzer and
Statistics.

## In the engine

The measurements are in `src/engine/analysis` ([analysis.md](analysis.md)), the commands in
`src/engine/document/audiofunctions.cpp`. They read the clip's file in blocks on the command thread. Positions are the
clip's sample frames; `start` and `end` default to the whole clip.

| Command | What it does | Reference |
|---|---|---|
| `clip.detectSilence` | Analyze: the sections of `clip` from `start` to `end` that aren't silent, by start | pp. 603-606 |
| `event.stripSilence` | Process for audio `events`: the same in the section each plays, then Strip Silence, Add as Regions and Apply Fades | pp. 603-606 |
| `clip.spectrum` | The average spectrum from `start` to `end`, per channel | pp. 607-608 |
| `clip.statistics` | The statistics from `start` to `end`, per channel, and its loudness | p. 609 |

- **Detect Silence** (`analysis::SilenceDetector`). A section opens at the first frame where a sample of any channel
  reaches the open threshold and closes after the last frame that reaches the close threshold (at most the open one),
  once 20 ms have stayed below it, so that a tone's zero crossings don't close it. Silences shorter than Minimum Time
  Closed between sections are kept, sections shorter than Minimum Time Open dropped; Pre-roll and Post-roll widen the
  rest, which join where they then overlap. Thresholds are dBFS (-120 to 0, defaults -20 and -30), times milliseconds
  (defaults 10, 100, 0 and 0).
- **Process** (`event.stripSilence`, one undo step, "Detect silence"): `regions` adds a region of the clip per section
  ([sample-editor.md](sample-editor.md)), named `regionName` and a number counting from `autoNumberStart` across the
  events; `strip` (the default) splits each event at the sections' edges (`event.split`) and removes the silent parts
  (`event.remove`); `fades` gives the events that remain a fade in and out of that many milliseconds, at most half
  their length each (`audioEvent.setFades`). It runs those commands as part of it (`ProjectModel::runEdit`). Stripping
  an event that is silent or locked is an error.
- **Spectrum** (`analysis::PowerSpectrum`, `analysis::spectrumCurve`): Welch's average power spectrum, as
  `audio.analyze` measures it, at points 1/24 octave apart from 20 Hz up to the Nyquist frequency. Each point has the
  level of the strongest bin within 1/48 octave of it, or between the bins next to it where they are farther apart,
  scaled by the window's noise bandwidth so that a sine reads its peak level: a full-scale sine 0 dBFS, up to 0.8 dB
  less between bins.
- **Statistics** (`analysis::StatisticsMeter`): per channel the lowest and highest sample, the peak, the DC offset (the
  mean, also in percent), the bit depth (the fewest bits b for which every sample times 2^(b-1) is whole, 32 for
  floating point beyond 24), the estimated pitch (the strongest frequency of the spectrum), the RMS of every sample
  and the highest RMS of 50 ms windows 10 ms apart, both as AES17 has them (a full-scale sine is 0 dB). Loudness and
  true peak come from the loudness meter ([metering.md](metering.md)), which the audio is fed through offline:
  maximum momentary and short-term loudness, integrated loudness, loudness range and the true peak per channel, 4
  times oversampled, of up to 8 channels; channels beyond have their sample peak as true peak.

## In the UI

The Audio menu has Advanced > Detect Silence..., Spectrum Analyzer and Statistics, the UI commands
`audio.detectSilence`, `audio.spectrumAnalyzer` and `audio.statistics` of the event display. They open a dialog for
the selected audio events, as the fade dialog does ([fades.md](fades.md)), whose Lua does the rest through the engine
API.

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
  select#sa-channel             the channel shown
  #sa-value                     channel, frequency, note and level under the mouse, and D, the difference to the
                                reference
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

## Not yet

The Detect Silence dialog's waveform with its threshold handles, preview playback with Mute Gaps, and the zoom; the
Spectrum Analyzer's zoom slider and showing every channel at once; clips in the pool and the sample editor's selection
range as what they analyze (the engine takes a clip and a range already); a pitch that is the fundamental of sounds
whose overtone is louder, not their strongest frequency.

## Checks

- `tests/audiofunctions_tests.cpp`: Detect Silence on tones whose edges are known, with both thresholds, the minimum
  times, pre-roll and post-roll; `clip.detectSilence` in a range and its errors; `event.stripSilence` splitting,
  regions, fades, one undo step, regions alone and its errors; the statistics of reference signals (a sine's levels,
  RMS and pitch, the true peak of a sine at a quarter of the sample rate, DC offset, 16 and 8 bit samples, the
  highest RMS, EBU Tech 3341 and 3342's loudness signals, silence and a range); the spectrum of sines in two channels
  and of silence.
- `tests/audiofunctions_ui_tests.cpp` drives the dialogs with `ui::Automation`: Analyze and its sections along the
  event, Add as Regions and Process for the first event, Auto, Linked and the open threshold for the second;
  Cancel, and Process All Selected Events with Apply Fades; the Statistics window's values; the Spectrum Analyzer's
  bars, the values under the mouse, the reference of a right click and D, and a click dropping it.
