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

## Commands

### audio

#### `gnubase.audio.capture`

Captured output samples of one device output channel, from frame start, at most 1048576 frames.

Only reads; changes nothing.

Parameters:

- `channel`: integer, required
- `frames`: integer, required
- `start`: integer, required

Returns:

- `samples`: array, required

#### `gnubase.audio.get`

The audio device, whether it runs, and how many frames it has processed and captured.

Only reads; changes nothing.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `gnubase.audio.render`

Offline mode: processes the given number of frames as fast as possible, then returns. The device must be stopped.

Parameters:

- `frames`: integer, required

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `gnubase.audio.setDevice`

Replaces the audio device with a dummy device: no hardware, its output captured in memory (audio.capture). Defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 outputs, 60 s of capture. The input is silent, or a built-in generator (see processor.list) with parameters by name. Starts stopped at position 0.

Parameters:

- `blockSize`: integer
- `captureSeconds`: number
- `input`: object
- `inputChannels`: integer
- `outputChannels`: integer
- `sampleRate`: number

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `gnubase.audio.start`

Simulated real time: the device processes one block per block duration until stopped.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `gnubase.audio.stop`

Stops simulated real time. Does nothing if the device isn't running.

Returns:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `position`: integer, required
- `running`: boolean, required

### audioEvent

#### `gnubase.audioEvent.add`

Adds an audio event that plays a section of a clip on an audio track. position is in the track's time base (ticks or samples); offset and length are in the clip's sample frames, length defaulting to the rest of the clip.

Parameters:

- `clip`: integer, required
- `length`: integer
- `offset`: integer
- `position`: integer, required
- `track`: integer, required

Returns:

- `event`: object, required

#### `gnubase.audioEvent.remove`

Removes an audio event.

Parameters:

- `event`: integer, required

#### `gnubase.audioEvent.set`

Changes an audio event's position, clip, offset or length.

Parameters:

- `clip`: integer
- `event`: integer, required
- `length`: integer
- `offset`: integer
- `position`: integer

Returns:

- `event`: object, required

### bus

#### `gnubase.bus.add`

Adds an input or output bus with 1 to 16 audio channels.

Parameters:

- `channels`: integer
- `direction`: one of "input" | "output", required
- `name`: string, required

Returns:

- `bus`: object, required

#### `gnubase.bus.remove`

Removes a bus; channels connected to it are disconnected.

Parameters:

- `bus`: integer, required

#### `gnubase.bus.set`

Renames a bus.

Parameters:

- `bus`: integer, required
- `name`: string

Returns:

- `bus`: object, required

### channel

#### `gnubase.channel.set`

Changes a channel's audio channels, input (an input bus) or output (an output bus or a group channel; for MIDI tracks an instrument channel). 0 disconnects.

Parameters:

- `channel`: integer, required
- `channels`: integer
- `input`: integer
- `output`: integer

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

### controller

#### `gnubase.controller.add`

Adds controller events to a MIDI part: position from the part's start in ticks, controller number 0-127, value 0-1, MIDI channel 0-15 (default 0).

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

### engine

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

### media

#### `gnubase.media.add`

Adds an audio file. Its sample rate, channel count and length in sample frames are read from the file, or taken as given, for a file that is still being recorded.

Parameters:

- `channels`: integer
- `length`: integer
- `path`: string, required
- `sampleRate`: number

Returns:

- `media`: object, required

#### `gnubase.media.convert`

Converts a media file to another sample rate (by default the project's), writes the result to path as a 32-bit float WAV file and adds it as a new media file.

Parameters:

- `media`: integer, required
- `path`: string, required
- `sampleRate`: number

Returns:

- `media`: object, required

#### `gnubase.media.remove`

Removes an audio file that no clip uses.

Parameters:

- `media`: integer, required

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

Adds notes to a MIDI part: position from the part's start and length in ticks, pitch 0-127, velocity 0-1, MIDI channel 0-15 (default 0).

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

### processor

#### `gnubase.processor.list`

Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units.

Only reads; changes nothing.

Returns:

- `processors`: array, required

### project

#### `gnubase.project.get`

The whole project: busses, tracks in order, channels, media, clips, events and parts.

Only reads; changes nothing.

#### `gnubase.project.load`

Replaces the project with a DAWproject file and clears the undo history. Media files embedded in it are extracted to mediaFolder, by default a folder named after the file next to it.

Parameters:

- `mediaFolder`: string
- `path`: string, required

#### `gnubase.project.new`

Replaces the project with an empty one that has a stereo input and output bus, and clears the undo history.

#### `gnubase.project.save`

Saves the project as a DAWproject file. Media files are copied into it unless embedMedia is false; then the file refers to them where they are.

Only reads; changes nothing.

Parameters:

- `embedMedia`: boolean
- `path`: string, required

#### `gnubase.project.set`

Changes the project's name or sample rate. Linear positions keep their time at a new sample rate.

Parameters:

- `name`: string
- `sampleRate`: number

### track

#### `gnubase.track.add`

Adds a track and its channel. Tracks go into a folder with parent, at index among its tracks (default: last). Audio channels default to stereo; folders get a channel only with groupChannel. New channels play through the main output, or through the folder's group channel.

Parameters:

- `channels`: integer
- `color`: string or null
- `groupChannel`: boolean
- `index`: integer
- `name`: string
- `parent`: integer
- `timeBase`: one of "musical" | "linear"
- `type`: one of "audio" | "instrument" | "midi" | "folder" | "group" | "effect", required

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

Removes a track with its channel, events and parts; a folder also removes the tracks inside it.

Parameters:

- `track`: integer, required

#### `gnubase.track.set`

Changes a track's name, color (#rrggbb, or null for the default) or time base. A new time base keeps its events and parts at the same time.

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

Whether playback runs, the project cursor in samples, the locators and the modes.

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
