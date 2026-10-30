# Lua API

Generated from the engine's command registry; don't edit it by hand. The tests check that it is up to date: run them
with `GNUBASE_UPDATE_DOCS=1` to write it again.

Every engine command is a function of the global table `gnubase`: `track.add` is `gnubase.track.add`. Pass the
parameters as a table and get the result as a table. A failed command raises an error with the reason, which `pcall`
catches. Commands run one after the other on the script's thread, so coroutines can call them as well.

```lua
local version = gnubase.engine.version().version
local ok, problem = pcall(gnubase.track.add, {type = "nonsense"})
print(version, ok, problem)
```

## Events

- `gnubase.on(name, handler)`: calls `handler(event)` for every event with that name, or `"*"` for all, while the
  script runs commands or waits.
- `gnubase.waitFor(name, seconds)`: takes the next event with that name, waiting up to `seconds` (default 10);
  `nil` on timeout.
- `gnubase.events()`: takes every event that arrived and hasn't been taken.

An event is a table with `name`, `data` (what changed) and `request` (the command that caused it).

## Running scripts

- `gnubase --headless --script file.lua` runs a trusted script: every standard library except `os.exit`. A script
  that raises an error ends GNUbase with exit code 4.
- The MCP tool `script_run` runs untrusted code: no `io`, `os`, `debug`, `package`, `require`, `dofile` or
  `loadfile`, `load` takes source code only, and Lua code may run for at most 60 seconds. It returns what the script
  printed.

## Tests

`gnubase.test` is a small test framework. A script registers test cases, which run when the script ends, in order.
The report is printed in TAP format: `ok 1 - name`, or `not ok 1 - name` with the failure on lines starting with `#`.
If a test case fails, the script fails, so `gnubase --headless --script` exits with code 4.

```lua
local test = gnubase.test

test.before(gnubase.project.new) -- every test case starts with a new project

test.case("adding a track is one undo step", function()
	local added = gnubase.track.add{type = "audio"}
	test.equal(gnubase.project.get().tracks, {added.track})
	gnubase.history.undo()
	test.equal(gnubase.project.get().tracks, {})
end)
```

A failure names the file and line of the failing call: `tests/lua/project.lua:9: at [1].name: expected "Audio 01",
got "Audio 02"`. Errors without a position, like those of commands and `assert`, get the line in the test case that
caused them.

Assertions take an optional message last, which goes in front of the failure:

- `test.ok(value)`: anything but `false` and `nil`.
- `test.equal(actual, expected)`, `test.notEqual(actual, unexpected)`: tables are compared by their contents, and a
  failure names the first difference.
- `test.near(actual, expected, tolerance)`: numbers, or lists of numbers, within the tolerance.
- `test.matches(text, pattern)`: text that matches a Lua pattern.
- `test.raises(fn, text)`: `fn` raises an error whose message contains `text` (plain text, optional). Returns the
  message.
- `test.fail(message)`.

Fixtures:

- `test.before(fn)`, `test.after(fn)`: run before and after every test case of the script. A failing before hook skips
  the test case; after hooks run even when it fails.
- `test.fixture(setup)`: returns a function that gives the fixture's value. `setup` runs the first time a test case
  asks for it, and can return a teardown function after the value, which runs when the test case ends.
- `test.cleanup(fn)`: runs `fn` when the test case ends. Cleanups and teardowns run last first, before the after hooks.

Golden files, in trusted scripts only:

- `test.golden(name, audio, tolerance, options)`: compares `audio`, a table `{samples = {...}, channels = 1,
  sampleRate = 48000}` (interleaved samples; the defaults shown), with the golden file `<name>.wav`: 32-bit float WAV,
  like the C++ tests' golden files. A mismatch fails and writes the actual audio and the difference to the output
  folder. `options` can set `goldenDir`, `outputDir` and `update` (write the golden file instead of comparing). They
  default to the environment variables `GNUBASE_GOLDEN_DIR` (else `tests/golden`), `GNUBASE_TEST_OUTPUT_DIR` (else the
  current folder) and `GNUBASE_UPDATE_GOLDEN=1`.
- `test.readWav(path)` returns such audio from a 32-bit float WAV file; `test.writeWav(path, audio)` writes one.

Every `tests/lua/*.lua` is a ctest test with the label `lua`, which runs `gnubase --headless --script` in the source
folder. AI agents run test cases with the MCP tool `script_run`, whose output has the report.

## Commands

### audio

#### `gnubase.audio.analyze`

Measures the captured output of channels (default: every output) from frame start for frames frames (default: to the end of the capture). Per channel: the peak and RMS level, and a spectrum summary with the strongest frequencies (at most peaks, default 5, up to 20) and the power of octave bands. For the channels together: the integrated loudness in LUFS (ITU-R BS.1770), null below 400 ms. Levels are linear and in dBFS (a full-scale sine's peak is 0 dBFS, its RMS -3 dBFS); dBFS is null for silence.

Only reads; changes nothing.

Parameters:

- `channels`: array of integer
- `frames`: integer
- `peaks`: integer
- `start`: integer

Returns:

- `channels`: array of object, required
- `frames`: integer, required
- `loudness`: number or null, required
- `sampleRate`: number, required
- `start`: integer, required

#### `gnubase.audio.capture`

Captured output samples of one output channel of the dummy device, from frame start, at most 1048576 frames.

Only reads; changes nothing.

Parameters:

- `channel`: integer, required
- `frames`: integer, required
- `start`: integer, required

Returns:

- `samples`: array, required

#### `gnubase.audio.compare`

Null test: subtracts an audio file, reference, from the captured output and measures what remains. The file's channels are compared with channels (default: every output) in order, from frame start, for frames frames (default: the file's length); its sample rate must be the device's. peak is the largest difference of all channels, 0 if they cancel completely.

Only reads; changes nothing.

Parameters:

- `channels`: array of integer
- `frames`: integer
- `reference`: string, required
- `start`: integer

Returns:

- `channels`: array of object, required
- `frames`: integer, required
- `peak`: number, required
- `peakDb`: number or null, required

#### `gnubase.audio.controlPanel`

Opens the control panel of the audio device's driver. Changes made there may reset the device.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.devices`

The audio backends of this build, the preferred one first, and the devices each has now: their ids, names, channel names, sample rates, block sizes, clock sources (and the current one), whether the driver has a control panel, and whether the device is available. An unavailable device, e.g. one whose hardware isn't connected, has an unavailableReason; audio.setDevice still tries it. Some backends only learn that by opening or starting the device (ASIO). A backend that can't list its devices has an error instead.

Only reads; changes nothing.

Returns:

- `backends`: array, required

#### `gnubase.audio.get`

The audio device (its backend, id, name, format, channel names, latencies in frames, clock source and whether it has a control panel), whether it runs, how many frames it has processed and, if it is the dummy device, captured, how many frames of audio track playback were silent because a disk stream was late, and the processing threads (see audio.setProcessing).

Only reads; changes nothing.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.performance`

Performance data for the performance monitor. Loads are the share of a block's duration spent processing it, so above 1 the device drops out. load: averaged over about 0.3 s; peak: the highest block; overloads: blocks above 1; cycles: blocks measured; threads: the load of each processing thread, the audio thread first. Peak, overloads and cycles count since the last audio.resetPerformance; everything starts over with a new device or new processing threads. diskLoad: how far the disk stream furthest behind has run empty, 0 to 1; diskDropouts: frames of audio track playback that were silent because a disk stream was late.

Only reads; changes nothing.

Returns:

- `cycles`: integer, required
- `diskDropouts`: integer, required
- `diskLoad`: number, required
- `load`: number, required
- `overloads`: integer, required
- `peak`: number, required
- `threads`: array of number, required

#### `gnubase.audio.ports`

The audio device's ports by name, in order, each with the busses that use it (the bus ids); each bus's ports on this device, null where it has none (a child bus's are its parent's); and the missing ports: port names that busses use but the device doesn't have, with those busses. bus.remapPorts re-routes them. Without a device there are no ports.

Only reads; changes nothing.

Returns:

- `busses`: array, required
- `inputs`: array, required
- `missing`: array, required
- `outputs`: array, required

#### `gnubase.audio.record`

Writes the captured output of channels (default: every output) from frame start for frames frames (default: to the end of the capture) to a WAV file at path, with samples in format (default float32). Returns the file's absolute path.

Only reads; changes nothing.

Parameters:

- `channels`: array of integer
- `format`: one of "float32" | "int16" | "int24" | "int32"
- `frames`: integer
- `path`: string, required
- `start`: integer

Returns:

- `channels`: array of integer, required
- `frames`: integer, required
- `path`: string, required
- `sampleRate`: number, required

#### `gnubase.audio.render`

Offline mode: processes the given number of frames as fast as possible, then returns. Only the dummy device, and only while it is stopped.

Parameters:

- `frames`: integer, required

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.reset`

Opens the audio device again with the same settings and restarts it if it ran. Drivers ask for this when their settings change, e.g. in their control panel; the engine then resets on its own.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.resetPerformance`

Starts a new performance measurement, like clicking the overload indicator: clears the peak, the overloads and the cycles; the averaged loads go on. Immediately while the device is stopped, otherwise with its next block. Returns the measurement that ended, as audio.performance does, and sends it as the event audio.performanceReset.

Returns:

- `cycles`: integer, required
- `diskDropouts`: integer, required
- `diskLoad`: number, required
- `load`: number, required
- `overloads`: integer, required
- `peak`: number, required
- `threads`: array of number, required

#### `gnubase.audio.setDevice`

Replaces the audio device. backend: one from audio.devices, by default "dummy". device: its id, by default the backend's default device. sampleRate and blockSize: by default the driver's current rate and preferred size. clockSource: a name from the device's clockSources. The device starts stopped at position 0. Another backend's device is closed before the new one opens, since drivers often allow only one; if opening fails, there is no device.
The dummy device has no hardware: its output is captured in memory (audio.capture) and it renders offline (audio.render). Its defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 outputs (inputChannels, outputChannels), 60 s of capture (captureSeconds). Its input is silent, or a built-in generator (see processor.list) with parameters by name.

Parameters:

- `backend`: string
- `blockSize`: integer
- `captureSeconds`: number
- `clockSource`: string
- `device`: string
- `input`: object
- `inputChannels`: integer
- `outputChannels`: integer
- `sampleRate`: number

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.setProcessing`

Multi-processing: threads is how many threads process the graph, the audio thread included (1 to 64; default: half the hardware threads, about one per core). Independent tracks then run in parallel; the output is the same for any number of threads. The device must be stopped. activeThreads in the result is how many process, and limitedBy why that is fewer: multiProcessing is off, the hardware has fewer threads, worker threads didn't get real-time priority (on Linux: no real-time privileges), or the system refused to start them (system). Fails, keeping the previous settings, if threads can't be started.

Parameters:

- `multiProcessing`: boolean
- `threads`: integer

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.start`

Simulated real time: the device processes one block per block duration until stopped.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `gnubase.audio.stop`

Stops simulated real time. Does nothing if the device isn't running.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

### audioEvent

#### `gnubase.audioEvent.add`

Adds an audio event that plays a section of a clip on an audio track. position is in the track's time base (ticks or samples); offset and length are in the clip's sample frames, length defaulting to the rest of the clip. volume is in dB, from -144 to 6 (default 0); a muted event doesn't play. A track plays one event at a time: where events overlap, the one added last plays, unless they crossfade.

Parameters:

- `clip`: integer, required
- `length`: integer
- `mute`: boolean
- `offset`: integer
- `position`: integer, required
- `track`: integer, required
- `volume`: number

Returns:

- `event`: object, required

#### `gnubase.audioEvent.remove`

Removes an audio event.

Parameters:

- `event`: integer, required

#### `gnubase.audioEvent.set`

Changes an audio event's position, clip, offset, length, volume (dB) or mute.

Parameters:

- `clip`: integer
- `event`: integer, required
- `length`: integer
- `mute`: boolean
- `offset`: integer
- `position`: integer
- `volume`: number

Returns:

- `event`: object, required

#### `gnubase.audioEvent.setFades`

Changes an audio event's fade in (from its start) or fade out (up to its end): length in the clip's sample frames (0: none) and shape: linear, exponential (slow, then fast), logarithmic (fast, then slow), sCurve (half a cosine) or equalPower (a quarter sine). Together they fit into the event. A crossfade replaces the fades at the ends it joins while it plays.

Parameters:

- `event`: integer, required
- `fadeIn`: object
- `fadeOut`: object

Returns:

- `event`: object, required

### autoFades

#### `gnubase.autoFades.set`

Changes the auto fades and crossfades of the project, or of an audio track: fadeIn and fadeOut where an event has no fade of its own at that end (they never stack), crossfades where an event starts on top of another one or ends above one that goes on, unless they crossfade already; there an event's own fade is the crossfade's, mirrored by the event behind. length is 1 to 500 ms; shapes as for audioEvent.setFades; crossfadeMode equalGain or equalPower, with crossfadeShape the fade in as for crossfade.set. A track uses the project's settings until it gets its own; useProject: true goes back to them. Returns the settings that apply.

Parameters:

- `crossfadeMode`: one of "equalGain" | "equalPower" | "custom"
- `crossfadeShape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `crossfades`: boolean
- `fadeIn`: boolean
- `fadeInShape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `fadeOut`: boolean
- `fadeOutShape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `length`: number
- `track`: integer
- `useProject`: boolean

Returns:

- `autoFades`: object, required

### automation

#### `gnubase.automation.get`

The automation settings. followsEvents (default false): when an audio event or MIDI part moves, the automation of its track in its range moves with it and replaces the points where it lands.

Only reads; changes nothing.

Returns:

- `followsEvents`: boolean, required

#### `gnubase.automation.set`

Changes the automation settings (see automation.get). A setting, so it stays when a new project starts.

Parameters:

- `followsEvents`: boolean

Returns:

- `followsEvents`: boolean, required

### automationLane

#### `gnubase.automationLane.add`

Adds an automation lane to a track: the curve of one parameter. target.type says which: volume (the channel's fader, in dB), pan, panLeft and panRight (-1 to 1, see channel.set) on tracks with audio channels; instrument, a parameter of an instrument track's built-in instrument (target.parameter, an id from processor.list, in its range); controller (target.number 0-127), pitchBend (0.5 is no bend) and channelPressure, all 0 to 1, on MIDI and instrument tracks, with target.channel 0-15 (default 0). A track has one lane per target. read (default true): the curve plays; without it, or without points, the parameter keeps its static value.

Parameters:

- `read`: boolean
- `target`: object, required
- `track`: integer, required

Returns:

- `lane`: object, required

#### `gnubase.automationLane.list`

The automation lanes of a track, or of every track in track order, with their points sorted by position.

Only reads; changes nothing.

Parameters:

- `track`: integer

Returns:

- `lanes`: array, required

#### `gnubase.automationLane.remove`

Removes an automation lane with its points.

Parameters:

- `lane`: integer, required

#### `gnubase.automationLane.set`

Turns reading an automation lane on or off.

Parameters:

- `lane`: integer, required
- `read`: boolean

Returns:

- `lane`: object, required

### automationPoint

#### `gnubase.automationPoint.add`

Adds points to an automation lane: position in the track's time base (ticks on musical tracks, so automation follows tempo changes; samples on linear ones, so it keeps its time), value in the target's unit, shape of the curve to the next point (step, linear or curve; default linear) and for curves the bend, curve, from -1 to 1 (above 0 the value moves fast at first). Before the first point the curve has its value, after the last point the last one's; points at one position make a jump. A ramp of volume moves the gain in a straight line.

Parameters:

- `lane`: integer, required
- `points`: array of object, required

Returns:

- `points`: array, required

#### `gnubase.automationPoint.remove`

Removes points from an automation lane.

Parameters:

- `lane`: integer, required
- `points`: array of integer, required

#### `gnubase.automationPoint.set`

Changes points of an automation lane: their position, value, shape or curve.

Parameters:

- `lane`: integer, required
- `points`: array of object, required

Returns:

- `points`: array, required

### bus

#### `gnubase.bus.add`

Adds an input or output bus. format is its speaker arrangement (default stereo): mono, stereo, or a surround format with a fixed speaker order, e.g. 5.1 as L R C LFE Ls Rs. channels (1 to 8) picks mono, stereo, LRC, quadro, 5.0, 5.1, 7.0 or 7.1 instead. Without a name it is named after its format and direction, e.g. "Stereo In 2". ports: the device port of each channel by name (see audio.ports), an empty name for none; without ports the bus takes consecutive device ports in bus order. A port can serve several busses. The first output bus becomes the main mix. A child bus routes to some channels of its parent, a top-level bus of the same direction with more channels (direction may be left out): parentChannels are the parent's channel indices, one per channel of the child, by default the speakers both formats have; it uses its parent's ports.

Parameters:

- `channels`: integer
- `direction`: one of "input" | "output"
- `format`: one of "mono" | "stereo" | "LRC" | "LRS" | "LRC+LFE" | "LRS+LFE" | "LRCS" | "LRCS+LFE" | "quadro" | "quadro+LFE" | "5.0" | "5.1" | "6.0 cine" | "6.0 music" | "7.0" | "7.1"
- `name`: string
- `parent`: integer
- `parentChannels`: array of integer
- `ports`: array of string

Returns:

- `bus`: object, required

#### `gnubase.bus.applyPreset`

Replaces the project's busses of a direction with a preset's (see bus.presets). Channels that used the old busses move to the new bus at the same place, or to none (inputs) or the main mix (outputs). Returns the new busses.

Parameters:

- `direction`: one of "input" | "output", required
- `name`: string, required

Returns:

- `busses`: array, required

#### `gnubase.bus.presets`

The bus presets of a direction: standard ones with one bus of a common format, hardware ones made from the audio device's ports (all mono, or stereo pairs), and the user's own (bus.savePreset), which hide others of their name. Each has a name, a kind and its busses: name, format, ports and children (name, format, parentChannels).

Only reads; changes nothing.

Parameters:

- `direction`: one of "input" | "output", required

Returns:

- `presets`: array, required

#### `gnubase.bus.remapPorts`

Re-routes ports, e.g. missing ones (see audio.ports) after opening a project made on other hardware: ports maps port names to the names that replace them in every bus of the direction. Returns the busses that changed.

Parameters:

- `direction`: one of "input" | "output", required
- `ports`: object, required

Returns:

- `busses`: array, required

#### `gnubase.bus.remove`

Removes a bus and its child busses; channels connected to them are disconnected. Removing the main mix makes the next output bus the main mix.

Parameters:

- `bus`: integer, required

#### `gnubase.bus.removePreset`

Removes one of the user's own bus presets.

Parameters:

- `direction`: one of "input" | "output", required
- `name`: string, required

#### `gnubase.bus.rename`

Renames several busses with an incrementing number or letter: name ends in a number ("Out 1", "Out 01") or in a space and a capital letter ("Bus A", up to Z). The first bus given gets the name as given, the others follow in bus order from there, continuing from the top.

Parameters:

- `busses`: array of integer, required
- `name`: string, required

Returns:

- `busses`: array, required

#### `gnubase.bus.savePreset`

Saves the project's busses of a direction as a user preset, for every project (the user setting audio.busPresets), replacing one of the same name. A preset named Default is applied to new projects.

Parameters:

- `direction`: one of "input" | "output", required
- `name`: string, required

#### `gnubase.bus.set`

Renames a bus, changes its ports (as in bus.add; [] for consecutive ports in bus order) or makes it the main mix (mainMix: true), the output bus new channels are routed to and that is monitored.

Parameters:

- `bus`: integer, required
- `mainMix`: boolean
- `name`: string
- `ports`: array of string

Returns:

- `bus`: object, required

### channel

#### `gnubase.channel.set`

Changes a channel's format (or channels, as in bus.add), input (an input bus) or output (an output bus or a group channel; for MIDI tracks an instrument channel). 0 disconnects. Audio converts between formats where a channel feeds a destination of another one: speakers both formats have connect, the others are mixed down (5.1 to stereo: C and LFE into L and R at -3 dB, Ls into L and Rs into R at -6 dB; 7.1 to 5.1: Sl and Sr into the front and surround sides at -3 dB) or up (mono to C, or to L and R at the pan law's center). instrument is the built-in instrument an instrument track plays (see processor.list), or empty for none. volume is the fader in dB, from -144 (silence) to the project's maxVolume (default 0); values beyond them stop there. The panner depends on the routing: a mono channel on a stereo destination has its position pan, -1 (left) to 1 (right), placed with the project's pan law; a stereo channel on a stereo or mono destination has the balance panner (panner balance, the default: pan turns down the other side) or the combined panner (panner combined: panLeft and panRight place its left and right channel, by default -1 and 1). panBypass plays mono channels at the center and stereo ones hard left and right. Destinations wider than stereo have no panner yet. Automation of volume, pan, panLeft and panRight overrides them while it plays; changes glide over 5 ms. MIDI channels have no audio: midiVolume and midiPan (0 to 127, 64 is the center pan, null for none, the default) are sent to the instrument as MIDI volume (CC 7) and pan (CC 10) on MIDI channel 1, whenever they change and when playback starts or jumps; automation of those controllers overrides them.

Parameters:

- `channel`: integer, required
- `channels`: integer
- `format`: one of "mono" | "stereo" | "LRC" | "LRS" | "LRC+LFE" | "LRS+LFE" | "LRCS" | "LRCS+LFE" | "quadro" | "quadro+LFE" | "5.0" | "5.1" | "6.0 cine" | "6.0 music" | "7.0" | "7.1"
- `input`: integer
- `instrument`: string
- `midiPan`: integer or null
- `midiVolume`: integer or null
- `output`: integer
- `pan`: number
- `panBypass`: boolean
- `panLeft`: number
- `panRight`: number
- `panner`: one of "balance" | "combined"
- `volume`: number

Returns:

- `channel`: object, required

### clip

#### `gnubase.clip.add`

Adds a clip that plays an audio file; it is named after the file unless a name is given.

Parameters:

- `media`: integer, required
- `name`: string

Returns:

- `clip`: object, required

#### `gnubase.clip.remove`

Removes a clip that no audio event uses.

Parameters:

- `clip`: integer, required

#### `gnubase.clip.setFades`

Changes a clip's fade in or fade out, at its media file's start and end: length in sample frames (0: none) and shape, as for audioEvent.setFades. Every event that plays the clip gets them.

Parameters:

- `clip`: integer, required
- `fadeIn`: object
- `fadeOut`: object

Returns:

- `clip`: object, required

### controller

#### `gnubase.controller.add`

Adds controller events to a MIDI part: position from the part's start in ticks, MIDI channel 0-15 (default 0). type (default controller) says what else an event has: controller: number 0-127 and value 0-1; pitchBend: value 0-1, 0.5 is no bend; channelPressure: value; polyPressure: number (the pitch) and value; program: number (the program) and optionally bank 0-16383; sysEx: data, the message's bytes from 0xF0 to 0xF7.

Parameters:

- `controllers`: array of object, required
- `part`: integer, required

Returns:

- `controllers`: array, required

#### `gnubase.controller.remove`

Removes controller events from a MIDI part.

Parameters:

- `controllers`: array of integer, required
- `part`: integer, required

#### `gnubase.controller.set`

Changes controller events of a MIDI part: each change names a controller event by id and gives what changes: position in ticks, type, and the fields of the type as controller.add takes them (number, value, channel, bank, data). A field the type doesn't have is an error, such as value on a program change; a null bank removes it. A new type keeps the fields it shares with the old one and needs the others. Returns the changed controller events.

Parameters:

- `controllers`: array of object, required
- `part`: integer, required

Returns:

- `controllers`: array, required

### crossfade

#### `gnubase.crossfade.remove`

Removes the crossfades into and out of an audio event.

Parameters:

- `event`: integer, required

#### `gnubase.crossfade.set`

Crossfades from an audio event into the next one on its track, where they overlap: to must start inside from and end after it. Both play there; from fades out up to its end over outLength (from's clip frames), to fades in from its start over length (to's clip frames); 0, the default, is the whole overlap, so the crossfade is symmetric. mode equalGain (default): the fade out mirrors the fade in (shape linear, exponential, logarithmic or sCurve), so their gains add up to 1; equalPower: quarter sines whose power adds up to 1; custom: shape and outShape of their own. An event fades out into one event only. Changing a crossfade keeps what isn't given. After an edit that moves the events apart, the crossfade rests and both play their own fades until they overlap like that again. Returns to.

Parameters:

- `from`: integer, required
- `length`: integer
- `mode`: one of "equalGain" | "equalPower" | "custom"
- `outLength`: integer
- `outShape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `shape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `to`: integer, required

Returns:

- `event`: object, required

### engine

#### `gnubase.engine.commandLog`

The last 256 commands that ran, oldest first, without queries: what the UI and other clients did. Each with its request id (events name it too), the client that sent it (ui, mcp, lua, app for the command line, engine for the engine's own commands), the command, its parameters, and the error if it failed.

Only reads; changes nothing.

Returns:

- `commands`: array of object, required

#### `gnubase.engine.commands`

Every command with its documentation, parameters and result.

Only reads; changes nothing.

Returns:

- `commands`: array, required

#### `gnubase.engine.version`

The GNUbase version.

Only reads; changes nothing.

Returns:

- `version`: string, required

### graph

#### `gnubase.graph.get`

The project's audio processing graph: its nodes (busses, then channels in track order) with the nodes they receive audio and events from, and the order they are processed in.

Only reads; changes nothing.

### history

#### `gnubase.history.describe`

Sets the details of a history step, a note for the user.

Parameters:

- `details`: string, required
- `step`: integer, required

#### `gnubase.history.get`

The undo history: every step with its action, time (UTC) and details, the position (steps before it are done, the ones after it can be redone) and the maximum number of steps (0: no limit).

Only reads; changes nothing.

#### `gnubase.history.redo`

Redoes the next undone step, or the given number of steps.

Parameters:

- `steps`: integer

Returns:

- `position`: integer, required

#### `gnubase.history.set`

Sets the maximum number of undo steps (0: no limit). The oldest steps beyond it are dropped. A setting, so it stays when a new project starts.

Parameters:

- `maxSteps`: integer, required

#### `gnubase.history.transaction`

Runs document commands in order as one undo step named description. If one fails, none of them changes the project. Returns each command's result.

Parameters:

- `commands`: array of object, required
- `description`: string, required

Returns:

- `results`: array, required

#### `gnubase.history.undo`

Undoes the last step, or the given number of steps.

Parameters:

- `steps`: integer

Returns:

- `position`: integer, required

### job

#### `gnubase.job.cancel`

Asks a running job to stop. It ends soon after as cancelled, with a job.finished event, and cleans up what it made; a job that has ended stays as it is.

Parameters:

- `job`: integer, required

Returns:

- `job`: object, required

#### `gnubase.job.get`

A background job, as job.list shows it.

Only reads; changes nothing.

Parameters:

- `job`: integer, required

Returns:

- `job`: object, required

#### `gnubase.job.list`

Every background job since the engine started, such as backups: id, kind, state (running, done, failed or cancelled), progress from 0 to 1, what it is doing, and its result or error once it ended.

Only reads; changes nothing.

Returns:

- `jobs`: array of object, required

### marker

#### `gnubase.marker.activateTrack`

Makes a marker track the active one, which marker commands use when they are given no track.

Parameters:

- `track`: integer, required

#### `gnubase.marker.add`

Adds a position marker, or with type cycle a cycle marker, to a marker track (default: the active one; without a marker track one is added). position and end are in the track's time base (ticks or samples) or "cursor". A position marker defaults to the cursor, a cycle marker to the locators; a cycle marker ends at least one sample after its position. Position and cycle markers are numbered separately on each track; a new marker gets the first free number unless one is given.

Parameters:

- `color`: string or null
- `end`: integer or string
- `name`: string
- `number`: integer
- `position`: integer or string
- `track`: integer
- `type`: one of "position" | "cycle"

Returns:

- `marker`: object, required

#### `gnubase.marker.list`

The markers of a marker track (default: the active one; track 0 if there is none) in timeline order, with their position and end also in the project's samples.

Only reads; changes nothing.

Parameters:

- `track`: integer

Returns:

- `markers`: array, required
- `track`: integer, required

#### `gnubase.marker.locate`

Moves the project cursor to a marker, given by id (marker) or by its number on a marker track (default: the active one) and type (default: position). Cycle markers locate to their start.

Parameters:

- `marker`: integer
- `number`: integer
- `track`: integer
- `type`: one of "position" | "cycle"

Returns:

- `marker`: object or null, required
- `position`: integer, required

#### `gnubase.marker.locateNext`

Moves the project cursor to the next marker after it on a marker track (default: the active one): position markers and the start and end of cycle markers. Without one the cursor stays and marker is null. Returns the marker and the cursor in samples.

Parameters:

- `track`: integer

Returns:

- `marker`: object or null, required
- `position`: integer, required

#### `gnubase.marker.locatePrevious`

Moves the project cursor to the previous marker before it on a marker track (default: the active one), like marker.locateNext.

Parameters:

- `track`: integer

Returns:

- `marker`: object or null, required
- `position`: integer, required

#### `gnubase.marker.range`

The range between the markers around a position in samples, on a marker track (default: the active one): from the last marker at or before it to the next one, counting position markers and the start and end of cycle markers. A range selection between markers spans it.

Only reads; changes nothing.

Parameters:

- `position`: integer, required
- `track`: integer

Returns:

- `end`: integer, required
- `start`: integer, required

#### `gnubase.marker.remove`

Removes markers.

Parameters:

- `markers`: array of integer, required

#### `gnubase.marker.renumber`

Numbers a marker track's position or cycle markers again from 1, in timeline order (default: the active track).

Parameters:

- `track`: integer
- `type`: one of "position" | "cycle", required

#### `gnubase.marker.set`

Changes a marker: moves it (position, in the track's time base or "cursor"; a cycle marker keeps its length unless end is given), renames it, changes its color or number, or moves it to another marker track, where it takes the first free number if its own is taken.

Parameters:

- `color`: string or null
- `end`: integer or string
- `marker`: integer, required
- `name`: string
- `number`: integer
- `position`: integer or string
- `track`: integer

Returns:

- `marker`: object, required

#### `gnubase.marker.setLocators`

Sets the left and right locators to a cycle marker's start and end, given by id (marker) or by its number on a marker track (default: the active one).

Parameters:

- `marker`: integer
- `number`: integer
- `track`: integer

Returns:

- `left`: integer, required
- `marker`: object, required
- `right`: integer, required

### media

#### `gnubase.media.add`

Adds an audio file. Its sample rate, channel count and length in sample frames are read from the file, or taken as given, for a file that is still being recorded. With copy, a file from outside the project folder is also copied into its media folder (see project.location); the project must have been saved. The copy runs as a job, returned as job (null without one), with the result that media.prepareArchive's job has: the media file is added at once where it is, and when the copy is done it is pointed at the copy as an undo step of its own.

Parameters:

- `channels`: integer
- `copy`: boolean
- `length`: integer
- `path`: string, required
- `sampleRate`: number

Returns:

- `job`: object or null, required
- `media`: object, required

#### `gnubase.media.convert`

Converts a media file to another sample rate (by default the project's), writes the result to path as a 32-bit float WAV file and adds it as a new media file. path must not be one of the project's media files.

Parameters:

- `media`: integer, required
- `path`: string, required
- `sampleRate`: number

Returns:

- `media`: object, required

#### `gnubase.media.peakStatus`

The state of the waveform overviews of a media file, or of every media file: pending, generating (with progress from 0 to 1), ready, failed (with the error) or recording; the frames that have peaks; the peak file they were loaded from or written to; whether they were generated from the audio file rather than loaded. Peak files are kept next to their audio file as <file name>.gbpeaks, or in fallbackFolder where that can't be written.

Only reads; changes nothing.

Parameters:

- `media`: integer

Returns:

- `fallbackFolder`: string, required
- `media`: array of object, required

#### `gnubase.media.peaks`

The waveform overview of a media file for drawing: frames sample frames of the file from start (default: the rest of the file), in pixels columns (up to 65536). Per channel (default: all of them; channels picks some, numbered from 0), the smallest and largest sample of each column, from -32767 to 32767 (full scale). Pixels times channels can be up to 262144. The columns come from the coarsest level of the file's peaks (level: 256, 4096 or 65536 frames per peak) that has a peak per column, so they cost the same at any length. status says whether the peaks are ready; while they are generated, only the first available frames have columns and the rest are 0. A file that changed since its peaks were made gets new ones; media.peaksProgress and media.peaksReady report how that goes.

Only reads; changes nothing.

Parameters:

- `channels`: array of integer
- `frames`: integer
- `media`: integer, required
- `pixels`: integer, required
- `start`: integer

Returns:

- `available`: integer, required
- `channels`: array of object, required
- `level`: integer, required
- `media`: integer, required
- `status`: one of "pending" | "generating" | "ready" | "failed" | "recording", required

#### `gnubase.media.prepareArchive`

Makes the project self-contained, so that its folder can be archived or moved: copies every media file from outside the project folder into its media folder and points the media at the copies. The copies run as a job: this returns at once, job.progress and job.finished events follow, and job.cancel stops it and removes the copies. Once they are done, the media are pointed at them as one undo step. The job's result lists the copies as copied, media files that couldn't be read as missing (they keep their paths), and media whose path changed while the job ran as changed (they keep the new path). The project must have been saved; save it again without embedding media to keep the new paths.

Returns:

- `job`: object, required

#### `gnubase.media.relink`

Points media files at other files, such as copies, as one undo step. Each new file must be an audio file with the media file's sample rate and channel count. An entry with from applies only if the media file is still there and still points at from; otherwise it is skipped. Returns the media files it relinked and the ones it skipped.

Parameters:

- `files`: array of object, required

Returns:

- `relinked`: array of integer, required
- `skipped`: array of integer, required

#### `gnubase.media.remove`

Removes an audio file that no clip uses.

Parameters:

- `media`: integer, required

#### `gnubase.media.stretch`

Time-stretches and pitch-shifts a media file, writes the result to path as a 32-bit float WAV file and adds it as a new media file; path must not be one of the project's media files. ratio is the new length over the old one, from 0.1 to 10 (default 1: unchanged); semitones and cents shift the pitch, together within 48 semitones either way (default 0). preset picks the algorithm: quality (default) and efficient for any material, drums, plucked, pads, vocals, mix and solo tuned for kinds of material at a lower CPU cost. formants keeps the spectral envelope when the pitch shifts, e.g. for voices. mode offline (default) analyzes the whole file first for the best result; realtime processes it the way playback does, with ratios from 0.25 to 4 and pitch shifts within 24 semitones.

Parameters:

- `cents`: number
- `formants`: boolean
- `media`: integer, required
- `mode`: one of "realtime" | "offline"
- `path`: string, required
- `preset`: one of "quality" | "efficient" | "drums" | "plucked" | "pads" | "vocals" | "mix" | "solo"
- `ratio`: number
- `semitones`: number

Returns:

- `media`: object, required

### midi

#### `gnubase.midi.bounce`

Bounces MIDI parts on one track into a single part from the first one's start to the last one's end, named after the first. Events keep their time; notes are cut where their part ended. Returns the tracks it added, the parts it added (parts), changed and removed.

Parameters:

- `parts`: array of integer, required

Returns:

- `changedParts`: array of integer, required
- `parts`: array of integer, required
- `removedParts`: array of integer, required
- `tracks`: array of integer, required

#### `gnubase.midi.deleteContinuousControllers`

Removes the control changes of continuous controllers. Bank select, data entry, the switches 64-69 (such as the sustain pedal), data increment and decrement, NRPN, RPN and channel mode messages stay. Works on the MIDI parts in parts. Returns the parts it changed.

Parameters:

- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.deleteControllers`

Removes control changes, pitch bend, channel and poly pressure; program changes and SysEx stay. Works on the MIDI parts in parts. Returns the parts it changed.

Parameters:

- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.deleteDoubles`

Of notes with the same pitch, channel and position, keeps the longest (the first added on a tie). Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `notes`: array of integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.deleteNotes`

Deletes notes shorter than minimumLength (ticks) or softer than minimumVelocity (0-1); at least one is required. removeWhen both deletes only notes below both minimums (default: either). Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `minimumLength`: integer
- `minimumVelocity`: number
- `notes`: array of integer
- `parts`: array of integer, required
- `removeWhen`: one of "either" | "both"

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.deleteOverlaps`

Shortens notes so that none still sounds when the next one on its channel starts: in mode mono the next one of the same pitch, in mode poly of any pitch. Notes that start together don't overlap. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `mode`: one of "mono" | "poly", required
- `notes`: array of integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.dissolve`

Dissolves MIDI parts by MIDI channel or by pitch (by: channel or pitch): for each channel or pitch found in the parts of a track, a new MIDI track after it, named after the track and the channel or pitch (middle C, 60, is C3), routed to the same instrument, gets the events of that channel or pitch in parts at the same place. By channel SysEx stays, by pitch everything but notes and poly pressure; parts left empty are removed. optimize shortens the new parts to the bars their events play in. Returns the tracks it added, the parts it added (parts), changed and removed.

Parameters:

- `by`: one of "channel" | "pitch", required
- `optimize`: boolean
- `parts`: array of integer, required

Returns:

- `changedParts`: array of integer, required
- `parts`: array of integer, required
- `removedParts`: array of integer, required
- `tracks`: array of integer, required

#### `gnubase.midi.fixLengths`

Sets the length of notes, in ticks. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `length`: integer, required
- `notes`: array of integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.fixVelocities`

Sets the velocity of notes, 0-1. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `notes`: array of integer
- `parts`: array of integer, required
- `velocity`: number, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.invert`

Mirrors notes in time: each one starts where its mirror image ends. Without notes, controllers with a value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, and return to their last value where the range ends, or on the part's last tick if the range reaches its end; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `notes`: array of integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.legato`

Extends notes to the start of the next note on their MIDI channel, plus overlap in ticks (negative: a gap; default 0). With betweenSelected, to the next of the given notes. The last notes keep their length. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `betweenSelected`: boolean
- `notes`: array of integer
- `overlap`: integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.merge`

Merges the notes and controllers that start between start and end (project samples, like the locators, compared in samples as playback does) on the MIDI and instrument tracks in tracks (default: all of them) into a new part on the destination track from start, or from the first merged event if that is earlier, to end, named name (default: the track's name). Notes keep their length, cut where their part ends. Without destination a new MIDI track is added at the end, routed to the instrument of the first track merged. eraseDestination first removes the destination's parts that lie between start and end, and the events of its other parts that start there. includeChase adds the last value of every controller and program before start at the new part's start. Returns the tracks it added, the parts it added (parts), changed and removed.

Parameters:

- `destination`: integer
- `end`: integer, required
- `eraseDestination`: boolean
- `includeChase`: boolean
- `name`: string
- `start`: integer, required
- `tracks`: array of integer

Returns:

- `changedParts`: array of integer, required
- `parts`: array of integer, required
- `removedParts`: array of integer, required
- `tracks`: array of integer, required

#### `gnubase.midi.monitor`

The MIDI input received since the last call from every open input, at most 4096 messages, each with its port, its frame position on the audio device and its bytes. dropped counts what didn't fit. Input reaches the monitor while an audio device processes blocks, which places it in time.

Only reads; changes nothing.

Returns:

- `dropped`: integer, required
- `received`: array, required

#### `gnubase.midi.pedalToNoteLength`

Extends notes that end while the sustain pedal (controller 64) on their channel is down to where it is released (the part's end if never), but not past the next note of the same pitch, and removes the sustain pedal events of their channels. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `notes`: array of integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.ports`

The MIDI ports of every MIDI backend, inputs and outputs: backend, id, system name, the name they are shown as (showAs), whether they are visible, whether an input belongs to all MIDI inputs, whether the port is present now and open, and whether the system speaks MIDI 2.0 with it. Ports that were unplugged stay listed with their settings, not present.

Only reads; changes nothing.

Returns:

- `ports`: array, required

#### `gnubase.midi.refresh`

Lists the MIDI ports again: opens new and returning visible inputs and marks unplugged ports as not present. Runs on its own when a backend reports that its ports changed.

Returns:

- `ports`: array, required

#### `gnubase.midi.restrictPolyphony`

Shortens notes so that at most voices (1 or more) sound at once on each MIDI channel: a new note ends the one that started first, the lowest of notes that started together. A note that would lose its whole length is deleted. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `notes`: array of integer
- `parts`: array of integer, required
- `voices`: integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.reverse`

Reverses the order in which notes start, keeping their lengths: the last one starts where the first one started. Without notes, controllers with a value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, and return to their last value where the range ends, or on the part's last tick if the range reaches its end; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `notes`: array of integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.setPort`

Changes a MIDI port's settings. The port is given by its id, whether it is an input, and its backend if ids repeat. showAs: the name it is shown as; empty shows the system name. visible: hidden inputs are closed. inAllInputs, for inputs: whether tracks set to all MIDI inputs receive it. Returns the port.

Parameters:

- `backend`: string
- `id`: string, required
- `inAllInputs`: boolean
- `input`: boolean, required
- `showAs`: string
- `visible`: boolean

#### `gnubase.midi.thinOut`

Thins out continuous controllers, pitch bend and pressure, per controller and channel: removes events that repeat the value before them, and events closer than minimumDistance ticks (default a 64th note) to the event kept before them, unless the curve turns there, the next event is at least minimumDistance away or it is the last one. Bank select, data entry, the switches 64-69, NRPN, RPN and channel mode messages stay, like program changes and SysEx. Works on the MIDI parts in parts. Returns the parts it changed.

Parameters:

- `minimumDistance`: integer
- `parts`: array of integer, required

Returns:

- `parts`: array of integer, required

#### `gnubase.midi.transpose`

Transposes the notes of MIDI parts by semitones (-127 to 127). fromScale and toScale (a root 0-11, 0 is C, and a scale type) correct the notes from one scale to another: a note moves from its degree in fromScale to the same degree in toScale, and notes outside fromScale, or all of them when the scales have a different number of degrees, go to the closest note of toScale. lowest and highest (0-127) keep notes in range: a note that leaves it moves by octaves into it, or to the limit it crossed if no octave fits. Notes stay within 0-127 by octaves. Poly pressure follows when no notes are given. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Parameters:

- `fromScale`: object
- `highest`: integer
- `lowest`: integer
- `notes`: array of integer
- `parts`: array of integer, required
- `semitones`: integer
- `toScale`: object

Returns:

- `parts`: array of integer, required

### midiFile

#### `gnubase.midiFile.export`

Exports MIDI and instrument tracks (tracks, default all of them) as a standard MIDI file of type 0 (one track with every MIDI channel) or 1 (default: a tempo track, then a track per exported track), with ppq ticks per quarter note (24-960, default 480). The file has the notes and controllers as they play, converted to MIDI 1.0, the track names, tempo and time signatures, and the markers unless markers is false. A note ends where the next note of its pitch and channel starts, so that overlapping notes read back the same. range {start, end} in ticks exports only what starts in it, moved to the file's start. Returns the number of tracks in the file.

Only reads; changes nothing.

Parameters:

- `markers`: boolean
- `path`: string, required
- `ppq`: integer
- `range`: object
- `tracks`: array of integer
- `type`: one of 0 | 1

Returns:

- `tracks`: integer, required

#### `gnubase.midiFile.import`

Imports a standard MIDI file (type 0, 1 or 2) into new MIDI tracks (trackType midi, the default) or instrument tracks, one part per track from the bar of its first event to the bar after its last. A type 0 file gets a track per MIDI channel; the sequences of a type 2 file are laid out one after another, each from a bar start. Names that aren't UTF-8 are read as Windows-1252. Unless tempo is false, the file's tempo and time signatures replace the project's; unless markers is false, its markers are added to the active marker track (one is added if there is none). One undo step. Returns the new tracks and parts.

Parameters:

- `markers`: boolean
- `path`: string, required
- `tempo`: boolean
- `trackType`: one of "midi" | "instrument"

Returns:

- `parts`: array, required
- `tracks`: array, required

### midiPart

#### `gnubase.midiPart.add`

Adds a MIDI part to a MIDI or instrument track. position and length are in the track's time base.

Parameters:

- `length`: integer, required
- `name`: string
- `position`: integer, required
- `track`: integer, required

Returns:

- `part`: object, required

#### `gnubase.midiPart.remove`

Removes a MIDI part with its notes and controllers.

Parameters:

- `part`: integer, required

#### `gnubase.midiPart.set`

Changes a MIDI part's name, position or length.

Parameters:

- `length`: integer
- `name`: string
- `part`: integer, required
- `position`: integer

Returns:

- `part`: object, required

### note

#### `gnubase.note.add`

Adds notes to a MIDI part: position from the part's start and length in ticks, pitch 0-127, velocity 0-1, release (note-off) velocity 0-1 (default 0.5), MIDI channel 0-15 (default 0).

Parameters:

- `notes`: array of object, required
- `part`: integer, required

Returns:

- `notes`: array, required

#### `gnubase.note.remove`

Removes notes from a MIDI part.

Parameters:

- `notes`: array of integer, required
- `part`: integer, required

#### `gnubase.note.set`

Changes notes of a MIDI part: each change names a note by id and gives what changes, position and length in ticks, pitch 0-127, velocity 0-1, release velocity 0-1 or MIDI channel 0-15. Returns the changed notes.

Parameters:

- `notes`: array of object, required
- `part`: integer, required

Returns:

- `notes`: array, required

### pluginhost

#### `gnubase.pluginhost.check`

Reports the plugins that failed since the last check because their host process crashed, missed a processing deadline (reason "hung") or broke the protocol, and emits plugin.failed for each with its instance, plugin id, process and reason. A failed plugin is bypassed and outputs silence. Also emits pluginhost.changed for each hosting mode change whose moves are done. The engine runs this on its own as soon as a failure is detected or the moves are done.

Returns:

- `failures`: array of object, required

#### `gnubase.pluginhost.get`

How plugins are grouped into plugin host processes, and the processes. mode: the hosting mode, "shared" (one process for every plugin), "perVendor", "perPlugin" or "perInstance"; defaultMode: the mode GNUbase starts with; overrides: plugin ids with a mode of their own. hosts: each process with its id (process), its group ("shared", "vendor:<vendor>", "plugin:<id>" or "instance:<instance>"), its state ("running", "hung" or "exited") and its plugin instances, whether they failed.

Only reads; changes nothing.

Returns:

- `defaultMode`: string, required
- `errors`: array of string
- `hosts`: array of object, required
- `mode`: string, required
- `overrides`: object, required

#### `gnubase.pluginhost.setMode`

Sets the hosting mode (see pluginhost.get) and returns at once. Plugins whose process changes then move to their new process in the background, with their state, between two blocks, so they keep sounding the same. When they did, pluginhost.changed is emitted with the new status, and with the plugins that could not move and stay where they are in errors.

Parameters:

- `mode`: one of "shared" | "perVendor" | "perPlugin" | "perInstance", required

Returns:

- `defaultMode`: string, required
- `errors`: array of string
- `hosts`: array of object, required
- `mode`: string, required
- `overrides`: object, required

#### `gnubase.pluginhost.setOverride`

Gives a plugin (its id, e.g. "clap:com.example.synth") a hosting mode of its own, e.g. "perInstance" to isolate an unstable plugin, or removes its override with null. Moves the plugin's instances like pluginhost.setMode, which emits pluginhost.changed when they moved.

Parameters:

- `mode`: one of "shared" | "perVendor" | "perPlugin" | "perInstance" | null, required
- `plugin`: string, required

Returns:

- `defaultMode`: string, required
- `errors`: array of string
- `hosts`: array of object, required
- `mode`: string, required
- `overrides`: object, required

### processor

#### `gnubase.processor.list`

Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units. A stepped parameter takes whole numbers only; its options, if any, name its values from the minimum on.

Only reads; changes nothing.

Returns:

- `processors`: array, required

### project

#### `gnubase.project.backup`

Backs the project up as it is now into folder, which must be empty or new: the project file, named name (by default after the project file, or else the project), and copies of its media in the folder Audio, which the project file refers to relative to itself, so that the folder opens on another machine. The project itself stays as it is. minimize keeps only the ranges of audio files that audio events play, with handles seconds (default 1) before and after; removeUnused leaves out clips that no audio event uses and media that no clip uses. currentVersionsOnly and skipVideo have no effect until clips have versions and projects have video. activate opens the backup once it is done, unless the project changed meanwhile. The backup runs as a job: this returns at once, job.progress and job.finished events follow, and job.cancel stops it and removes what it wrote. A folder that a running job writes into can't be used. The job's result is {file, files, missing, activated, activationError}: missing lists the media files that couldn't be read and keep their old paths; activated says whether the backup was opened, and activationError why not, if it was asked to be.

Parameters:

- `activate`: boolean
- `currentVersionsOnly`: boolean
- `folder`: string, required
- `handles`: number
- `minimize`: boolean
- `name`: string
- `removeUnused`: boolean
- `skipVideo`: boolean

Returns:

- `job`: object, required

#### `gnubase.project.get`

The whole project: busses, tracks in order, channels, media, clips, events and parts.

Only reads; changes nothing.

#### `gnubase.project.load`

Replaces the project with a DAWproject file and clears the undo history. Media files embedded in it are extracted to mediaFolder, by default a folder named after the file next to it. With revision, it loads only if the project is still at that revision (see project.revision), and fails otherwise, so that no change made meanwhile is lost.

Parameters:

- `mediaFolder`: string
- `path`: string, required
- `revision`: integer

#### `gnubase.project.location`

Where the project is: its project file (set by project.save and project.load), the folder of that file, and the media folder in it where media.add with copy and media.prepareArchive put audio files. All null for a project that has no file yet.

Only reads; changes nothing.

Returns:

- `file`: string or null, required
- `folder`: string or null, required
- `mediaFolder`: string or null, required

#### `gnubase.project.new`

Replaces the project with an empty one that has a stereo input and output bus, or the busses of the user's bus presets named Default (see bus.savePreset), and clears the undo history.

#### `gnubase.project.revision`

A number that grows with every change of the project: every edit, undo and redo, and loading or starting a project. project.load with revision loads only if it hasn't changed.

Only reads; changes nothing.

Returns:

- `revision`: integer, required

#### `gnubase.project.save`

Saves the project as a DAWproject file. Media files are copied into it unless embedMedia is false; then the file refers to them where they are.

Only reads; changes nothing.

Parameters:

- `embedMedia`: boolean
- `path`: string, required

#### `gnubase.project.set`

Changes the project's name, sample rate, stereo pan law or maximum volume. Linear positions keep their time at a new sample rate. panLaw is how much a mono channel panned to the center is turned down on each side: 0dB, -3dB (the default), -4.5dB, -6dB or equalPower (-3.01 dB, the power stays the same wherever it is panned); the side panned toward is always at 0 dB. maxVolume is the top of the faders, 6 (the default) or 12 dB; faders above a lower maximum come down to it.

Parameters:

- `maxVolume`: number
- `name`: string
- `panLaw`: one of "0dB" | "-3dB" | "-4.5dB" | "-6dB" | "equalPower"
- `sampleRate`: number

### quantize

#### `gnubase.quantize.apply`

Quantizes note starts: moves them toward the nearest position of the grid, or of the groove, and keeps their lengths. Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's (see quantize.set), or preset's, changed by settings. Quantizing starts from the notes' original timing unless settings.soft is true, and keeps it, so quantize.reset puts it back. Notes moved by other means since they were quantized start from where they are; velocities and lengths changed by other means stay. Returns the notes that changed. Grids are in project time and start again at every bar; notes don't move before their part's start or to or past its end.

Parameters:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer
- `preset`: string
- `settings`: object

Returns:

- `notes`: array, required

#### `gnubase.quantize.ends`

Quantizes note ends: moves them toward the nearest grid position and keeps the starts. Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's (see quantize.set), or preset's, changed by settings. Quantizing starts from the notes' original timing unless settings.soft is true, and keeps it, so quantize.reset puts it back. Notes moved by other means since they were quantized start from where they are; velocities and lengths changed by other means stay. Returns the notes that changed.

Parameters:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer
- `preset`: string
- `settings`: object

Returns:

- `notes`: array, required

#### `gnubase.quantize.extractGroove`

Makes a groove preset from a MIDI part's rhythm: a groove position at every note start, with the highest velocity and the longest length of the notes there. The groove is length ticks long, by default the part's length, and the preset is named name, by default after the part. The groove's origin is the part's start, so it repeats in phase with the part.

Parameters:

- `length`: integer
- `name`: string
- `part`: integer, required

Returns:

- `preset`: object, required

#### `gnubase.quantize.freeze`

Makes quantized notes' timing permanent: quantizing starts from it from now on, and quantize.reset no longer goes back. Takes the notes of part (only notes, if given) or of every part in parts and returns the notes that changed.

Parameters:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer

Returns:

- `notes`: array, required

#### `gnubase.quantize.get`

The project's quantize settings and whether auto quantize is on (see quantize.set for the settings).

Only reads; changes nothing.

Returns:

- `autoQuantize`: boolean, required
- `settings`: object, required

#### `gnubase.quantize.lengths`

Sets note lengths and keeps the starts: to length, a note value ("1/8", "1/8T", "1/8.") or ticks, or without length to the step of the grid at each note's start, so tuplets and swing count and notes outside the catch range keep their length. Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's (see quantize.set), or preset's, changed by settings. quantize.reset puts the lengths back. Returns the notes that changed.

Parameters:

- `length`: string or integer
- `notes`: array of integer
- `part`: integer
- `parts`: array of integer
- `preset`: string
- `settings`: object

Returns:

- `notes`: array, required

#### `gnubase.quantize.presets`

The quantize presets: the factory presets (straight grids "1/1" to "1/128", triplets such as "1/8 Triplet" and dotted grids such as "1/4 Dotted") and the user's, each with its settings. The user's presets are kept in the settings folder, so they come back when GNUbase starts again: file is where (null if they are kept in memory only), readOnly whether changes are saved (not while the file belongs to a newer GNUbase), and problems what was wrong with it, such as presets that this GNUbase can't read and skips; they stay in the file.

Only reads; changes nothing.

Returns:

- `file`: string or null, required
- `presets`: array, required
- `problems`: array, required
- `readOnly`: boolean, required

#### `gnubase.quantize.removePreset`

Removes one of the user's quantize presets, and the presets of that name that this GNUbase can't read (see quantize.presets).

Parameters:

- `name`: string, required

#### `gnubase.quantize.renamePreset`

Renames one of the user's quantize presets.

Parameters:

- `name`: string, required
- `newName`: string, required

#### `gnubase.quantize.reset`

Puts quantized notes back to their timing before quantizing: starts, lengths and velocities. Notes moved by other means since keep their timing, and velocities and lengths changed by other means stay. Takes the notes of part (only notes, if given) or of every part in parts and returns the notes that changed.

Parameters:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer

Returns:

- `notes`: array, required

#### `gnubase.quantize.savePreset`

Saves the project's quantize settings, changed by settings, as a preset, replacing the user's preset of that name. Factory presets can't be replaced. Presets belong to the user, not to the project: they are saved in the settings folder.

Parameters:

- `name`: string, required
- `settings`: object

Returns:

- `preset`: object, required

#### `gnubase.quantize.set`

Changes the project's quantize settings: to preset's, if given, then changed by settings. Settings: grid, a note value from "1/1" to "1/128", "T" for a triplet and "." for dotted; tuplet, 0 (off) or 3 to 15 notes in the time of the largest power of two below it; swing 0-1, moving every second grid position later, at 1 by a third of a step (straight grids without tuplets only); catchRange, ticks from a grid position within which notes are quantized (0: all); safeRange, ticks within which they stay; randomize, ticks by which quantized notes move at random, by seed; soft, starting from the current positions instead of the original ones; strength 0-1, the part of the way notes move. groove {length, points: [{position, velocity 0-1, length}]} quantizes to a groove instead of the grid, with preQuantize (a note value), maxMove (ticks), velocityStrength, lengthStrength and origin (where it starts repeating, in project ticks). autoQuantize is whether recording quantizes. Settings are not undo steps; they are saved with the project.

Parameters:

- `autoQuantize`: boolean
- `preset`: string
- `settings`: object

Returns:

- `autoQuantize`: boolean, required
- `settings`: object, required

### settings

#### `gnubase.settings.get`

The value of a user setting; settings.list describes them all.

Only reads; changes nothing.

Parameters:

- `key`: one of "audio.backend" | "audio.blockSize" | "audio.busPresets" | "audio.device" | "audio.sampleRate" | "ui.scale" | "ui.theme", required. The setting.

Returns:

- `key`: string, required
- `value`: any, required. The setting's value.

#### `gnubase.settings.list`

Every user setting with its key, description, JSON schema, default and value; the settings file, null when settings are kept in memory only; its version; and what happened when it was loaded: the status (inMemory, missing, loaded, migrated, reset or newer), the version the file had, the problems found, where the original file was kept, and whether the file is read-only for this session, so that changes apply but aren't saved (a newer GNUbase's file, or one that couldn't be backed up or locked).

Only reads; changes nothing.

Returns:

- `file`: string or null, required
- `load`: object, required
- `settings`: array, required
- `version`: integer, required

#### `gnubase.settings.reset`

Puts a user setting back to its default, or every one without key, and saves the settings file. Returns the settings that were reset with their values; emits settings.changed with those that changed.

Parameters:

- `key`: one of "audio.backend" | "audio.blockSize" | "audio.busPresets" | "audio.device" | "audio.sampleRate" | "ui.scale" | "ui.theme". The setting.

Returns:

- `values`: object, required

#### `gnubase.settings.set`

Changes a user setting and saves the settings file, unless settings.list reports it read-only. User settings belong to the user, not to a project. Emits settings.changed with the changed value, unless it had that value already. The settings: `audio.backend` (string, default ""): The audio backend, such as wasapi or coreaudio, or empty for the preferred one; `audio.blockSize` (integer, 16 to 8192, default 256): Frames per audio block; `audio.busPresets` (object, default {"input":{},"output":{}}): The user's own bus presets, per direction (input, output) by name: each a list of busses with their name, format, ports and child busses (bus.savePreset); `audio.device` (string, default ""): The audio device's id, or empty for the backend's default device; `audio.sampleRate` (number, 8000 to 768000, default 48000.0): The audio device's sample rate in Hz; `ui.scale` (number, 0.5 to 4, default 1.0): The UI scale, applied on top of each monitor's own scale; `ui.theme` (string, default "gnubase"): The theme's folder name in resources/themes.

Parameters:

- `key`: one of "audio.backend" | "audio.blockSize" | "audio.busPresets" | "audio.device" | "audio.sampleRate" | "ui.scale" | "ui.theme", required. The setting.
- `value`: any, required. The new value; it must fit the setting's schema.

Returns:

- `key`: string, required
- `value`: any, required. The setting's value.

### track

#### `gnubase.track.add`

Adds a track and its channel. Tracks go into a folder with parent, at index among its tracks (default: last). Audio channels default to stereo; format or channels set another format, as in bus.add. Folders get a channel only with groupChannel. New channels play through the main output, or through the folder's group channel. Marker tracks have no channel and stay at the top level; a project has at most 10, and a new one becomes the active marker track.

Parameters:

- `channels`: integer
- `color`: string or null
- `format`: one of "mono" | "stereo" | "LRC" | "LRS" | "LRC+LFE" | "LRS+LFE" | "LRCS" | "LRCS+LFE" | "quadro" | "quadro+LFE" | "5.0" | "5.1" | "6.0 cine" | "6.0 music" | "7.0" | "7.1"
- `groupChannel`: boolean
- `index`: integer
- `name`: string
- `parent`: integer
- `timeBase`: one of "musical" | "linear"
- `type`: one of "audio" | "instrument" | "midi" | "folder" | "group" | "effect" | "marker", required

Returns:

- `channel`: object or null, required
- `track`: object, required

#### `gnubase.track.move`

Moves a track, with what it contains, into a folder (parent, 0 for the top level).

Parameters:

- `index`: integer, required
- `parent`: integer, required
- `track`: integer, required

#### `gnubase.track.remove`

Removes a track with its channel, events, parts or markers; a folder also removes the tracks inside it. Without the active marker track, the topmost marker track becomes the active one.

Parameters:

- `track`: integer, required

#### `gnubase.track.set`

Changes a track's name, color (#rrggbb, or null for the default) or time base. A new time base keeps its events, parts and markers at the same time.

Parameters:

- `color`: string or null
- `name`: string
- `timeBase`: one of "musical" | "linear"
- `track`: integer, required

Returns:

- `track`: object, required

### transport

#### `gnubase.transport.exchangeLocators`

Swaps the left and right locators.

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `gnubase.transport.get`

Whether playback runs, the project cursor in samples, the locators and the modes. After a locate the cursor is where it goes, even before the audio thread has applied it.

Only reads; changes nothing.

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `gnubase.transport.locate`

Moves the project cursor to a position in samples, or to a target: projectStart, projectEnd (the end of the last event), leftLocator or rightLocator. While playing, playback continues from there.

Parameters:

- `position`: integer
- `target`: string

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `gnubase.transport.play`

Starts playback from the project cursor, or from the left locator if startMode is cycleStart. Does nothing while playing.

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `gnubase.transport.set`

Changes the given settings and keeps the rest. left and right: the locators, in samples, or "cursor" for the cursor position. cycle: cycle playback between the locators; with the right locator before the left one, playback skips the range instead. startMode: cursor or cycleStart (the left locator). returnToStart: on stop, return to where playback started.

Parameters:

- `cycle`: boolean
- `left`: integer or string
- `returnToStart`: boolean
- `right`: integer or string
- `startMode`: string

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `gnubase.transport.stop`

Stops playback. With returnToStart the cursor goes back to where playback started or was last located to.

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `gnubase.transport.toggle`

Stops playback if it plays, otherwise starts it.

Returns:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required
