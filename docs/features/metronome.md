# Metronome and count-in

The click that follows the project's tempo and time signatures, and the bars it counts in before playback starts. The code is in `src/engine/metronome`.

## Settings
- The settings belong to the project: they are saved with it (the `metronome` object of GNUbase's extra DAWproject entry) but changing them is no undo step. `metronome.get` returns them, `metronome.set` changes the fields it is given and leaves the others, `metronome.setPattern` sets or removes the click pattern of one time signature. Each change emits `metronome.changed` with all settings.
- `click` turns the click on; `whilePlaying` and `whileRecording` say when it sounds. Recording doesn't exist yet, so `whileRecording` is stored but has no effect.
- **Audio click** (`audio`): played through one or more output busses (`outputs`, the main mix if empty or if the chosen busses are gone) at `level` dB. The sounds are `beeps` (frequency and level per accent) or `samples` (an audio file and level per accent). A sample that can't be read plays its accent's beep instead; `metronome.set` rejects a new sample path that can't be read. Samples are decoded to mono, cut at 10 seconds and converted to the device's sample rate on the command thread, and decoded again at the next change to the project or the device after the file changed.
- **MIDI click** (`midi`): a note per accent (`notes`, with velocity) on MIDI `channel` 1–16, sent to the instrument channel `destination`. Sending it to a MIDI output port comes with MIDI output routing.
- **Accents and patterns**: there are four accents, 1 the strongest. A bar of a signature without its own pattern clicks once per beat of the signature, accent 1 on the first. A pattern (`patterns`, one per signature with a numerator up to 64) divides the bar into up to 64 equal clicks and gives each an accent 1–4 or 0 for silence, e.g. 6/8 as two clicks per bar or 7/8 as 2+2+3.
- **Count-in** (`countIn`): `bars` 1–16 counted in when `transport.play` is called with `countIn: true` and the count-in is on. Its `signature` is `start` (the signature and tempo at the start position), `project` (the bars before the start as the project has them, with their tempo changes; it begins `bars` bars before the bar of the start position and runs up to the start, so from a start within a bar it also counts that bar's first beats) or `custom` (`customSignature` at the tempo at the start).

Patterns per signature event, as opposed to per signature, come with the signature track (#156); the setup dialog comes with the UI.

## Processing
- The metronome is a built-in processor (`Metronome`), added to the graph as its last node while the click or the count-in is on. Its mono output goes to the chosen output busses, its events to the destination instrument channel, so busses sum it in their fixed order like every other input.
- The command thread compiles the settings into a `ClickConfig` (sounds with their level, patterns, tempo and signature maps) and publishes it to the audio thread; it compiles again only when the settings, the tempo or signature map, a sample rate or a sample file changed.
- `clicksIn` (`clicks.h`) lists the clicks within a range of samples: bar by bar, each bar divided into its pattern's clicks at exact tick positions, converted to samples with the same rounding as the tempo map. The clicks of a block don't depend on where blocks start, so the click is the same for every block size and thread count.
- Eight voices play the audio click; a new click takes a free voice or the one that started first. The voices are rendered one after another into the output, so the sum is the same every time. A MIDI click note lasts 50 ms; a new click ends the note before.

## Count-in in the transport
- `transport.play` with `countIn: true` hands the transport a `graph::CountIn`: its length in samples, and how its clicks are placed (on the project timeline before the start, or in one tempo and signature). The transport then counts in instead of playing: `TransportBlock` carries the count-in frames and its position, and the metronome clicks them. Playback starts at the exact sample the count-in ends, within the block, with the cursor at the start position. Stopping during a count-in ends it and leaves the cursor where it was.
- Frames of a block before playback starts are stopped for plugins (a stopped transport segment first) and `-1` for the `positionProbe`.
