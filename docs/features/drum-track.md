# Drum tracks

Drum tracks (#182, reference manual pp. 145-146, 938-980) play the built-in drum machine: 128 pads, one per MIDI
note, each playing a sample from the pool or a percussion synth. With the pattern editor
([pattern-editor.md](pattern-editor.md)) they are the core of beat production.

## In the engine

A drum track is an instrument track whose channel plays the built-in **drum machine** (`processors::DrumMachine`,
`src/engine/processors/drummachine.h`), with its pads' names and pool clips in the channel's `pads`
(`[{pad, name, sample}]`, the pads that have either). MIDI parts, pattern events, recording, monitoring, the mixer and
automation work on it as on any instrument track. The drum machine is a GNUbase processor and runs inside the engine
(architecture rule 3).

| Command | What it does | Reference |
|---|---|---|
| `drum.create` | A new drum track, named `name` or Drum 01, Drum 02 and so on, placed and routed with `parent`, `index` and `output` as `track.add` does; its pads start with the default kit (below), or all off with `kit` false | pp. 145-146 |
| `drum.loadPad` | Loads a sample onto pad `pad`: a pool clip (`clip`), an audio event's clip (`event`) or an audio file (`path`), which goes into the pool in the same undo step, from `start` to `end` in clip frames. The pad's instrument becomes Sample, its other settings go back to their defaults unless `keepSettings` (Apply Hot Swap) | pp. 941, 944 |
| `drum.setPad` | Rename Pad: an empty `name` goes back to the sample's clip's name | p. 943 |
| `drum.copyPad` | Copy Pad and Paste Pad onto `to`: name, sample and settings; with `move` Cut Pad | p. 943 |
| `drum.resetPad` | Reset Pad, or Reset All Pads without `pad`: no name, no sample, every setting at its default | p. 943 |

Each is one undo step and emits `channel.changed`. Another instrument (`channel.set`, a track preset's) drops the
pads. The pads' clips count as used in the pool (`pool.get`, `pool.removeUnused`, `clip.remove` refuses them,
`media.removeMissing` empties their pads) and in backups, as a sampler's do; imported and exported tracks and track
presets take them along, and they are saved with the channel in `gnubase.json`, where a clip that isn't in the pool
when the project loads is dropped.

**The default kit** of `drum.create` (p. 942) is made of the percussion synths on the General MIDI notes that new drum
patterns start with, named after the General MIDI sounds: kick 36, snare 38, clap 39, closed, pedal and open hi-hats
42, 44 and 46 in exclusive group 1, toms 45, 48 and 50, crash 49 and cowbell 56.

**Synchronization with the pattern editor** (p. 938): a new drum pattern on a drum track (`pattern.add`) has a lane
per pad that plays, named after it; renaming, loading, copying or resetting a pad renames the lanes that play it in
the track's drum patterns (`pattern.changed`). A pad's name is its own, else its sample's clip's, else the General
MIDI sound's of its note.

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
  (`DrumMachine::setPadValues`, a lock-free snapshot), whenever the channel's values change.

## In the UI

Add Drum Track in the Project menu and the track list's context menu (`project.addDrumTrack`) opens the Add Track
dialog on its Drum page, with Output Routing, Name and Count ([add-track.md](add-track.md)).

**The drum machine panel** (`ui::DrumMachinePanel`, `src/ui/drummachinepanel.h`, pp. 938-977) is a panel of the lower
zone, Drum Machine, that Project > Show Drum Machine (`window.drumMachine`) shows; like every panel it can open in a
window of its own ([docking.md](docking.md)). It shows the first selected track while that is a drum track, else only
a hint (the root `.drum-machine` has the class `.empty`):

- **Toolbar**: the editing pages Instrument, Delay and Reverb (`#drum-page-instrument` and so on, the one shown
  `.selected`), Bypass All Effects (`#drum-bypass`) and a message (`#drum-message`, `.error`) when loading a sample
  failed.
- **Pad section**: the eight groups (`.drum-group`, attribute `group`), and the 16 pads of the one shown
  (`.drum-pad`, attribute `pad`) in four rows of four, the lowest note at the bottom left, with their names and notes;
  pads that play something are `.assigned`, the selected one `.selected`, muted, soloed and soloed-out ones `.muted`,
  `.soloed` and `.soloed-out`. A click selects a pad; it opens on the third group, with the kick.
- **Pad tools**, in the toolbar too: the selected pad's name (`#drum-pad-name`, Rename Pad when Enter is pressed or the field loses the
  focus), Load (`#drum-load`, a file dialog, then `drum.loadPad` with `path`), Copy, Cut, Paste (`drum.copyPad`) and
  Reset (`drum.resetPad`). A file dropped on a pad loads onto that pad, elsewhere on the panel onto the selected one.
- **Page**: the Instrument page has a control per parameter of the selected pad (`[parameter]`,
  `#drum-parameter-<id>`), the Delay and Reverb pages the global ones: a toggle, a drop-down list or a value field,
  each gesture one undo step (`parameter.set` through `EngineBinding::sendStep`).

Not yet: four layers per pad and the layer slots' context menus, the synths' own parameters of each model (Kick 1-4,
Snare 1-3, the rimshots, HiHat 1-4 and so on, pp. 948-964, of which GNUbase has one model per instrument with Tone and
Noise), the sample layer's fades, pre-filter, pitch and filter envelopes, filter shapes and distortion types
(pp. 965-968), Pad FX (pp. 968-972), the Group page (p. 973), low and high keys for melodic playing, the 31 output
busses, pad colors, the delay's Cross mode and L/R offsets, the reverb's chorusing, shape, density and band times,
Delay and Reverb presets, the drum machine's own undo history, MIDI Reset, MIDI Follow, playing a pad with a click,
the triggered pad's highlight, Hot Swap Preview, the Media Rack's Find Samples and Find Instruments, layer and pad
presets, the automation parameters with their quick controls, Learn CC and ranges (pp. 978-980), the pool folder of
the drum machine's samples (p. 980), and the Add Track dialog's Event Type (pattern event or MIDI part) and Input
Routing.

## Tests

`tests/drummachine_tests.cpp`: the drum machine on its own (a pad's sample at its note, start, end, tuning and
reverse; level, pan, velocity, mute and solo; exclusive groups and the gate; every synth sounding, decaying, held and
the same in any block size; the delay and reverb sends and Bypass All Effects; its parameters in `processor.list`),
the commands, the default kit, pattern lanes that follow the pads, saving and loading, another instrument dropping the
pads, automation of a pad's parameter and the values set without it, and offline renders of a drum track against the
sample itself and the golden file `drum-track-beat` (a pattern event playing the kit with its sends);
`tests/rtcheck_tests.cpp` plays samples, every synth, groups and the effects without allocating;
`tests/drummachine_ui_tests.cpp` drives the panel (following the selection, groups, pages, controls, renaming,
copying, cutting, pasting, resetting and dropping a file on a pad) and the Add Track dialog's Drum page.
