# Drum tracks

Drum tracks (#182, reference manual pp. 145-146, 938-980) play the built-in drum machine: 128 pads, one per MIDI
note, each playing a sample from the pool or a percussion synth. With the pattern editor
([pattern-editor.md](pattern-editor.md)) they are the core of beat production.

## In the engine

A drum track is an instrument track whose channel plays the built-in **drum machine** (`processors::DrumMachine`,
`src/engine/processors/drummachine.h`), with its pads' names, pool clips and colors in the channel's `pads` (`[{pad,
name, sample, sound, color}]`, the pads that have one of them). MIDI parts, pattern events, recording, monitoring, the
mixer and automation work on it as on any instrument track. The drum machine is a GNUbase processor and runs inside the
engine (architecture rule 3).

| Command | What it does | Reference |
|---|---|---|
| `drum.create` | A new drum track, named `name` or Drum 01, Drum 02 and so on, placed and routed with `parent`, `index` and `output` as `track.add` does; its pads start with the default kit (below), or all off with `kit` false | pp. 145-146 |
| `drum.loadPad` | Loads a sample onto pad `pad`: a pool clip (`clip`), an audio event's clip (`event`) or an audio file (`path`), which goes into the pool in the same undo step, into the pool folder Drum Machine, made when the first file comes (p. 980), from `start` to `end` in clip frames. The pad's instrument becomes Sample, its other settings go back to their defaults unless `keepSettings` (Apply Hot Swap) | pp. 941, 944, 980 |
| `drum.setPad` | Rename Pad: an empty `name` goes back to the sample's clip's name; Set Color: `color` `#rrggbb`, empty or null for none (#1567) | pp. 942-943 |
| `drum.copyPad` | Copy Pad and Paste Pad onto `to`: name, sample and settings; with `move` Cut Pad. A pad that plays something and goes by the General MIDI sound of its note goes by that sound in the copy too (`sound`, the note of the sound), until it gets a name or a sample (#1567) | p. 943 |
| `drum.resetPad` | Reset Pad, or Reset All Pads without `pad`: no name, no sample, every setting at its default | p. 943 |

Each is one undo step and emits `channel.changed`. Another instrument (`channel.set`, a track preset's) drops the pads.
The pads' clips count as used in the pool (`pool.get`, `pool.removeUnused`, `clip.remove` refuses them,
`media.removeMissing` empties their pads) and in backups, as a sampler's do; imported and exported tracks and track
presets take them along (`track.loadPreset` and `track.addFromPreset` bring the preset's clips into the pool, as
`track.import` does, unless it has the same clips already, and share the project's media files that have the same audio;
a preset has the samples' files in it, and refers to those that are missing where they were), and they are saved with
the channel in `gnubase.json`, where a clip that isn't in the pool when the project loads is dropped.

**The default kit** of `drum.create` (p. 942) is made of the percussion synths on the General MIDI notes that new drum
patterns start with, without names of their own, so that they go by the General MIDI sounds' until they get a sample
or a name: kick 36, snare 38, clap 39, closed, pedal and open hi-hats 42, 44 and 46 in exclusive group 1, toms 45, 48
and 50, crash 49 and cowbell 56.

**Synchronization with the pattern editor** (p. 938): a new drum pattern on a drum track (`pattern.add`) has a lane per
pad that plays, named after it; renaming, loading, copying or resetting a pad, and loading a track preset
(`track.loadPreset`) or importing a drum track onto the track with `track.import`'s `destinations`, renames the lanes
that play it in the track's drum patterns (`pattern.changed`). A pad's name is its own, else its sample's clip's, else
the General MIDI sound's of its note, or of the note in its `sound`.

The drum machine's settings are its parameters (`processor.list`, `parameter.set`, automatable like any
instrument's). Pad *n*'s have the ids *n* × 32 and up:

| Parameter | What it does | Reference |
|---|---|---|
| Instrument | Off, Sample, Kick, Snare, Hi-hat, Tom, Clap, Cymbal or Cowbell: what the pad plays (its layer) | pp. 945-965 |
| Level, Pan, Mute, Solo | The pad's level (dB) and position; a muted pad, or one not soloed while others are, plays on silently | pp. 942, 944 |
| Tune | Semitones, for samples and synths | pp. 946, 965 |
| Tone, Noise | The synths' brightness and noise | pp. 947-963 |
| Hold, Decay | The amplifier envelope: the time before the decay, and the decay's time to fall by 60 dB | pp. 947, 968 |
| Velocity to level | How much the velocity sets the level (Vel > Lev) | p. 947 |
| One shot | Samples play whole, without hold and decay | p. 967 |
| Gate | The note's end ends the sound, in 5 ms; otherwise pads play on to their end | p. 944 |
| Start, End, Reverse | The part of the sample that plays, shares of it; Reverse counts them from its end | p. 965 |
| Exclusive group | 1-32: a pad cuts the others of its group off, in 5 ms (0: none) | p. 974 |
| Delay send, Reverb send | How much of the pad goes to the global delay and reverb | pp. 944, 968 |

The global ones have the ids 4096 and up: Level, Bypass All Effects, the Delay page's (on, time in ms or synced to a
note value, feedback, ping-pong, To Reverb, level; pp. 975-976, on the delay of the mixing effects) and the Reverb
page's (on, predelay, room size, time, damping, level; pp. 977-978, on the algorithmic reverb).

How it plays:

- A note plays its pad; up to 64 voices, a new one beyond that takes the oldest. The synths are sines, squares and
  filtered noise with pitch envelopes: a kick falls from four times its pitch, toms and snares less; hi-hats and
  cymbals are a cluster of six squares; the clap is three bursts of band-passed noise before its tail. Each voice's
  noise comes from a generator of its own, seeded by the pad, so every render is the same (rule 6).
- Samples are read between frames (linear) at the sample's own rate whatever the project's, unchanged at no tuning.
  They load as a sampler's do (#1122): the sample loader reads them on its own thread, a pad plays its sample before,
  or nothing, until it is read, and offline renders wait for them ([sampler-track.md](sampler-track.md)).
- The 2432 pad parameters are more than the 1024 events of a block, so the automation player has lanes only for the
  pad parameters that have automation and for the global ones. The others reach the drum machine with its samples
  (`DrumMachine::setPadValues`, a lock-free snapshot), whenever the channel's values change. Events for a pad
  parameter count only while it is automated: the engine hands over the values without a removed lane before the
  automation without it, and the lane's last events would otherwise keep their value (#1361).
- A synth voice takes its pad's tuning, tone and noise at its note on; level, pan, envelope and sends follow the
  pad's values while it sounds.

## In the UI

Add Drum Track in the Project menu and the track list's context menu (`project.addDrumTrack`) opens the Add Track
dialog on its Drum page, with Output Routing, Name and Count ([add-track.md](add-track.md)).

**The drum machine panel** (`ui::DrumMachinePanel`, `src/ui/drummachinepanel.h`, pp. 938-977) is a panel of the lower
zone, Drum Machine, that Project > Show Drum Machine (`window.drumMachine`) shows; like every panel it can open in a
window of its own ([docking.md](docking.md)). It shows the first selected track while that is a drum track, else only
a hint and Add Drum Track (`#drum-add-track`, p. 146), which opens the Add Track dialog on its Drum page as
`project.addDrumTrack` does (the root `.drum-machine` has the class `.empty`):

- **Toolbar**: the editing pages Instrument, Delay and Reverb (`#drum-page-instrument` and so on, the one shown
  `.selected`), Bypass All Effects (`#drum-bypass`) and a message (`#drum-message`, `.error`) when loading a sample
  failed.
- **Pad section**: the eight groups (`.drum-group`, attribute `group`), and the 16 pads of the one shown (`.drum-pad`,
  attribute `pad`) in four rows of four, the lowest note at the bottom left, with their names and notes; a pad without a
  name of its own or a sample shows the General MIDI sound's of its note, which `processor.list` has for it
  (`padNames`), as pattern lanes do; pads that play something are `.assigned`, the selected one `.selected`, muted,
  soloed and soloed-out ones `.muted`, `.soloed` and `.soloed-out`, and those with a color `.colored`, with the color
  in their strip (`.drum-pad-color`, attribute `color` and `background-color`, user data). A click selects a pad, and
  the pad plays at velocity 127 while the mouse button is down on it (p. 942): a note on and off into the track's
  channel through `midi.inject` with `drumSounds`, as the drum editor auditions sounds; a button released outside the
  panel ends the note with the next press. It opens on the third group, with the kick.
- **Pad tools**, in the toolbar too: the selected pad's name (`#drum-pad-name`, Rename Pad when Enter is pressed or the
  field loses the focus), Set Color (`#drum-pad-color`, a drop-down list of No Color and the project's color set, the
  16 colors of the factory set in a new project, [colors.md](colors.md)), Load (`#drum-load`, a file dialog, then
  `drum.loadPad` with `path`), Copy, Cut, Paste
  (`drum.copyPad`) and Reset (`drum.resetPad`). A file dropped on a pad loads onto that pad, elsewhere on the panel onto
  the selected one.
- **Page**: the Instrument page has a control per parameter of the selected pad (`[parameter]`,
  `#drum-parameter-<id>`), the Delay and Reverb pages the global ones: a toggle, a drop-down list or a value field,
  each gesture one undo step (`parameter.set` through `EngineBinding::sendStep`).

Not yet: four layers per pad and the layer slots' context menus, the synths' own parameters of each model (Kick 1-4,
Snare 1-3, the rimshots, HiHat 1-4 and so on, pp. 948-964, of which GNUbase has one model per instrument with Tone and
Noise), the sample layer's fades, pre-filter, pitch and filter envelopes, filter shapes and distortion types (pp.
965-968), Pad FX (pp. 968-972), the Group page (p. 973), low and high keys for melodic playing, the 31 output busses,
the delay's Cross mode and L/R offsets, the reverb's chorusing, shape, density and band times, Delay and Reverb presets,
the drum machine's own undo history, MIDI Reset, MIDI Follow, the triggered pad's highlight, Hot Swap Preview, the media
browser's Find Samples and Find Instruments, layer and pad presets, the toolbar's Read/Write Automation, Load Track
Preset and preset management (pp. 939-940), the automation parameters with their quick controls, Learn CC and ranges
(pp. 978-980), and the Add Track dialog's Event Type (pattern event or MIDI part) and Input Routing. A track preset has
the whole file of a sample that a pad or a sampler plays a section of; `project.backup`'s `minimize` cuts such files
down for backups.

## Tests

`tests/drummachine_tests.cpp`: the drum machine on its own (a pad's sample at its note, start, end, tuning and reverse;
level, pan, velocity, mute and solo; exclusive groups and the gate; every synth sounding, decaying, held and the same in
any block size; the delay and reverb sends and Bypass All Effects; its parameters in `processor.list`), a pad parameter
ignoring the events of its removed lane, the commands (a file's clip in the pool folder Drum Machine), the default kit
and the names it goes by (`processor.list`'s `padNames`), a copy going by the sound a synth pad went by until a sample
names it, a synth voice keeping its tuning, tone and noise from its note on, pattern lanes that follow the pads, track
presets and drum tracks imported onto the track, saving and loading, another instrument dropping the pads, the pads'
clips in the pool (`pool.removeUnused`, `media.removeMissing`), track presets loaded onto a drum track and onto another
instrument's, tracks added from a preset or imported from another project with their pads' clips, automation of a pad's
parameter and the values set without it, and offline renders of a drum track against the sample itself and the golden
file `drum-track-beat` (a pattern event playing the kit with its sends); `tests/rtcheck_tests.cpp` plays samples, every
synth, groups and the effects without allocating; `tests/drummachine_ui_tests.cpp` drives the panel (following the
selection, the names of pads without their own, groups, pages, controls, playing a pad with a click, renaming, coloring,
copying (a synth pad's copy shown by the sound it was copied from), cutting, pasting, resetting and dropping a file on a
pad) and the Add Track dialog's Drum page, from the Project menu's command and from the empty panel.
