# Channel strip

The sections of an audio channel before its fader (#107, reference manual pp. 463–489): the pre section, the
equalizer and the strip's dynamics, and their controls in the mixer. The fader and the panner are in
[mixer.md](mixer.md), the mixer itself in [mixer-window.md](mixer-window.md).

## In the engine

- **Settings** are part of the channel (`Channel::sections`, `ChannelSections` in `src/engine/document/document.h`),
  in `project.get` as its `pre`, `eq` and `strip` objects, changed with `channel.set`: each object changes only what
  it has, an EQ band is picked by its `band` (1 to 4). Values out of range, slopes other than 6, 12, 24, 36 and 48 dB
  per octave and band types a band can't have are refused; MIDI channels have no sections. Every change is an undo
  step, copied by `track.duplicate` and saved in GNUbase's extra entry; files from before have every section off.
- **Pre** (pp. 473–474): `gain` (-24 to +24 dB), `phase` (inverts the polarity), and a `lowCut` (20 Hz to 20 kHz) and
  a `highCut` (50 Hz to 20 kHz), each `on` with its `frequency` and `slope` (default 12 dB per octave).
- **Equalizer** (pp. 479–480): four bands, each `on` with its `type`, `frequency` (20 Hz to 20 kHz), `gain` (±24 dB)
  and `q` (0.1 to 12). Bands 2 and 3 are bells; band 1 can also be a low shelf or a low cut, band 4 a high shelf or a
  high cut, cuts at 12 dB per octave. `bypass` turns them all off.
- **Strip** (pp. 481–488): a noise `gate` (threshold, range, attack, release), a `compressor` (threshold, ratio,
  attack, release, makeup gain) and a brickwall `limiter` (threshold, the highest level any sample leaves at, and
  release; 1 ms of look-ahead), each with `on`.
- **Signal flow**: the channel's source (what plays or the input), then the pre gain and phase and the cut filters,
  then the plugin slots, then the gate, the compressor, the equalizer and the limiter, then the fader and the panner.
  This is the reference's default order of the strip's modules, with the EQ between the compressor and the limiter.
- **Processing**: `StripSections` (`src/engine/audio/stripsections.h`) runs the sections on the DSP cores of the
  built-in effects ([processors.md](processors.md)): one `dsp::Equalizer` for the cuts (its first and last band),
  another for the four bands, and the gate, compressor and limiter cores. The channel keeps it across graph rebuilds
  (`ChannelState`). Its values arrive as parameter events from the channel's `AutomationPlayer`, with the fader's
  (parameter `g_stripEvents` plus the parameter's id): the static values whenever they change, and the automation
  as it plays. It applies each one at its sample on the audio thread, splitting the block there, without
  allocating. The pre gain glides along the events' ramps frame by frame, like the fader, and over 5 ms to a new
  static value; the phase glides over 20 ms through zero; the equalizers glide their bands; dynamics start afresh
  when they come on. Sections that are off cost nothing: an equalizer runs while a band is on and 100 ms after the
  last one went off, to glide out, and the output of a strip with everything off is bit-identical to its input. The
  limiter's look-ahead counts in the channel node's latency while it is on or a lane of its switch plays
  (`ChannelStrip::latency`), and the limiter runs only while it counts, so that it never delays a channel by more
  than the latency its graph reports: a `channel.set` that turns it on reaches the old strip, which reports none,
  before the program with the new strips arrives. While such a lane has it off, it runs bypassed, so the latency
  stays and the switch doesn't click. A bypassed limiter only delays the signal, but its detector and gain keep
  running, so when the lane switches it on during a loud passage its gain is ready from the first frame and nothing
  is hard-clipped at the threshold; when the lane switches it off, its gain stops being applied from that sample on,
  so the level steps up by the reduction it was doing.
- **Parameters and automation** (#658): every setting is a parameter with a target of type `strip` and an id
  (`StripParameter`, `src/engine/document/stripparameters.h`), in `parameter.list`, `parameter.get` and
  `parameter.set` with its range, unit and options, and a target of automation lanes ([automation.md](automation.md),
  [parameters.md](parameters.md)): the pre section 0 to 7 (gain, phase, then the low and the high cut's switch,
  frequency and slope), the EQ's bypass 10 and band n's switch, type (bands 1 and 4: bell, shelf, cut), frequency,
  gain and Q at 10n+1 to 10n+5, the gate 50 to 54, the compressor 60 to 65 and the limiter 70 to 72, each switch
  first. Switches are 0 or 1, the slopes (6 to 48 dB per octave) and band types stepped choices, so their lanes are
  step curves. `automationLane.targets` groups them as Pre, Equalizer and Strip. `parameter.set` of the equalizer's
  and the dynamics' parameters passes the change on to the other channels of the channel's link group as `channel.set`
  does; the pre section's stays the channel's own, as with `channel.set` ([link-groups.md](link-groups.md)). A frozen
  track still plays the lanes of the sections after the plugin slots; its freeze file has the pre section's.

## In the mixer

The mixer's strips (`ui::Mixer`) have three more sections of the docking set `channel`, hidden by default and shown
with Set up Window Layout: Pre (`.mixer-pre`), Equalizer (`.mixer-eq`) and Strip (`.mixer-strip`), on audio channels
only; the fader section has the panner, a knob (`.mixer-pan`) for `pan`. Every control has a `param` attribute, the
path of its value in the channel's JSON (`pre.lowCut.on`, `eq.bands.3.gain`): toggle buttons for the switches, knobs
for gains, thresholds, ratios and Q, value fields in Hz for frequencies. A change sends `channel.set` with that value,
a knob's or value field's drag, wheel turns or held arrow key as one undo step (`EngineBinding::sendStep`, #716);
the strip shows the engine's values after every undo step, whichever client changed them.

## Not yet

The strip's other modules (tools: de-esser and envelope shaper; saturation), the tube and vintage compressors and the
maximizer and standard limiter, moving modules in the signal flow, side-chains, auto release and auto makeup, gain
reduction and gate state LEDs, the EQ curve display and note names for frequencies, EQ and strip presets, choosing the
cut filters' slopes and the EQ band types in the mixer, the combined panner's two controls, pre-fader and post-fader
insert slots, the sections of input and output channels, the sections in the inspector, and routing. The channel
settings window shows them for one channel ([channel-settings.md](channel-settings.md)). The sends are in
[routing.md](routing.md). Mapping the equalizer onto DAWproject's `Equalizer` device comes with DAWproject devices.

## Checks

- `tests/channelstrip_tests.cpp`: `channel.set` changes, refuses, undoes, saves and loads the sections; a strip with
  everything off, and one whose EQ was on and went off, passes its audio bit-exactly; measured with `audio.analyze`,
  the pre gain, the phase (the negated input), the low and high cuts at their slopes, EQ bells, shelves and cuts and
  the EQ bypass, the gate's range, the compressor's reduction and makeup and the limiter's threshold change the output
  as they should; the pre section drives a plugin slot and the EQ lifts what comes out of it; the limiter's latency,
  and that the limiter delays its channel only while that latency counts its look-ahead.
- `tests/parameter_tests.cpp`: the sections' parameters with their ranges, units and options, `parameter.set` and
  `channel.set` changing the same values, their groups, and none on a MIDI channel's or a VCA channel's, each refusal
  saying which.
- `tests/linkgroups_tests.cpp`: `parameter.set` passes the equalizer's and the dynamics' parameters on to the
  channel's link group, and not the pre section's.
- `tests/automation_tests.cpp`: at block sizes 64 and 100, a pre gain lane with steps and a ramp gives the expected
  gain on every sample and the static gain with reading off; the phase's lane is a step curve that switches at its
  point's sample; the limiter switch's lane plays it bypassed with its look-ahead and, once it switches it on, lowers
  loud samples with its gain instead of clamping them, and once it switches it off, plays the delayed input again bit
  for bit; section lanes check their ranges and targets, round-trip through the project file and are copied with the
  track. `tests/effects_tests.cpp` checks the bypassed limiter core itself: it only delays, and its gain is ready
  when the bypass ends. `tests/rtcheck_tests.cpp` plays section automation, the limiter's switch included, under the
  RT checker.
- `tests/mixerwindow_tests.cpp`: the setup pane shows the sections, which MIDI channels don't have; the toggles,
  knobs and the panner send `channel.set`, and the controls show values set from the engine.
