# Time-stretching and pitch-shifting

How GNUbase changes the length and the pitch of audio independently (#142): the libraries we evaluated, what we measured, the one we chose, the engine wrapper around it and how audio tracks will use it.

The reference offers this in three places: offline processes, the sample editor and real-time playback of warped audio events. It has algorithm families with presets: a high-quality one for polyphonic and monophonic material (with a formant-preserving variant and a cheaper "efficient" mode), and a CPU-efficient real-time one with presets for drums, plucked instruments, pads, vocals, mixes and solo instruments. GNUbase follows that structure with its own presets (see [Presets](#presets)).

## Candidates

Both are GPLv3-compatible and maintained.

| | Rubber Band Library | Signalsmith Stretch |
|---|---|---|
| Version | v4.0.0 (2024) | 1.4.0 (2025), with signalsmith-linear 0.6.6 |
| License | GPL-2.0-or-later (or commercial) | MIT |
| Method | phase vocoder; R2 engine ("faster") with transient detection and phase resets, R3 engine ("finer") with multi-resolution analysis | phase vocoder with spectral peak tracking |
| Modes | real-time (streaming, ratio and pitch may change) and offline (studies the whole input first) | streaming; `exact()` for whole buffers |
| Formants | preserved on request (both engines) | formant shift and compensation |
| Build | one C++ file (`single/RubberBandSingle.cpp`), built-in FFT and resampler | header-only C++11 plus the signalsmith-linear headers |

Others were not measured: SoundTouch (LGPL-2.1, time-domain WSOLA) is cheap but known for weak results on polyphonic material, and the commercial libraries the reference uses are not GPL-compatible.

## Benchmark

`tests/bench/timestretch_bench.cpp` measures both on synthetic signals at 48 kHz and prints the tables below. Rubber Band is measured through GNUbase's wrapper (`src/engine/dsp/timestretch.h`) with four of its presets; Signalsmith Stretch directly, with its two presets and a fixed random seed.

```sh
# Signalsmith Stretch isn't part of GNUbase; check it out anywhere for the comparison.
git clone --branch 1.4.0 https://github.com/Signalsmith-Audio/signalsmith-stretch.git /tmp/ss
git clone --branch 0.6.6 https://github.com/Signalsmith-Audio/linear.git /tmp/signalsmith-linear
cmake -S . -B build/bench -DGNUBASE_BENCHMARKS=ON "-DGNUBASE_BENCH_SIGNALSMITH_DIRS=/tmp/ss;/tmp"
cmake --build build/bench --config Release --target gnubase-timestretch-bench
build/bench/bin/Release/gnubase-timestretch-bench
```

What it measures, each in offline mode and in real-time mode (output pulled in blocks of 512 frames, the way a track would play it):

- **Pitch error:** a 440 Hz sine shifted by +7, -5, +12, +0.5 and -12.3 semitones, its frequency measured from zero crossings over 1.6 s. The largest error in cents.
- **Transient smearing:** noise bursts with a sharp attack and a 20 ms decay every 250 ms, stretched by 1.5. Pre-echo is the energy 3 to 25 ms before each hit's peak relative to the 22 ms after it (the input: -98 dB); attack is the 10 % to 90 % rise time of a 1 ms peak envelope (the input: 0.1 ms). Lower is better for both.
- **Phasiness:** 20 harmonics of 110 Hz in cosine phase, a pulse wave with a 16 dB crest factor, stretched by 1.5. When a phase vocoder loses the phase relations between partials (vertical phase coherence), the waveform turns diffuse and "phasey" and its crest factor drops: crest loss. Ripple is the spread of the level of 100 ms windows (11 periods each), i.e. amplitude modulation the stretch adds.
- **Unity null:** the pulse wave at ratio 1 and no shift, subtracted from the input at the best whole-sample lag; the residual in dB below the input.
- **CPU:** 10 s of stereo (pulse wave and bursts), stretched by 1.25 and shifted by +3 semitones. Offline: total time as a multiple of real time. Real time: the same plus the slowest block as a share of its 10.7 ms.
- **Latency:** in real-time mode, the output frames before the output lines up with the input. Rubber Band also wants a start pad of input frames before the first one; a player reads those from before the start, so they cost no time.
- **Allocations:** heap allocations and frees in the real-time process path, counted by the RT checker (`gnubase-rtcheck`). On Linux and in an MSVC Debug build, it also sees `malloc` and `free`, so third-party code can't hide them.

### Results

Windows 11, x64, MSVC 19.50 Release, on a machine shared with other builds, so CPU numbers vary by about 20 %. Allocations from the Debug build.

Offline:

| Candidate | Pitch error (cents) | Pre-echo (dB) | Attack (ms) | Crest loss (dB) | Ripple (dB) | Unity null (dB) | CPU (x real time) |
|---|---|---|---|---|---|---|---|
| Rubber Band R3 (preset quality) | 0.0 | -9.0 | 5.3 | 3.3 | 0.01 | -173 | 8 |
| Rubber Band R3 short window (efficient) | 0.0 | -10.3 | 1.3 | 3.8 | 0.01 | -187 | 24 |
| Rubber Band R2 (mix) | 0.0 | -0.1 | 16.5 | 2.8 | 0.15 | -87 | 27 |
| Rubber Band R2 (drums) | 30.1 | -20.2 | 2.5 | -1.1 | 0.12 | -75 | 32 |
| Signalsmith Stretch (default) | 5.1 | -16.3 | 5.8 | 3.1 | 0.00 | -131 | 66 |
| Signalsmith Stretch (cheaper) | 7.1 | -11.1 | 6.0 | 1.5 | 0.01 | -126 | 101 |

Real time:

| Candidate | Pitch error (cents) | Pre-echo (dB) | Attack (ms) | Crest loss (dB) | Unity null (dB) | CPU (x real time) | Slowest block | Latency (frames) | Allocations |
|---|---|---|---|---|---|---|---|---|---|
| Rubber Band R3 (quality) | 0.0 | -8.8 | 6.1 | 3.3 | -40.5 | 7 | 23 % | 1723 (36 ms) | 0 |
| Rubber Band R3 short window (efficient) | 0.0 | -17.6 | 3.1 | 3.8 | -43.8 | 18 | 10 % | 1077 (22 ms) | 0 |
| Rubber Band R2 (mix) | 0.0 | -0.0 | 21.7 | 3.1 | -43.8 | 17 | 13 % | 862 (18 ms) | 0 |
| Rubber Band R2 (drums) | 38.1 | -14.8 | 1.8 | -1.2 | -43.8 | 20 | 9 % | 431 (9 ms) | 0 |
| Signalsmith Stretch (default) | 5.1 | -16.1 | 5.8 | 5.4 | -129 | 63 | 9 % | 6480 (135 ms) | 0 |
| Signalsmith Stretch (cheaper) | 7.1 | -15.9 | 6.3 | 3.9 | -126 | 102 | 2 % | 7320 (153 ms) | 0 |

The Rubber Band rows count no allocations only with the fixes in [Real-time safety](#real-time-safety); before them, every preset allocated in its first blocks and when the ratio changed.

### Findings

- **Pitch accuracy.** Rubber Band's R3 engine and R2's standard window hit the pitch exactly. Signalsmith Stretch is 5 to 7 cents off on pure tones, which is audible next to unshifted material. R2 with short windows and independent phases (the drums preset) is 30 to 38 cents off: fine for drums, as the reference warns for its drum mode too, not for tuned material.
- **Transients.** Signalsmith Stretch and Rubber Band's short-window presets keep attacks sharpest; R3 with its full multi-resolution analysis smears attacks over about 5 ms with some pre-echo. R2's mix preset smears noise bursts badly (its transient detection resets phases only outside the musical range), so drums go to the drums preset.
- **Phasiness.** Both lose about 3 to 5 dB of crest factor on a 1.5 stretch: comparable. Neither adds amplitude modulation.
- **Transparency.** At ratio 1, both are transparent offline. Rubber Band's real-time mode runs pitch through a resampler even without a shift, so pitch can change smoothly while playing; that leaves a sub-sample delay that a null test sees (-40 dB) but the ear doesn't.
- **CPU.** Signalsmith Stretch is the cheapest by far, about 8 times cheaper than R3's quality preset. R3 quality uses about 14 % of a core for a stereo track in real time; the efficient and R2 presets about 6 %.
- **Latency.** Rubber Band: 9 to 36 ms depending on the preset. Signalsmith Stretch: 135 to 150 ms, too much for monitoring or live pitch-shifting.
- **API fit.** Rubber Band's real-time API is block-based with a declared maximum block size (`setMaxProcessSize`), tells how much input it needs (`getSamplesRequired`), reports start pad and delay, and has a separate offline mode with an analysis pass and an exact output length. Signalsmith Stretch takes any block sizes, which is simple, but leaves alignment to the caller, has no analysis pass, and seeds its random generator from `std::random_device` unless given a seed, so its default output differs between runs.
- **Build portability.** Rubber Band builds from one file with MSVC, GCC and Clang without extra libraries. Signalsmith Stretch is header-only but needs a second library (signalsmith-linear) that its CMake file downloads at configure time.

## Decision

**Rubber Band Library**, pinned at v4.0.0 in `ext/rubberband`. It is the only candidate with exact pitch, a real offline mode, formant preservation in both engines, presets for kinds of material, and latency low enough for real time. Its cost is CPU: the quality preset is several times more expensive than Signalsmith Stretch. The efficient preset narrows that gap with little loss, and the anticipative processing of #23 runs playback ahead on spare cores anyway.

Signalsmith Stretch remains an option for a future cheap preview mode; the benchmark keeps it measurable.

## Real-time safety

Rubber Band documents its real-time mode as free of allocations, locks and blocking calls once set up. Reading its source and measuring found exceptions, all in the code that runs when the ratio or pitch changes. GNUbase patches Rubber Band (`ext/patches/rubberband.patch`, applied with Git to a copy of its sources in the build folder at configure time; configuring fails if it stops applying):

- **R2 windows and FFTs.** The R2 engine (`R2Stretcher::reconfigure`) picks a window and FFT size from the ratio and pitch, between 512 frames and four times its base size, and created a window, a sinc filter and an FFT the first time it met a size. The patch creates them for every size up front (`R2Stretcher::configure`), and sizes the per-channel buffers for the largest (`ChannelData::construct`), so `ChannelData::setSizes` never grows them.
- **R2 onset detector.** `PercussiveAudioCurve::setFftSize` freed and allocated its buffer on every FFT size change, so a ratio automated across a window size boundary allocated each time it crossed (the drums preset at a ratio of about 0.38, for example). The patch makes it grow only, and `configure` grows it to the largest size up front.
- **The built-in resampler** (`BQResampler`, used for pitch in both engines) replaced its buffer with a new vector on its first use and whenever its ratio changed, and reserved room for fewer phases than some ratios need. The patch reassigns the buffer in place and reserves the most phases any ratio needs.

What the patch leaves to the wrapper: the R2 output buffer, which only grows, and grows the first time ratio and pitch reach a new extreme. The wrapper's constructor steps through the whole real-time range once, so it reaches its largest size before playback. That is why real-time mode has a narrower range than offline mode (ratio 0.25 to 4, pitch within 24 semitones, against 0.1 to 10 and 48 semitones): the buffers stay a reasonable size. The R3 engine needs none of this.

The patch should become a GNUbase fork of Rubber Band like SDL and RmlUi (and go upstream); see [dependencies.md](dependencies.md).

`tests/timestretch_tests.cpp` plays every preset in real-time mode while ratio and pitch sweep the whole real-time range back and forth, crossing every window and FFT size change, under the RT checker. The RT checker sees the C heap too (`malloc`, `posix_memalign`, `free`) on Linux and in MSVC Debug builds, so the Linux CI jobs catch allocations in Rubber Band, which allocates with those. GNUbase builds Rubber Band without threads, timing or thread checks; its log messages go to GNUbase's real-time safe log.

## Alignment

Unchanged (ratio 1, no shift), the output is the input, to the sample, in both modes: offline mode compensates internally, real-time mode through `startPad()` and `latency()`. Stretched or shifted, events move by up to about 10 ms (20 ms with the pads preset), as in every phase vocoder: windows and phase resets fall on a grid of hops, not on the transient. One exception is measured but not yet fixed: in real time, the R2 presets other than drums lag by a constant 16 ms (pads 23 ms) when ratio times pitch scale reaches 4, because Rubber Band reports a latency that is too short there. `tests/timestretch_tests.cpp` checks where short tone bursts land, for every mode and preset.

## Presets

| Preset | Engine and options | For |
|---|---|---|
| `quality` (default) | R3, standard (multi-resolution) window, channels together | the best result for most material: mixes, vocals, bass, soft onsets |
| `efficient` | R3, short window, channels together | nearly as good at a third of the CPU and lower latency |
| `drums` | R2, short window, independent phases, crisp transients, percussive detector | drums and percussion: timing first, pitch less exact |
| `plucked` | R2, crisp transients, compound detector | transients over a stable sound: guitars, piano |
| `pads` | R2, smooth transients, long window | pitched, slow material; the smoothest, timing less exact |
| `vocals` | R2, mixed transients, soft detector | slower material with soft transients and a tonal character |
| `mix` | R2, mixed transients, compound detector, channels together | mixed material where CPU matters |
| `solo` | R2, crisp transients, soft detector | monophonic instruments and voices |

"Channels together" keeps stereo mono-compatible. Formant preservation is a separate switch for every preset. Real-time mode always uses Rubber Band's high-consistency pitch method, so pitch can change while playing without clicks.

The reference's tape variant (pitch follows the speed, like a tape) is resampling rather than stretching; it will use the r8brain resampler GNUbase already has.

## Engine wrapper

`src/engine/dsp/timestretch.h`, the only code that includes Rubber Band:

- `StretchSettings`: channels, sample rate, mode (`RealTime` or `Offline`), preset, ratio (output length over input length), pitch in semitones and cents, formant preservation and the largest block per process call.
- `TimeStretcher`: one stretcher. The constructor allocates everything. In real-time mode `setRatio`, `setPitch`, `setPreserveFormants`, `inputRequired`, `process`, `available` and `retrieve` are real-time safe. `startPad()` and `latency()` report the input frames to feed before the first one and the output frames to drop. Offline mode takes the input length and a `study` pass first, then outputs exactly the stretched length with no latency.
- `stretch()` for whole buffers and `stretchFile()` for files, in either mode, aligned and exactly `round(frames * ratio)` long.

The same input and settings always give the same output, in both modes and for any block size a player pulls (tested). Across platforms, results may differ in the last bits: Rubber Band uses the C library's math functions, whose rounding differs between platforms.

## Engine command

`media.stretch` stretches and shifts a media file into a new 32-bit float WAV file and adds it as a new media file, one undo step, like `media.convert`. Parameters: `ratio`, `semitones`, `cents`, `preset`, `formants` and `mode` (`offline` by default; `realtime` renders what playback would sound like). It is the engine side of the offline time-stretch and pitch-shift processes; MCP and Lua get it from the command registry ([lua.md](lua.md)).

## How audio tracks will use it

Warping audio events in real time (musical mode, tempo changes, warp markers, transposing events) comes with the audio editing tickets of M11. The plan:

- Each audio event gets warp settings in the document: preset, formant preservation, transpose, and a stretch ratio that comes from the tempo map in musical mode or from warp markers.
- Stretching runs where resampling runs today: on the thread that fills a voice's disk stream (`DiskStream`), ahead of playback, with one `TimeStretcher` in real-time mode per voice. The audio thread keeps reading finished frames from lock-free queues, so the CPU cost lands on disk and prefetch threads (#23), never on the device callback.
- Cueing a voice reads `startPad()` frames from before the cue position and drops `latency()` output frames, so the stretched audio starts at its exact sample, as resampled clips do now. A tempo change sets the ratio between blocks; real-time mode allows that without allocating.
- Rendering and freezing use the same real-time path, so playback and export sound the same. "Make permanent" uses `media.stretch` in offline mode for the best quality.
- Warp markers between two points map to ratio segments in real time, and to Rubber Band's key-frame map in offline mode.
- Like the reference's algorithms, stretching only plays forward: reverse playback and scrubbing of warped events will play the unstretched audio or the frozen file.

## Follow-ups

- Fork Rubber Band into the GNUbase organisation with the patch, point `ext/rubberband` there, and send the patch upstream.
- The real-time lag of the R2 presets when ratio times pitch scale reaches 4 (see [Alignment](#alignment)).
- Warp settings on audio events and real-time warping in the disk streams (M11).
- Tape mode (varispeed) through the existing resampler.
- The UI for the offline processes and the sample editor's warp tools.
