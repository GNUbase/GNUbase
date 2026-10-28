# MCP server

GNUbase has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server, so AI agents such as Claude Code can use everything the engine can do: create projects, add tracks and notes, play, render and measure audio, and run Lua.

- **Tools come from the engine's command registry.** Every engine command is a tool, named with `_` instead of `.` (`track.add` is `track_add`), with the command's parameter schema as its input schema and its result schema as its output schema. A command that only reads is marked read-only (`readOnlyHint`). The [tool catalogue](#tool-catalogue) at the end lists them all; the registry itself is `engine_commands`.
- **`script_run`** runs Lua code with the same API ([lua.md](lua.md)) in a sandbox, for work that takes many steps or for test cases written with `gnubase.test`.
- **Resources:** queries without required parameters can also be read as resources, `gnubase://<command>`, e.g. `gnubase://project.get`.
- **Errors:** a command that fails is a tool result with `isError: true` and the reason, such as `parameters.type must be one of [...]`, so the agent can correct itself. Malformed messages get JSON-RPC errors.
- The server speaks MCP revision 2025-06-18; HTTP clients may also send `MCP-Protocol-Version: 2025-03-26`.

How it fits into GNUbase is described in [architecture.md](architecture.md#scripting-mcp-and-tests).

## Starting the server

GNUbase serves MCP in headless mode, without UI. The engine plays through the dummy audio device, which has no hardware and captures its output in memory, so agents can render audio offline and measure it.

**stdio** (the default): the MCP client starts GNUbase and talks to it over stdin and stdout, one JSON-RPC message per line. GNUbase stops when stdin ends.

```bash
gnubase --headless
gnubase --headless --project song.dawproject --sample-rate 44100 --block-size 128
```

Stdout belongs to MCP. Messages for people go to stderr and to the log file `gnubase-headless.log` in the per-user log folder.

**HTTP**: GNUbase runs on its own and clients connect to it on this machine. It prints the URL and a new access token, and serves until Ctrl+C or SIGTERM.

```bash
gnubase --headless --mcp-http          # a free port
gnubase --headless --mcp-http=8765     # a fixed port
```

```text
MCP server: http://127.0.0.1:8765/mcp
Access token: 3f1c...e9a0
```

Clients send each message with `POST /mcp` and the header `Authorization: Bearer <token>`. Answers are JSON; notifications get `202 Accepted`. There is no stream of server-initiated messages yet, so `GET /mcp` answers `405`.

`gnubase --help` lists every option. Exit codes: 0 done, 1 unexpected error, 2 invalid command line, 3 the audio device or the project could not be set up.

## Connecting clients

### Claude Code

stdio, with GNUbase started by Claude Code; everything after `--` is GNUbase's command line:

```bash
claude mcp add gnubase -- /path/to/gnubase --headless
claude mcp add gnubase -- /path/to/gnubase --headless --project /path/to/song.dawproject
```

HTTP, to a GNUbase that already runs with `--mcp-http`; use the URL and the token it printed:

```bash
claude mcp add --transport http gnubase http://127.0.0.1:8765/mcp --header "Authorization: Bearer <token>"
```

The token changes every time GNUbase starts, so stdio is simpler for everyday use. `claude mcp list` shows whether Claude Code can reach the server, and `/mcp` inside Claude Code shows its status and tools. `--scope project` stores the server in the project's `.mcp.json`, to share it with a team; `--scope user` makes it available in all your projects.

### Other clients

Most MCP clients take a JSON configuration with an `mcpServers` entry. For stdio:

```json
{
  "mcpServers": {
    "gnubase": {
      "command": "/path/to/gnubase",
      "args": ["--headless"]
    }
  }
}
```

For HTTP:

```json
{
  "mcpServers": {
    "gnubase": {
      "type": "http",
      "url": "http://127.0.0.1:8765/mcp",
      "headers": {"Authorization": "Bearer <token>"}
    }
  }
}
```

A client that only speaks stdio can still use GNUbase: that is the default. A client that only speaks HTTP needs `--mcp-http`.

## Example session

A session over stdio, as the client sends it and GNUbase answers (long results shortened with `…`). The client starts with `initialize`:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"example","version":"1"}}}
{"id":1,"jsonrpc":"2.0","result":{"capabilities":{"resources":{"listChanged":false,"subscribe":false},"tools":{"listChanged":false}},"instructions":"GNUbase is a digital audio workstation. Every tool runs one engine command; tools marked read-only only read. engine_commands lists every command with its documentation.","protocolVersion":"2025-06-18","serverInfo":{"name":"gnubase","title":"GNUbase","version":"0.1.0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

A tool that changes the project, `track_add`. The result comes as text and as `structuredContent`:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"track_add","arguments":{"type":"audio"}}}
{"id":2,"jsonrpc":"2.0","result":{"content":[{"text":"…","type":"text"}],"isError":false,"structuredContent":{"channel":{"channels":2,"id":4,"input":1,"output":2,"track":3},"track":{"channel":4,"color":null,"id":3,"name":"Audio 01","parent":0,"timeBase":"musical","type":"audio"}}}}
```

Play a 1 kHz sine on the device input through the new audio track, rendered offline:

```json
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"audio_setDevice","arguments":{"input":{"processor":"sine","parameters":{"Frequency":1000}}}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"audio_render","arguments":{"frames":48000}}}
{"id":4,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"capturedFrames":48000,"device":{"backend":"dummy",…},"droppedFrames":0,"playbackDropouts":0,"position":48000,"running":false}}}
```

Check the result with a Lua test case in `script_run`; a failing case would come back with `isError` and the file and line:

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"script_run","arguments":{"name":"level.lua","code":"local test = gnubase.test\ntest.case('the output plays the sine', function()\n\tlocal peak = 0\n\tfor _, s in ipairs(gnubase.audio.capture{channel = 0, start = 0, frames = 4800}.samples) do\n\t\tpeak = math.max(peak, math.abs(s))\n\tend\n\ttest.near(peak, 0.5, 0.01)\nend)"}}}
{"id":5,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"output":"1..1\nok 1 - the output plays the sine\n"}}}
```

Read a query as a resource:

```json
{"jsonrpc":"2.0","id":6,"method":"resources/read","params":{"uri":"gnubase://transport.get"}}
{"id":6,"jsonrpc":"2.0","result":{"contents":[{"mimeType":"application/json","text":"{\"cycle\":false,\"left\":0,\"playing\":false,\"position\":0,\"returnToStart\":false,\"right\":0,\"startMode\":\"cursor\"}","uri":"gnubase://transport.get"}]}}
```

A command that fails tells the agent why:

```json
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"track_add","arguments":{"type":"nonsense"}}}
{"id":7,"jsonrpc":"2.0","result":{"content":[{"text":"parameters.type must be one of [\"audio\",\"instrument\",\"midi\",\"folder\",\"group\",\"effect\"]","type":"text"}],"isError":true}}
```

Over HTTP, the same messages go one per request:

```bash
curl -X POST http://127.0.0.1:8765/mcp -H "Authorization: Bearer <token>" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"engine_version"}}'
```

## Security

The MCP server gives a client the same power over GNUbase as the person using it, and it is built so that only the intended client gets it:

- **stdio** talks only to the process that started GNUbase. Whoever can start a program as you can already do what GNUbase does.
- **HTTP listens on 127.0.0.1 only.** Requests from other addresses are refused (`403`), so the server can't be reached from the network.
- **Every HTTP request needs the access token** (`401` without it). The token is 32 random bytes from the operating system's random source, new on every start, and compared in constant time. Other users and programs on the machine can't use the server without it.
- **Web pages can't use it.** Browsers send an `Origin` header; requests from pages of other origins are refused (`403`), which stops DNS rebinding attacks, where a web page reaches a local server through a name that resolves to 127.0.0.1.
- **Tools act with your rights.** Some tools read and write files wherever you can, such as `project_load`, `project_save`, `media_add` and `media_convert`. Only connect agents you trust with your files, and review what they do, as with any tool that edits files.
- **`script_run` is sandboxed:** Lua without files, processes or bytecode (no `io`, `os`, `debug`, `package`, `require`, `dofile` or `loadfile`), and at most 60 seconds of Lua code per call. Scripts that come with a project never run without your consent.

## Troubleshooting

| What happens | Why, and what to do |
|---|---|
| The client can't start GNUbase, or `claude mcp list` shows it failed | The path is wrong or misses `--headless`. Run the same command in a terminal: it should wait for input. Exit code 2 means an invalid option (`gnubase --help`), 3 that the audio device or `--project` could not be set up; stderr says which. |
| The client gets text that isn't JSON over stdio | Something else wrote to stdout, such as a wrapper script. GNUbase itself only writes MCP messages there. |
| `401 Unauthorized` over HTTP | The token is missing or old: it changes every time GNUbase starts. Use the one it printed last, as `Authorization: Bearer <token>`. |
| `403 Forbidden` over HTTP | The request came from another machine or a web page. Connect from this machine, to `127.0.0.1`, without a browser. |
| `405 Method Not Allowed` | The client opened `GET /mcp` for server-initiated messages, which GNUbase doesn't send yet. Clients fall back to POST. |
| A tool result has `isError: true` | The command failed, and the text says why; nothing changed. `engine_commands` describes every command. |
| This catalogue misses a tool | It's generated from the server; the tests fail when it's out of date. Run them with `GNUBASE_UPDATE_DOCS=1` and commit `docs/mcp.md`. |

## Tool catalogue

Generated from what the MCP server lists, with `script_run`: every tool by area, its engine command, whether it only reads, its arguments and its result. The tests fail when it is out of date; running them with `GNUBASE_UPDATE_DOCS=1` writes it again.

<!-- Generated from the MCP server's tools/list: run the tests with GNUBASE_UPDATE_DOCS=1 to update it. -->

### audioEvent

#### `audioEvent_add`

Engine command `audioEvent.add`.

Adds an audio event that plays a section of a clip on an audio track. position is in the track's time base (ticks or samples); offset and length are in the clip's sample frames, length defaulting to the rest of the clip. volume is in dB, from -144 to 6 (default 0); a muted event doesn't play. A track plays one event at a time: where events overlap, the one added last plays.

Arguments:

- `clip`: integer, required
- `length`: integer
- `mute`: boolean
- `offset`: integer
- `position`: integer, required
- `track`: integer, required
- `volume`: number

Result:

- `event`: object, required

#### `audioEvent_remove`

Engine command `audioEvent.remove`.

Removes an audio event.

Arguments:

- `event`: integer, required

#### `audioEvent_set`

Engine command `audioEvent.set`.

Changes an audio event's position, clip, offset, length, volume (dB) or mute.

Arguments:

- `clip`: integer
- `event`: integer, required
- `length`: integer
- `mute`: boolean
- `offset`: integer
- `position`: integer
- `volume`: number

Result:

- `event`: object, required

### audio

#### `audio_capture`

Engine command `audio.capture`. Read-only.

Captured output samples of one output channel of the dummy device, from frame start, at most 1048576 frames.

Arguments:

- `channel`: integer, required
- `frames`: integer, required
- `start`: integer, required

Result:

- `samples`: array, required

#### `audio_controlPanel`

Engine command `audio.controlPanel`.

Opens the control panel of the audio device's driver. Changes made there may reset the device.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `audio_devices`

Engine command `audio.devices`. Read-only.

The audio backends of this build, the preferred one first, and the devices each has now: their ids, names, channel names, sample rates, block sizes, clock sources (and the current one), and whether the driver has a control panel. A backend that can't list its devices has an error instead.

Result:

- `backends`: array, required

#### `audio_get`

Engine command `audio.get`. Read-only.

The audio device (its backend, id, name, format, channel names, latencies in frames, clock source and whether it has a control panel), whether it runs, how many frames it has processed and, if it is the dummy device, captured, and how many frames of audio track playback were silent because a disk stream was late.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `audio_render`

Engine command `audio.render`.

Offline mode: processes the given number of frames as fast as possible, then returns. Only the dummy device, and only while it is stopped.

Arguments:

- `frames`: integer, required

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `audio_reset`

Engine command `audio.reset`.

Opens the audio device again with the same settings and restarts it if it ran. Drivers ask for this when their settings change, e.g. in their control panel; the engine then resets on its own.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `audio_setDevice`

Engine command `audio.setDevice`.

Replaces the audio device. backend: one from audio.devices, by default "dummy". device: its id, by default the backend's default device. sampleRate and blockSize: by default the driver's current rate and preferred size. clockSource: a name from the device's clockSources. The device starts stopped at position 0. Another backend's device is closed before the new one opens, since drivers often allow only one; if opening fails, there is no device.
The dummy device has no hardware: its output is captured in memory (audio.capture) and it renders offline (audio.render). Its defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 outputs (inputChannels, outputChannels), 60 s of capture (captureSeconds). Its input is silent, or a built-in generator (see processor.list) with parameters by name.

Arguments:

- `backend`: string
- `blockSize`: integer
- `captureSeconds`: number
- `clockSource`: string
- `device`: string
- `input`: object
- `inputChannels`: integer
- `outputChannels`: integer
- `sampleRate`: number

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `audio_start`

Engine command `audio.start`.

Simulated real time: the device processes one block per block duration until stopped.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

#### `audio_stop`

Engine command `audio.stop`.

Stops simulated real time. Does nothing if the device isn't running.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `running`: boolean, required

### bus

#### `bus_add`

Engine command `bus.add`.

Adds an input or output bus with 1 to 16 audio channels.

Arguments:

- `channels`: integer
- `direction`: one of "input" | "output", required
- `name`: string, required

Result:

- `bus`: object, required

#### `bus_remove`

Engine command `bus.remove`.

Removes a bus; channels connected to it are disconnected.

Arguments:

- `bus`: integer, required

#### `bus_set`

Engine command `bus.set`.

Renames a bus.

Arguments:

- `bus`: integer, required
- `name`: string

Result:

- `bus`: object, required

### channel

#### `channel_set`

Engine command `channel.set`.

Changes a channel's audio channels, input (an input bus) or output (an output bus or a group channel; for MIDI tracks an instrument channel). 0 disconnects.

Arguments:

- `channel`: integer, required
- `channels`: integer
- `input`: integer
- `output`: integer

Result:

- `channel`: object, required

### clip

#### `clip_add`

Engine command `clip.add`.

Adds a clip that plays an audio file; it is named after the file unless a name is given.

Arguments:

- `media`: integer, required
- `name`: string

Result:

- `clip`: object, required

#### `clip_remove`

Engine command `clip.remove`.

Removes a clip that no audio event uses.

Arguments:

- `clip`: integer, required

### controller

#### `controller_add`

Engine command `controller.add`.

Adds controller events to a MIDI part: position from the part's start in ticks, controller number 0-127, value 0-1, MIDI channel 0-15 (default 0).

Arguments:

- `controllers`: array of object, required
- `part`: integer, required

Result:

- `controllers`: array, required

#### `controller_remove`

Engine command `controller.remove`.

Removes controller events from a MIDI part.

Arguments:

- `controllers`: array of integer, required
- `part`: integer, required

### engine

#### `engine_commands`

Engine command `engine.commands`. Read-only.

Every command with its documentation, parameters and result.

Result:

- `commands`: array, required

#### `engine_version`

Engine command `engine.version`. Read-only.

The GNUbase version.

Result:

- `version`: string, required

### graph

#### `graph_get`

Engine command `graph.get`. Read-only.

The project's audio processing graph: its nodes (busses, then channels in track order) with the nodes they receive audio and events from, and the order they are processed in.

### history

#### `history_describe`

Engine command `history.describe`.

Sets the details of a history step, a note for the user.

Arguments:

- `details`: string, required
- `step`: integer, required

#### `history_get`

Engine command `history.get`. Read-only.

The undo history: every step with its action, time (UTC) and details, the position (steps before it are done, the ones after it can be redone) and the maximum number of steps (0: no limit).

#### `history_redo`

Engine command `history.redo`.

Redoes the next undone step, or the given number of steps.

Arguments:

- `steps`: integer

Result:

- `position`: integer, required

#### `history_set`

Engine command `history.set`.

Sets the maximum number of undo steps (0: no limit). The oldest steps beyond it are dropped. A setting, so it stays when a new project starts.

Arguments:

- `maxSteps`: integer, required

#### `history_transaction`

Engine command `history.transaction`.

Runs document commands in order as one undo step named description. If one fails, none of them changes the project. Returns each command's result.

Arguments:

- `commands`: array of object, required
- `description`: string, required

Result:

- `results`: array, required

#### `history_undo`

Engine command `history.undo`.

Undoes the last step, or the given number of steps.

Arguments:

- `steps`: integer

Result:

- `position`: integer, required

### media

#### `media_add`

Engine command `media.add`.

Adds an audio file. Its sample rate, channel count and length in sample frames are read from the file, or taken as given, for a file that is still being recorded.

Arguments:

- `channels`: integer
- `length`: integer
- `path`: string, required
- `sampleRate`: number

Result:

- `media`: object, required

#### `media_convert`

Engine command `media.convert`.

Converts a media file to another sample rate (by default the project's), writes the result to path as a 32-bit float WAV file and adds it as a new media file.

Arguments:

- `media`: integer, required
- `path`: string, required
- `sampleRate`: number

Result:

- `media`: object, required

#### `media_remove`

Engine command `media.remove`.

Removes an audio file that no clip uses.

Arguments:

- `media`: integer, required

### midiPart

#### `midiPart_add`

Engine command `midiPart.add`.

Adds a MIDI part to a MIDI or instrument track. position and length are in the track's time base.

Arguments:

- `length`: integer, required
- `name`: string
- `position`: integer, required
- `track`: integer, required

Result:

- `part`: object, required

#### `midiPart_remove`

Engine command `midiPart.remove`.

Removes a MIDI part with its notes and controllers.

Arguments:

- `part`: integer, required

#### `midiPart_set`

Engine command `midiPart.set`.

Changes a MIDI part's name, position or length.

Arguments:

- `length`: integer
- `name`: string
- `part`: integer, required
- `position`: integer

Result:

- `part`: object, required

### note

#### `note_add`

Engine command `note.add`.

Adds notes to a MIDI part: position from the part's start and length in ticks, pitch 0-127, velocity 0-1, MIDI channel 0-15 (default 0).

Arguments:

- `notes`: array of object, required
- `part`: integer, required

Result:

- `notes`: array, required

#### `note_remove`

Engine command `note.remove`.

Removes notes from a MIDI part.

Arguments:

- `notes`: array of integer, required
- `part`: integer, required

### processor

#### `processor_list`

Engine command `processor.list`. Read-only.

Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units.

Result:

- `processors`: array, required

### project

#### `project_get`

Engine command `project.get`. Read-only.

The whole project: busses, tracks in order, channels, media, clips, events and parts.

#### `project_load`

Engine command `project.load`.

Replaces the project with a DAWproject file and clears the undo history. Media files embedded in it are extracted to mediaFolder, by default a folder named after the file next to it.

Arguments:

- `mediaFolder`: string
- `path`: string, required

#### `project_new`

Engine command `project.new`.

Replaces the project with an empty one that has a stereo input and output bus, and clears the undo history.

#### `project_save`

Engine command `project.save`. Read-only.

Saves the project as a DAWproject file. Media files are copied into it unless embedMedia is false; then the file refers to them where they are.

Arguments:

- `embedMedia`: boolean
- `path`: string, required

#### `project_set`

Engine command `project.set`.

Changes the project's name or sample rate. Linear positions keep their time at a new sample rate.

Arguments:

- `name`: string
- `sampleRate`: number

### script

#### `script_run`

Runs Lua code with GNUbase's API: every engine command as gnubase.<area>.<name>(parameters), e.g. gnubase.track.add{type = "audio"}, and events through gnubase.on, gnubase.waitFor and gnubase.events (see docs/lua.md). Test cases registered with gnubase.test.case run when the code ends. Runs in a sandbox without files, processes or bytecode, for at most 60 s of Lua code. Returns what the script printed; an error comes back with its traceback and what was printed until then.

Arguments:

- `code`: string, required
- `name`: string

Result:

- `output`: string, required

### track

#### `track_add`

Engine command `track.add`.

Adds a track and its channel. Tracks go into a folder with parent, at index among its tracks (default: last). Audio channels default to stereo; folders get a channel only with groupChannel. New channels play through the main output, or through the folder's group channel.

Arguments:

- `channels`: integer
- `color`: string or null
- `groupChannel`: boolean
- `index`: integer
- `name`: string
- `parent`: integer
- `timeBase`: one of "musical" | "linear"
- `type`: one of "audio" | "instrument" | "midi" | "folder" | "group" | "effect", required

Result:

- `channel`: object or null, required
- `track`: object, required

#### `track_move`

Engine command `track.move`.

Moves a track, with what it contains, into a folder (parent, 0 for the top level).

Arguments:

- `index`: integer, required
- `parent`: integer, required
- `track`: integer, required

#### `track_remove`

Engine command `track.remove`.

Removes a track with its channel, events and parts; a folder also removes the tracks inside it.

Arguments:

- `track`: integer, required

#### `track_set`

Engine command `track.set`.

Changes a track's name, color (#rrggbb, or null for the default) or time base. A new time base keeps its events and parts at the same time.

Arguments:

- `color`: string or null
- `name`: string
- `timeBase`: one of "musical" | "linear"
- `track`: integer, required

Result:

- `track`: object, required

### transport

#### `transport_exchangeLocators`

Engine command `transport.exchangeLocators`.

Swaps the left and right locators.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_get`

Engine command `transport.get`. Read-only.

Whether playback runs, the project cursor in samples, the locators and the modes.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_locate`

Engine command `transport.locate`.

Moves the project cursor to a position in samples, or to a target: projectStart, projectEnd (the end of the last event), leftLocator or rightLocator. While playing, playback continues from there.

Arguments:

- `position`: integer
- `target`: string

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_play`

Engine command `transport.play`.

Starts playback from the project cursor, or from the left locator if startMode is cycleStart. Does nothing while playing.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_set`

Engine command `transport.set`.

Changes the given settings and keeps the rest. left and right: the locators, in samples, or "cursor" for the cursor position. cycle: cycle playback between the locators; with the right locator before the left one, playback skips the range instead. startMode: cursor or cycleStart (the left locator). returnToStart: on stop, return to where playback started.

Arguments:

- `cycle`: boolean
- `left`: integer or string
- `returnToStart`: boolean
- `right`: integer or string
- `startMode`: string

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_stop`

Engine command `transport.stop`.

Stops playback. With returnToStart the cursor goes back to where playback started or was last located to.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_toggle`

Engine command `transport.toggle`.

Stops playback if it plays, otherwise starts it.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

### Resources

Queries without required arguments can also be read as resources:

- `gnubase://audio.devices`: The audio backends of this build, the preferred one first, and the devices each has now: their ids, names, channel names, sample rates, block sizes, clock sources (and the current one), and whether the driver has a control panel. A backend that can't list its devices has an error instead.
- `gnubase://audio.get`: The audio device (its backend, id, name, format, channel names, latencies in frames, clock source and whether it has a control panel), whether it runs, how many frames it has processed and, if it is the dummy device, captured, and how many frames of audio track playback were silent because a disk stream was late.
- `gnubase://engine.commands`: Every command with its documentation, parameters and result.
- `gnubase://engine.version`: The GNUbase version.
- `gnubase://graph.get`: The project's audio processing graph: its nodes (busses, then channels in track order) with the nodes they receive audio and events from, and the order they are processed in.
- `gnubase://history.get`: The undo history: every step with its action, time (UTC) and details, the position (steps before it are done, the ones after it can be redone) and the maximum number of steps (0: no limit).
- `gnubase://processor.list`: Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units.
- `gnubase://project.get`: The whole project: busses, tracks in order, channels, media, clips, events and parts.
- `gnubase://transport.get`: Whether playback runs, the project cursor in samples, the locators and the modes.

<!-- End of the generated tool catalogue -->
