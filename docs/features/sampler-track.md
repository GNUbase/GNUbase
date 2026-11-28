# Sampler tracks

Sampler tracks (#180, reference manual pp. 143-144, 710-738) play a sample from the pool chromatically via MIDI.

## In the engine

A sampler track is an instrument track whose channel plays the built-in **sampler** instrument
(`processors::Sampler`, `src/engine/processors/sampler.h`), with the pool clip it plays in the channel's `sample`
(0 for none). MIDI parts, recording, monitoring, the mixer and automation work on it as on any instrument track. The
sampler is a GNUbase processor and runs inside the engine (architecture rule 3).

| Command | What it does | Reference |
|---|---|---|
| `sampler.create` | A new sampler track with a pool clip (`clip`), an audio event's clip (`event`) or an audio file (`path`), which goes into the pool as a clip in the same undo step, into the pool folder Sampler Tracks, made when the first file comes (p. 710), from `start` to `end` in clip frames (default: the event's section or the whole clip), or empty without any; named `name`, or Sampler 01, Sampler 02 and so on, the first number no track's name has; placed and routed with `parent`, `index` and `output` as `track.add` does, and taking its MIDI from `midiInput` as `channel.set`'s (`all`, the default, `chordPads` or `none`; the Add Track dialog's Input Routing, p. 144) | pp. 143-144, 710-711 |
| `sampler.load` | Loads another sample, the same way, into a sampler track while it plays (hot swap). Its settings go back to their defaults, or with `keepSettings` (Lock Parameter Settings) only those of the sample: root key, start, end, the fades, loop start, end and crossfade | pp. 710, 715-716 |
| `sampler.normalize` | Sets the Sample gain so that the highest peak between start and end reaches `level` dBFS (default 0); refused for a silent sample or a missing file | pp. 715, 735 |
| `sampler.zeroCrossings` | A query for Snap to Zero Crossing: moves `positions`, shares of the sample as Start and End are (counted from its end with Reverse), each to the nearest frame within 50 ms where the sum of its channels changes sign, the frame of the two there that is nearer to zero; a position without one near stays | p. 713 |

Each but the query is one undo step and emits `channel.changed`; `sampler.create` also `track.added`, and with a file
`media.added`, `clip.added`, `clip.changed` and, for the first one, `poolFolder.added`. Another instrument
(`channel.set`, or a track preset's in `track.loadPreset`) drops the sample. A sampler's clip counts as used in the pool
(`pool.get`, `pool.removeUnused`, `clip.remove` refuses it, `media.removeMissing` empties the sampler) and in a backup
(`project.backup`, p. 710: its `removeUnused` keeps it, and `minimize` copies a file that a sampler plays whole, as
Start, End and the loop are shares of it). Imported and exported tracks (`track.import`, `track.export`) and track
presets (`track.savePreset`, with the sample's file in the preset, and `track.loadPreset`) take their sample along,
whose clip goes into the pool folder Sampler Tracks when the project hasn't the same one yet, as the drum pads' go into
Drum Machine ([drum-track.md](drum-track.md)). The
sample is saved with the channel in `gnubase.json`; a clip that isn't in the pool when the project loads is dropped.

The sampler's settings are its parameters (`processor.list`, `parameter.set`, automatable like any instrument's):

| Section | Parameters | Reference |
|---|---|---|
| Keyboard | Root key (default 60), Key low and Key high (the key range), Pitchbend range (0-24 semitones) | pp. 713, 730, 733-734 |
| Pitch | Fixed pitch, Octave, Coarse (semitones), Fine (cents), Glide (ms, 0: off) and Fingered | pp. 714, 721 |
| Playback | Start, End, Loop start and Loop end (shares of the sample, 0 to 1), Fade in, Fade out and Loop crossfade (lengths, shares of the sample too), Loop mode (No loop, Continuous, Alternate, Once, Until release, Alternate until release), Reverse, One shot, Playback mode (Normal or Slice), Slices, Monophonic, Legato, Sample gain | pp. 714-720, 732 |
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
- The fades (p. 717) belong to the range from Start to End: Fade in rises from 0 at Start over its length, Fade out
  falls to 0 at End, both straight lines and counted in the sample's positions, so a loop inside a fade plays faded.
  Slices don't fade. Loop crossfade blends a loop's last frames into those just before its start while the loop
  wraps (Continuous, Once before its round, Until release while the key is held), so the wrap goes on with what it
  faded into; it is no longer than the loop and than what lies between Start and the loop's start. Alternating loops
  don't crossfade.
- Glide (p. 721) bends a new note's pitch from the note before to its own in the glide time, linearly in semitones:
  from the key of the note played last, also while that still glides, or from the one started last once nothing
  sounds. With Fingered only a note played while another is held glides, from the latest held; going back to a held
  key in monophonic mode glides too. Slices and Fixed pitch don't glide; the filter's key follow follows the glide.
- Slice mode cuts the range between Start and End into Slices equal slices, mapped to the keys from the root key up,
  each at the sample's pitch without loops (p. 735).
- Up to 128 voices; a new note beyond that takes the oldest. Monophonic plays one note: a note stolen while still held
  plays again when the newer one ends, and with Legato a new note only changes the pitch while the envelope runs on
  (p. 715). The note taken over fades out in 5 ms instead of stopping with a click, and notes still held from before
  Monophonic came on (automation can switch it) end with their note-offs; a legato note fades them out like any other
  monophonic note and takes over only the latest.
- Each voice has its own filter, whose cutoff rises with the key by the key follow share (100 %: exactly with the
  pitch), after Drive's tanh saturation; Drive belongs to the filter section (p. 722) and is off with the filter.
- All Sound Off (controller 120) silences the voices of its MIDI channel at once, One shot ones too, and forgets the
  held keys, so monophonic mode doesn't play a stuck key again when another one ends; Reset All Controllers (121)
  takes the pitch bend back to none. The panel's MIDI Reset sends both (p. 716).
- Loading (#1122): when a channel's sample changes, and for every sampler when a project loads, the audio engine's
  sample loader (`SampleLoader`, `src/engine/audio/sampleloader.h`) reads the file on a thread of its own, so a long
  sample holds up no command. Once it is read the engine runs `audio.updateSamples` on its own, which hands it to the
  samplers that wait for it. Until then a sampler plays the sample before, or nothing if it had none or if it belongs
  to a project that replaced another (`project.load`, `project.new`): the render snapshot's `opened` tells the audio
  engine so, since a channel of the same id keeps its instrument and the sample before is the other project's.
  Starting playback, with `transport.play` or with `transport.record` from stop without a count-in, waits for the
  samples as for the audio tracks' streams (up to 2 seconds); locating and a count-in don't. Offline renders
  (`audio.render`, `mixdown.export`, render in place) wait until every sample is there, so they come out the same every
  time: they log a warning every 30 seconds and fail with an error after 10 minutes (#1343), so a file whose read never
  ends, e.g. on a network drive that stalls, doesn't hold them for good; none of them can be cancelled while it waits.
  The warning and the error name the files still waited for (#1414). An export that fails there puts the transport, its
  settings and the device back and leaves no files, as any export that fails while it renders. Starting playback and the
  offline renders wait only for the files that samplers wait for, not for one being read that none wants any more.
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

Not yet: warped (time-stretched) playback with sync to the tempo, the quality modes, transient, grid and manual slicing
with fades and the slices' MIDI phrase, the pitch and filter envelopes and a multi-node amp envelope, the LFOs, Trim
Sample, Hot Swap Preview Sample that restores the sample before, the sampler panel's own undo history and A/B
settings, loading a MIDI part dragged onto the panel (rendered through its instrument, p. 711) and an audio event
dragged onto the panel in a window of its own, and transferring the sample to another instrument, which needs a plugin
wrapper extension for samples. The toolbar's Write Automation (with the write modes, #122) and preset section, and the
Add Track dialog's Event Type ([add-track.md](add-track.md)), are open too (#1122).

## In the UI

Create Sampler Track is in the Audio menu (`audio.createSamplerTrack`), for the first selected audio event, and in the
sample editor's Range section (`#se-create-sampler`), for the selection range or else the whole event (p. 711). Add
Sampler Track in the Project menu and the track list's context menu (`project.addSamplerTrack`, pp. 143-144, 711)
opens the Add Track dialog on its Sampler page, with Input Routing (All MIDI Inputs, Chord Pads or Not Connected),
Output Routing, Name and Count, which adds empty sampler tracks with `sampler.create` ([add-track.md](add-track.md)).
The Instrument page offers the Sampler among the instruments too.

**The sampler panel** (`ui::SamplerPanel`, `src/ui/samplerpanel.h`, pp. 712-717, 730) is a panel of the lower zone,
Sampler, that Project > Show Sampler (`window.sampler`) shows; like every panel it can open in a window of its own
([docking.md](docking.md)). It shows the first selected track while that is a sampler track, else only a hint (the
root `.sampler-panel` has the class `.empty`):

- **Toolbar** (pp. 712-716): Read Automation (`#sampler-read`, p. 712), on while every automation lane of the track
  reads and disabled without lanes, which turns them all on or off in one undo step as the track list's button does
  ([track-list.md](track-list.md)), the event received indicator (`#sampler-received`), Snap to Zero Crossing
  (`#sampler-zero`), Import Audio File (`#sampler-import`, a file dialog, then `sampler.load` with `path`), the clip's
  name (`#sampler-file`), the Normalization Level (`#sampler-level`, -96 to 0 dBFS, p. 735), Normalize Sample
  (`#sampler-normalize`, `sampler.normalize` with that `level`), Lock Parameter Settings (`#sampler-lock`), which
  loads keep the settings with (`keepSettings`), and MIDI Reset (`#sampler-midi-reset`). The lock, the level, Snap
  to Zero Crossing and the ruler's format are the user setting `ui.samplerPanel` (`lockParameters`, `normalizeLevel`,
  `snapToZero`, `rulerFormat`; what later versions add to it is kept), so they stay as they were when the panel is made anew, e.g. in a window of its
  own. While the panel's own changes of it are on their way, e.g. during a drag of
  the level, the older ones that `settings.changed` brings back don't set the field back. A change from elsewhere still
  shows: results and events reach the panel in the engine's order, so one made after the panel's last `settings.set`
  comes after that save's answer, when none of the panel's own is on its way. Why a load (a file that isn't audio) or
  Normalize Sample (a silent or missing sample) failed shows in the toolbar's message (`#sampler-message`, `.error`)
  until the next one succeeds or another track is shown; so does MIDI Reset's (no audio device).
  - The event received indicator gets `.lit` for a quarter of a second after a note-on or a controller that the track's
    MIDI input receives: with `midiInput` all, from an input that is among all MIDI inputs (`midi.ports`
    `inAllInputs`); with chordPads or none it stays out. While the panel shows a sampler track it turns
    `midi.activityListen` on for itself and reads the `midi.activity` events.
  - With Snap to Zero Crossing on, a drag of the marker of Start, End or the loop's start or end ends where the sample
    crosses zero nearest to where it was let go (`sampler.zeroCrossings`), still in order and in the drag's undo step;
    so does a drag of a bar, with both its markers in one query. Key commands, such as an undo right after the drag,
    wait for it (`EngineBinding::sendAwaited`); the panel's own mouse input doesn't. The fades' markers and the value
    fields set what they are given.
  - MIDI Reset (p. 716) sends All Sound Off and Reset All Controllers on all 16 MIDI channels straight to the sampler
    (`midi.inject` with `direct`), which stops what it plays, a long One shot sample too, and resets the pitch bend.
- **Waveform display** (`#sampler-display`): the sample's waveform from `media.peaks`, its parts before Start and after
  End shaded (`.sampler-outside`), the loop (`.sampler-loop`) and the markers (`.sampler-marker`, attribute `marker`:
  Start, End, Loop start, Loop end, Fade in, Fade out, Loop crossfade), placed in percent of the sample and mirrored
  when Reverse is on. Without a loop mode the loop and its markers have `.off`. Dragging a marker sets its parameter,
  one undo step per drag; Start stays before End and the loop's start before its end. The fades' and the loop
  crossfade's markers (`.length`, p. 717) mark the end of their lengths: Fade in's lies that far after Start, Fade out's
  before End, Loop crossfade's before the loop's end; dragging one sets the length, no longer than the range from Start
  to End, or for the crossfade than the loop and than what lies between Start and the loop's start, the loop within
  Start and End, as the sampler plays it. They lie below the flags of Start and End and above the loop's, so that those
  can still be dragged where a length of 0 puts a marker on them. Between Start and End at the flags' height, and
  between the loop's markers at the bottom, lies a bar (`.sampler-bar`, attribute `bar`: range, loop; the loop's has
  `.off` with the loop) that the themes show on hover: dragging it moves both markers by as far as the mouse went,
  keeping their distance, and stops at the sample's ends, both parameters in one step and one undo step
  per drag (p. 717).
- **Ruler and zoom** (p. 717, #1122): above the display a ruler (`.sampler-ruler`) shows the part of the sample shown,
  from the sample's start and at its own rate, in the format that the arrow button at its right picks
  (`#sampler-ruler-format`: Bars+Beats at the project's tempo and signatures, Seconds, the default, or Samples). Beside
  the display the vertical zoom (`#sampler-vertical-zoom`) raises the waveform up to 64 times; below it the scroll
  slider (`#sampler-scroll`) moves along the sample and the horizontal zoom (`#sampler-horizontal-zoom`) shows from the
  whole sample down to 64 frames around the middle of what it showed, asking `media.peaks` for that part only. Below 256
  frames per pixel the waveform shows sample columns rather than repeated block peaks ([peaks.md](peaks.md)). The
  markers, the bars, the loop and the shades lie in the part shown, markers outside it are hidden (`.out-of-view`), and
  drags count in it. The zoom is the panel's own, the whole sample again with another one.
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
- A file dropped on the panel loads into the sampler (`sampler.load` with `path`, p. 710), and so does an audio event
  dragged onto it from the event display, while the panel is in the project window's lower zone (`sampler.load` with
  `event`, its section as Start and End; #1122): the event display hands a drag of events that ends outside it to
  `EventDisplay::onDropEvent`, and the events move only when nothing takes them. Both keep the settings with Lock
  Parameter Settings.
- **Key commands** (p. 717, #1122): a press in the display gives the panel the keyboard focus. The panel is the focus
  zone `sampler`, where the project window's zoom keys zoom the display instead ([key-commands.md](key-commands.md)):
  H and G (`samplerPanel.zoomIn`, `samplerPanel.zoomOut`) halve and double the part shown around its middle, as far
  as the horizontal zoom goes, Shift+H and Shift+G (`samplerPanel.zoomInVertically`, `samplerPanel.zoomOutVertically`)
  double and halve the waveform's gain from 1 to 64 times, and Shift+F (`samplerPanel.zoomFull`) shows the whole
  sample.

## Tests

`tests/sampler_tests.cpp`: the sampler on its own (pitch, tuning, key range, start and end, reverse, every loop mode and
loops shorter than a step, one shot, slices, monophonic and legato with the fade of the note taken over and notes held
from before, the amp section and envelope, the filter types, key follow and drive, letting go of a swapped sample),
`sharedSample`, the commands and their names, the pool, track presets, removing clips, importing and saving, and offline
renders of a sampler track against the sample itself and the golden file `sampler-track-phrase`, then a hot swap and its
undo; `tests/rtcheck_tests.cpp` plays it with every loop mode, slices and the filter without allocating;
`tests/sampleeditor_ui_tests.cpp` and `tests/audiofunctions_ui_tests.cpp` create sampler tracks from the UI;
`tests/sampler_ui_tests.cpp` drives the sampler panel (following the selection, toggles, value fields, markers, the
keyboard, dropped files, Normalize Sample) and the Add Track dialog's Sampler page; `tests/backup_tests.cpp` backs one
up with `removeUnused` and `minimize`. #1122 added the sample loader's own tests and a legato note after Monophonic came
on to `tests/sampler_tests.cpp`, with the engine's side of a load while the dummy device plays in real time: a read held
back (`Engine::setSampleReader`) leaves `sampler.load` and later commands running and the sampler silent, the posted
`audio.updateSamples` alone hands the sample over, and a project loaded over another doesn't play the other's sample
meanwhile. It added the load and Normalize Sample messages for dropped and imported files, the lock and level as a user
setting, the keyboard's handles, and drags released outside the window to `tests/sampler_ui_tests.cpp`. #1180 and
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
#1122 also added the bars that move Start and End or the loop together to `tests/sampler_ui_tests.cpp`, and glide with
and without Fingered, the fades from the sample's start and from Start, and the loop crossfade and its limit to
`tests/sampler_tests.cpp`, the new parameters to the real-time test and the fades to the settings that `keepSettings`
doesn't keep.
#1122 added the markers of the fades and the loop crossfade to `tests/sampler_ui_tests.cpp`, then MIDI Reset's
messages, Snap to Zero Crossing at the end of a marker's and a bar's drag and the event received indicator, which a
note-off and an input outside all MIDI inputs don't light; `tests/sampler_tests.cpp` got All Sound Off and Reset All
Controllers, which also forgets the held keys, and `sampler.zeroCrossings`, reversed too.
#1343 added the warnings at every round of an offline render's wait for a sample and the error after the last of its
20 rounds, tested with rounds of 20 ms instead of 30 s (`Engine::setOfflineSampleWait`), and checks that starting
playback and recording from stop answer only after the held read is released.
#1122 then added to `tests/sampler_tests.cpp` the pool folder Sampler Tracks that files loaded into samplers, and
imported samplers' samples, go into, and to `tests/sampler_ui_tests.cpp` the ruler with its format and the zoom
sliders, with a marker's drag while zoomed in. Then `sampler.create`'s `midiInput` went into `tests/sampler_tests.cpp`,
and the Add Track dialog's Input Routing and the toolbar's Read Automation into `tests/sampler_ui_tests.cpp`.
#1122 then added the zoom's key commands and an audio event dragged from the event display onto the panel to
`tests/sampler_ui_tests.cpp`.
