# Sampler tracks

Sampler tracks (#180, reference manual pp. 143-144, 710-738) play a sample from the pool chromatically via MIDI.

## In the engine

A sampler track is an instrument track whose channel plays the built-in **sampler** instrument
(`processors::Sampler`, `src/engine/processors/sampler.h`), with the pool clip it plays in the channel's `sample`
(0 for none). MIDI parts, recording, monitoring, the mixer and automation work on it as on any instrument track. The
sampler is a GNUbase processor and runs inside the engine (architecture rule 3).

| Command | What it does | Reference |
|---|---|---|
| `sampler.create` | A new sampler track with a pool clip (`clip`), an audio event's clip (`event`) or an audio file (`path`), which goes into the pool as a clip in the same undo step, from `start` to `end` in clip frames (default: the event's section or the whole clip), or empty without any; named `name`, or Sampler 01, Sampler 02 and so on, the first number no track's name has; placed and routed with `parent`, `index` and `output` as `track.add` does | pp. 143-144, 711 |
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
  Monophonic came on (automation can switch it) end with their note-offs; a legato note fades them out like any other
  monophonic note and takes over only the latest.
- Each voice has its own filter, whose cutoff rises with the key by the key follow share (100 %: exactly with the
  pitch), after Drive's tanh saturation; Drive belongs to the filter section (p. 722) and is off with the filter.
- Loading (#1122): when a channel's sample changes, and for every sampler when a project loads, the audio engine's
  sample loader (`SampleLoader`, `src/engine/audio/sampleloader.h`) reads the file on a thread of its own, so a long
  sample holds up no command. Once it is read the engine runs `audio.updateSamples` on its own, which hands it to the
  samplers that wait for it. Until then a sampler plays the sample before, or nothing if it had none. Starting
  playback and locating wait for the samples as for the audio tracks' streams (up to 2 seconds), and offline renders
  (`audio.render`, `mixdown.export`, render in place) wait until every sample is there (up to 30 seconds, after which
  they log a warning), so they come out the same every time. Files that no sampler waits for any more, after an undo
  or another project, are dropped from the loader's queue unread, and a read that fails plays nothing, as a missing
  file does.
- Hot swap: the sampler takes the new sample without a lock (`rt::SnapshotPublisher`); voices keep playing on the new
  one. It lets go of the sample before by the next update, once the audio thread has moved on.
- Samples are read through `processors::sharedSample`, which reads a file once while something holds its sample:
  samplers that play the same clip and `sampler.normalize` share it. A file that changed (its time or size) is read
  again, and is read without holding up those who ask for other files. `sampler.normalize` still reads a file that no
  sampler holds on the command thread.

Not yet: warped (time-stretched) playback with sync to the tempo, the quality modes, transient, grid and manual
slicing with fades and the slices' MIDI phrase, the pitch and filter envelopes and a multi-node amp envelope, the LFOs,
glide, sample fades and the loop crossfade, Trim Sample, MIDI Reset, Hot Swap Preview Sample, the sampler panel's own
undo history and A/B settings, loading by dragging an event or a MIDI part onto the panel, a pool folder of the
sampler's samples (p. 710), and transferring the sample to another instrument, which needs a plugin wrapper extension
for samples. The panel's bars that move Start and End or the loop together, the display's ruler and zoom, the
toolbar's Read/Write Automation, event received indicator, Snap to Zero Crossing and preset section, and the Add Track
dialog's Input Routing and Event Type, which no page of it has yet ([add-track.md](add-track.md)), are open too
(#1122).

## In the UI

Create Sampler Track is in the Audio menu (`audio.createSamplerTrack`), for the first selected audio event, and in the
sample editor's Range section (`#se-create-sampler`), for the selection range or else the whole event (p. 711). Add
Sampler Track in the Project menu and the track list's context menu (`project.addSamplerTrack`, pp. 143-144, 711)
opens the Add Track dialog on its Sampler page, with Output Routing, Name and Count, which adds empty sampler tracks
with `sampler.create` ([add-track.md](add-track.md)). The Instrument page offers the Sampler among the instruments too.

**The sampler panel** (`ui::SamplerPanel`, `src/ui/samplerpanel.h`, pp. 712-717, 730) is a panel of the lower zone,
Sampler, that Project > Show Sampler (`window.sampler`) shows; like every panel it can open in a window of its own
([docking.md](docking.md)). It shows the first selected track while that is a sampler track, else only a hint (the
root `.sampler-panel` has the class `.empty`):

- **Toolbar**: Import Audio File (`#sampler-import`, a file dialog, then `sampler.load` with `path`), the clip's name
  (`#sampler-file`), the Normalization Level (`#sampler-level`, -96 to 0 dBFS, p. 735), Normalize Sample
  (`#sampler-normalize`, `sampler.normalize` with that `level`) and Lock Parameter Settings (`#sampler-lock`), which
  loads keep the settings with (`keepSettings`). The lock and the level are the user setting `ui.samplerPanel`
  (`lockParameters`, `normalizeLevel`; what later versions add to it is kept), so they stay as they were when the panel is made anew, e.g. in a window of its
  own. Why a load (a file that isn't audio) or Normalize Sample (a silent or missing sample) failed shows in the
  toolbar's message (`#sampler-message`, `.error`) until the next one succeeds or another track is shown.
- **Waveform display** (`#sampler-display`): the sample's waveform from `media.peaks`, its parts before Start and
  after End shaded (`.sampler-outside`), the loop (`.sampler-loop`) and the markers (`.sampler-marker`, attribute
  `marker`: Start, End, Loop start, Loop end), placed in percent of the sample and mirrored when Reverse is on. Without
  a loop mode the loop and its markers have `.off`. Dragging a marker sets its parameter, one undo step per drag; Start
  stays before End and the loop's start before its end.
- **Sections** (`.sampler-section`): Playback, Pitch, Filter, Amp and Keyboard, a control per parameter of
  `processor.list`'s sampler (`[parameter]`, `#sampler-parameter-<id>`): a toggle for those that are on or off, a
  drop-down list for those with options, a value field for the others, each gesture one undo step (`parameter.set`
  through `EngineBinding::sendStep`).
- **Keyboard** (`.sampler-key`, attribute `key`): the root key `.root`, keys outside the key range `.outside`; a click
  sets the root key. Above it the root key handle and the key range's handles (`.sampler-handle`, attribute `handle`:
  root, low, high; p. 730) sit on their keys; dragging one sets its key, one undo step per drag, and the range's low
  key stays at or below its high one.
- A file dropped on the panel loads into the sampler (`sampler.load` with `path`, p. 710).

## Tests

`tests/sampler_tests.cpp`: the sampler on its own (pitch, tuning, key range, start and end, reverse, every loop mode
and loops shorter than a step, one shot, slices, monophonic and legato with the fade of the note taken over and notes
held from before, the amp section and envelope, the filter types, key follow and drive, letting go of a swapped
sample), `sharedSample`, the commands and their names, the pool, track presets, removing clips, importing and saving, and offline renders of a sampler track against the sample itself and
the golden file `sampler-track-phrase`, then a hot swap and its undo; `tests/rtcheck_tests.cpp` plays it with every
loop mode, slices and the filter without allocating; `tests/sampleeditor_ui_tests.cpp` and
`tests/audiofunctions_ui_tests.cpp` create sampler tracks from the UI; `tests/sampler_ui_tests.cpp` drives the sampler
panel (following the selection, toggles, value fields, markers, the keyboard, dropped files, Normalize Sample) and the
Add Track dialog's Sampler page; `tests/backup_tests.cpp` backs one up with
`removeUnused` and `minimize`. #1122 added the sample loader's own test and a legato note after Monophonic came on to
`tests/sampler_tests.cpp`, and the load and Normalize Sample messages, the lock and level as a user setting and the
keyboard's handles to `tests/sampler_ui_tests.cpp`.
