# Audio analysis

Measuring the captured output, for tests and AI agents (#28).

- **Analysis** of the captured output (#28), for tests and AI agents: `audio.analyze` measures channels over a range of frames (peak and RMS level, a spectrum summary with the strongest frequencies and octave band levels, and the integrated loudness per ITU-R BS.1770-4); `audio.compare` subtracts a reference audio file and measures what remains (null test); `audio.record` writes the capture to a WAV file. The measurements are pure functions in `src/engine/analysis`, the commands in `src/engine/audio/analysiscommands.cpp`; they run on the command thread. Busses and track channels can be recorded and measured once the graph has capture taps; until then, the device outputs that the output busses reach.
- **Audio functions** (#147): Detect Silence, the spectrum and the statistics of clips (`clip.detectSilence`, `event.stripSilence`, `clip.spectrum`, `clip.statistics`) build on the same measurements, in [audio-functions.md](audio-functions.md).

## Tests

`tests/analysis_tests.cpp` covers peak and RMS levels, K-weighting coefficients, integrated loudness against EBU Tech
3341 signals, spectrum peaks and octave bands, captured-output analysis, recording, null comparison and MCP analysis.
The captured-output analysis integration test uses two seconds of a steady stereo sine to keep its measurements within
the default command timeout under sanitizers. It also checks a 100 ms range of one channel and rejects invalid channels,
ranges and peak counts. The loudness unit tests retain the full-length EBU signals, including the gating sequences.
