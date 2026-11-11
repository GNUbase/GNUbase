# Sampler tracks

Sampler tracks (#180, reference manual pp. 143-144, 710-738) play a sample from the pool chromatically via MIDI.

## In the engine

A sampler track is an instrument track whose channel plays the built-in **sampler** instrument
(`processors::Sampler`, `src/engine/processors/sampler.h`), with the pool clip it plays in the channel's `sample`
(0 for none). MIDI parts, recording, monitoring, the mixer and automation work on it as on any instrument track. The
sampler is a GNUbase processor and runs inside the engine (architecture rule 3).

| Command | What it does | Reference |
|---|---|---|
| `sampler.create` | A new sampler track with a pool clip (`clip`) or an audio event's clip (`event`), from `start` to `end` in clip frames (default: the event's section or the whole clip), or empty without either; named `name`, or Sampler 01, Sampler 02 and so on, the first number no track's name has | pp. 143, 711 |
| `sampler.load` | Loads another sample, the same way, into a sampler track while it plays (hot swap). Its settings go back to their defaults, or with `keepSettings` (Lock Parameter Settings) only those of the sample: root key, start, end, loop start and end | pp. 710, 715-716 |
| `sampler.normalize` | Sets the Sample gain so that the highest peak between start and end reaches `level` dBFS (default 0); refused for a silent sample or a missing file | pp. 715, 735 |

Each is one undo step and emits `channel.changed`; `sampler.create` also `track.added`. Another instrument
(`channel.set`, or a track preset's in `track.loadPreset`) drops the sample. A sampler's clip counts as used in the pool (`pool.get`, `pool.removeUnused`,
`clip.remove` refuses it, `media.removeMissing` empties the sampler) and in a backup (`project.backup`, p. 710: its
`removeUnused` keeps it, and `minimize` copies a file that a sampler plays whole, as Start, End and the loop are shares
of it). Imported and exported tracks (`track.import`, `track.export`) take their sample along. The sample is saved
with the channel in `gnubase.json`; a clip that isn't in the pool when the project loads is dropped.

The sampler's settings are its parameters (`processor.list`, `parameter.set`, automatable like any instrument's):

| Section | Parameters | Reference |
|---|---|---|
| Keyboard | Root key (default 60), Key low and Key high (the key range), Pitchbend range (0-24 semitones) | pp. 713, 730, 733-734 |
| Pitch | Fixed pitch, Octave, Coarse (semitones), Fine (cents) | pp. 714, 721 |
| Playback | Start, End, Loop start and Loop end (shares of the sample, 0 to 1), Loop mode (No loop, Continuous, Alternate, Once, Until release, Alternate until release), Reverse, One shot, Playback mode (Normal or Slice), Slices, Monophonic, Legato, Sample gain | pp. 714-716, 718-720, 732 |
| Filter | Filter on, type (low pass, high pass, band pass: 12 dB/oct), Cutoff, Resonance, Drive, Cutoff key follow | pp. 721-722 |
| Amp | Volume, Pan, and the amp envelope: Attack, Decay, Sustain, Release | p. 722 |

How it plays:

- A note plays the sample from Start to End, transposed by its distance from the root key (none with Fixed pitch),
  plus the tuning and the pitch bend, and at the sample's own rate whatever the project's. Catmull-Rom interpolation
  reads between frames, so a note on the root key plays the sample unchanged.
- Positions are shares of the sample, so they stay where they are when the sample's file changes rate. Reverse plays
  the sample backwards, with every position counted from its end.
- The loop modes follow p. 714: Once plays the loop once more, then on to the end; the Until release modes stop
  looping when the key goes up and play on to the end. One shot plays from start to end, ignoring loops and
  note-offs. A loop shorter than a step (a short loop octaves up) still loops: the position wraps, or reflects, by
  the remainder of the loop's length.
- Slice mode cuts the range between Start and End into Slices equal slices, mapped to the keys from the root key up,
  each at the sample's pitch without loops (p. 735).
- Up to 128 voices; a new note beyond that takes the oldest. Monophonic plays one note: a note stolen while still held
  plays again when the newer one ends, and with Legato a new note only changes the pitch while the envelope runs on
  (p. 715). The note taken over fades out in 5 ms instead of stopping with a click, and notes still held from before
  Monophonic came on (automation can switch it) end with their note-offs.
- Each voice has its own filter, whose cutoff rises with the key by the key follow share (100 %: exactly with the
  pitch), after Drive's tanh saturation; Drive belongs to the filter section (p. 722) and is off with the filter.
- Hot swap: the audio engine reads the sample's file on the command thread when the channel's sample changes and
  hands it to the sampler that plays without a lock (`rt::SnapshotPublisher`); voices keep playing on the new one.
  It lets go of the sample before the next time it attaches the graph, once the audio thread has moved on.
- Samples are read through `processors::sharedSample`, which reads a file once while something holds its sample:
  samplers that play the same clip and `sampler.normalize` share it. A file that changed (its time or size) is read
  again. The first read still holds up the command thread; loading in the background is left for later.

Not yet: the sampler panel in the lower zone and a Sampler page in the Add Track dialog, warped (time-stretched)
playback, transient and manual slicing and the slices' MIDI phrase, the pitch and filter envelopes, the LFOs, glide,
quality modes, the sampler panel's own undo history, and transferring the sample to another instrument, which needs a
plugin wrapper extension for samples.

## In the UI

Create Sampler Track is in the Audio menu (`audio.createSamplerTrack`), for the first selected audio event, and in the
sample editor's Range section (`#se-create-sampler`), for the selection range or else the whole event (p. 711). The
Add Track dialog offers the Sampler among the instruments of an instrument track.

## Tests

`tests/sampler_tests.cpp`: the sampler on its own (pitch, tuning, key range, start and end, reverse, every loop mode
and loops shorter than a step, one shot, slices, monophonic and legato with the fade of the note taken over and notes
held from before, the amp section and envelope, the filter types, key follow and drive, letting go of a swapped
sample), `sharedSample`, the commands and their names, the pool, track presets, removing clips, importing and saving, and offline renders of a sampler track against the sample itself and
the golden file `sampler-track-phrase`, then a hot swap and its undo; `tests/rtcheck_tests.cpp` plays it with every
loop mode, slices and the filter without allocating; `tests/sampleeditor_ui_tests.cpp` and
`tests/audiofunctions_ui_tests.cpp` create sampler tracks from the UI; `tests/backup_tests.cpp` backs one up with
`removeUnused` and `minimize`.
