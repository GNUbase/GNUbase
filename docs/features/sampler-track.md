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
  samplers that wait for it. Until then a sampler plays the sample before, or nothing if it had none or if it belongs
  to a project that replaced another (`project.load`, `project.new`): the render snapshot's `opened` tells the audio
  engine so, since a channel of the same id keeps its instrument and the sample before is the other project's.
  Starting playback, with `transport.play` or with `transport.record` from stop without a count-in, waits for the
  samples as for the audio tracks' streams (up to 2 seconds); locating and a count-in don't. Offline renders
  (`audio.render`, `mixdown.export`, render in place) wait until every sample is there, so they come out the same every
  time: they log a warning every 30 seconds and fail with an error after 10 minutes (#1343), so a file whose read
  never ends, e.g. on a network drive that stalls, doesn't hold them for good; none of them can be cancelled while
  it waits. The warning and the error name the files still waited for (#1414). An export that fails there puts the
  transport, its settings and the device back and leaves no files, as any export that fails while it renders. Both wait only for the files that samplers wait for, not for one being read that none wants any more.
  Files that no sampler waits for any more, after an undo or another project, are dropped from the loader's queue
  unread; the one being read can't be stopped, and the engine's end waits for it. A read that fails plays nothing, as
  a missing file does.
- Hot swap: the sampler takes the new sample without a lock (`rt::SnapshotPublisher`); voices keep playing on the new
  one. It lets go of the sample before by the next update, once the audio thread has moved on.
- Samples are read through `processors::sharedSample`, which reads a file once while something holds its sample:
  samplers that play the same clip and `sampler.normalize` share it. Who asks for a file while it is read waits for
  that read rather than reading it again. A file that changed (its time or size) is read again, and is read without
  holding up those who ask for other files. `sampler.normalize` still reads a file that no sampler holds on the
  command thread, or waits there for the loader's read of it.

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
  own. While the panel's own changes of it are on their way, e.g. during a drag of the level, the older ones that
  `settings.changed` brings back don't set the field back. A change from elsewhere still shows: results and events
  reach the panel in the engine's order, so one made after the panel's last `settings.set` comes after that save's
  answer, when none of the panel's own is on its way. Why a load (a file that isn't audio) or Normalize Sample (a silent or missing sample) failed shows in the
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
  root, low, high; p. 730) sit on their keys, in a row each, so that none lies over another however narrow the keys
  are (the low one on top, the high one in the middle, the root key's at the bottom, each a key wide); dragging one
  sets its key, one undo step per drag, and the range's low key stays at or below its high one.
- The markers and the handles are RmlUi drag elements (`drag: drag`): a drag ends at its `dragend`, wherever the button
  goes up, outside the window too. Like every drag of the app it also ends when its window loses the focus with the
  button held, e.g. to Alt+Tab, since the button may then go up over another application, where the window doesn't
  hear it (`SdlWindowSystem`, #1191).
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
`removeUnused` and `minimize`. #1122 added the sample loader's own tests and a legato note after Monophonic came on to
`tests/sampler_tests.cpp`, with the engine's side of a load while the dummy device plays in real time: a read held
back (`Engine::setSampleReader`) leaves `sampler.load` and later commands running and the sampler silent, the posted
`audio.updateSamples` alone hands the sample over, and a project loaded over another doesn't play the other's sample
meanwhile. It added the load and Normalize Sample messages for dropped and imported files, the lock and level as a
user setting, the keyboard's handles, and drags released outside the window to `tests/sampler_ui_tests.cpp`. #1180 and
#1191 added a key held from polyphonic mode that a monophonic note goes back to, the loader waiting only for the files
asked for and its end, `sharedSample` reading a file asked for while it is read once, and, with the read held back, a
hot swap that plays the sample before on, a project loaded while the one before reads, playback waiting for the
samples while locating doesn't, an undo dropping a file waiting, and an offline render that waits however long a read
takes, with `audio.updateSamples` out of `engine.commandLog`; the real-time tests are tagged `[realtime]`. The UI tests
got the level's quick changes, the handles side by side and the low one stopping at the high key, a drag that ends
when the window loses the focus, the message going with another track and its tooltip, and Import Audio File when the
panel closed while the dialog was open.
#1200 added recording from stop that waits for the samples unless a count-in plays, the handles apart on a narrow
keyboard, and a change of the panel's setting from elsewhere right after the panel's own quick ones.
#1343 added the warnings at every round of an offline render's wait for a sample and the error after the last of its
20 rounds, tested with rounds of 20 ms instead of 30 s (`Engine::setOfflineSampleWait`), and checks that starting
playback and recording from stop answer only after the held read is released.
