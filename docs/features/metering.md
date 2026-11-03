# Metering and loudness

Level meters for every channel and the main mix, the master meter's true peak and the loudness meter after EBU R 128 (#111, reference manual pp. 533-540). This is the engine part; the meters in the mixer's channel strips (#107) and the right zone's meter display come with their UI tickets.

## How it works

- **Level meters** (`LevelMeter` in `src/engine/audio/meters.h`). Each channel strip measures its output after the fader, the panner and muting (`ChannelStrip::fade`), per audio channel, in the channel's `ChannelState`, so the meter lives as long as the channel and survives new graphs. The audio thread publishes through atomics every 100 ms: the peak, the largest sample of those 100 ms, and the RMS level of the last three such windows (300 ms), plus their maxima since the last reset. Nothing allocates or locks. MIDI channels have no audio channels and no meter.
- **The main mix** is measured as the device plays it, after auditioning, on the main mix bus's device outputs (`AudioEngine::meterMainMix`): a level meter, and the loudness meter, which also is the master meter's true peak meter.
- **Loudness** (`LoudnessMeter`, ITU-R BS.1770-4, EBU Tech 3341 and 3342). The channels are K-weighted (`analysis::kWeighting`) and summed, each weighted 1. Every 100 ms step the meter publishes the momentary loudness (the last 400 ms) and the short-term loudness (the last 3 s). While the transport plays it also measures: the maxima of both, the integrated loudness of 400 ms blocks every 100 ms, gated at -70 LUFS and then 10 LU below the loudness of the blocks above that gate, and the loudness range of 3 s blocks every second, gated at -70 LUFS and 20 LU below their loudness, from the 10th to the 95th percentile. The blocks go into histograms of 0.1 LU from -70 LUFS up that keep each bin's energy and count, so the measurement runs for any length in fixed memory on the audio thread; the gates are exact to the bin, the range to 0.1 LU.
- **True peak**: each channel 4x oversampled through a 96-tap Kaiser-windowed sinc interpolator, centred on a tap so that it reads the samples and the points 1/4, 1/2 and 3/4 between them, flat within 0.01 dB up to 0.4 times the sample rate. The peak of the last 100 ms, and its maximum while playing.
- **Commands.** `meter.get` reads the channels (all, or those given), `main` and `loudness`, in dBFS, dBTP and LUFS; null for silence and for values not measured yet. `meter.reset` resets the maxima and starts the loudness measurement over and announces `meter.reset`. The user setting `meter.resetOnStart` (the reference's Reset on Start) starts the loudness meter and the main mix's maxima over whenever playback starts.

## Tests

`tests/meter_tests.cpp`: a sine's peak and RMS level, the maxima and their reset; EBU Tech 3341's steady sines (cases 1 and 2) and gating (cases 3 and 4) within 0.1 LU; EBU Tech 3342's loudness range cases 1 to 3 within 1 LU; the integrated loudness of noise against the offline `analysis::integratedLoudness`; the true peak of a sine at a quarter of the sample rate that falls between the samples; measuring only while playing and resetting; silence; and `meter.get`, `meter.reset` and `meter.resetOnStart` through the dummy device.

## Not yet

The meter settings that only the display needs (peak hold time, fallback, scales, alignment levels, the loudness scales and LU/LUFS, the threshold indicators), meters for the input and output busses and the control room, BS.1770's channel weights for surround formats (every channel is weighted 1), Measure Loudness switched off, and the meter UI.
