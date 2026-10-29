# Audio analysis

Measuring the captured output, for tests and AI agents (#28).

- **Analysis** of the captured output (#28), for tests and AI agents: `audio.analyze` measures channels over a range of frames (peak and RMS level, a spectrum summary with the strongest frequencies and octave band levels, and the integrated loudness per ITU-R BS.1770-4); `audio.compare` subtracts a reference audio file and measures what remains (null test); `audio.record` writes the capture to a WAV file. The measurements are pure functions in `src/engine/analysis`, the commands in `src/engine/audio/analysiscommands.cpp`; they run on the command thread. Busses and track channels can be recorded and measured once the graph has capture taps; until then, the device outputs that the output busses reach.
