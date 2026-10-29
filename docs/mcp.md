# MCP server

GNUbase has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server, so AI agents such as Claude Code can use everything the engine can do: create projects, add tracks and notes, play, render and measure audio, and run Lua.

- **Tools come from the engine's command registry.** Every engine command is a tool, named with `_` instead of `.` (`track.add` is `track_add`), with the command's parameter schema as its input schema and its result schema as its output schema. A command that only reads is marked read-only (`readOnlyHint`). The [tool catalogue](#tool-catalogue) at the end lists them all; the registry itself is `engine_commands`.
- **`script_run`** runs Lua code with the same API ([lua.md](lua.md)) in a sandbox, for work that takes many steps or for test cases written with `gnubase.test`.
- **Events:** `events_take` returns what changed in the engine since the last call, such as `track.added` or `history.changed`, whoever changed it: this client, other MCP clients, Lua scripts or the UI. See [Events](#events).
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

Clients send each message with `POST /mcp` and the header `Authorization: Bearer <token>`. Answers are JSON; notifications get `202 Accepted`. `initialize` starts a session and answers with its id in the header `Mcp-Session-Id`, which the client sends with every request after that; MCP clients do this on their own. `DELETE /mcp` ends the session, and so do 30 minutes without requests. Requests of a session that ended get `404`, and the client starts a new one with `initialize`. There is no stream of server-initiated messages yet, so `GET /mcp` answers `405`.

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

Measure what was rendered: levels in dBFS, the strongest frequency and octave bands, and the loudness in LUFS:

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"audio_analyze","arguments":{"channels":[0],"peaks":1}}}
{"id":5,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"channels":[{"channel":0,"peak":0.5,"peakDb":-6.020599913279624,"rms":0.35355338722797996,"rmsDb":-9.030899952595973,"spectrum":{"bands":[…],"peaks":[{"frequency":999.9819858148717,"level":-6.020600003146717}]}}],"frames":48000,"loudness":-9.024238371036198,"sampleRate":48000.0,"start":0}}}
```

Or check it with a Lua test case in `script_run`; a failing case would come back with `isError` and the file and line:

```json
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"script_run","arguments":{"name":"level.lua","code":"local test = gnubase.test\ntest.case('the output plays the sine', function()\n\tlocal peak = 0\n\tfor _, s in ipairs(gnubase.audio.capture{channel = 0, start = 0, frames = 4800}.samples) do\n\t\tpeak = math.max(peak, math.abs(s))\n\tend\n\ttest.near(peak, 0.5, 0.01)\nend)"}}}
{"id":6,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"output":"1..1\nok 1 - the output plays the sine\n"}}}
```

Read a query as a resource:

```json
{"jsonrpc":"2.0","id":7,"method":"resources/read","params":{"uri":"gnubase://transport.get"}}
{"id":7,"jsonrpc":"2.0","result":{"contents":[{"mimeType":"application/json","text":"{\"cycle\":false,\"left\":0,\"playing\":false,\"position\":0,\"returnToStart\":false,\"right\":0,\"startMode\":\"cursor\"}","uri":"gnubase://transport.get"}]}}
```

A command that fails tells the agent why:

```json
{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"track_add","arguments":{"type":"nonsense"}}}
{"id":8,"jsonrpc":"2.0","result":{"content":[{"text":"parameters.type must be one of [\"audio\",\"instrument\",\"midi\",\"folder\",\"group\",\"effect\"]","type":"text"}],"isError":true}}
```

Over HTTP, the same messages go one per request:

```bash
curl -X POST http://127.0.0.1:8765/mcp -H "Authorization: Bearer <token>" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"engine_version"}}'
```

Only `events_take` needs a session: send `initialize` first (`curl -i` shows the `Mcp-Session-Id` header of the answer) and then the header `Mcp-Session-Id: <id>` with every request.

## Events

Every session has a queue of the engine events since it started: what changed in the project, the transport, the audio device and the undo history, with the same names and data as in Lua ([lua.md](lua.md#events)). The events come from every client of the engine: this MCP client, other MCP clients, Lua scripts (those run with `script_run` too) and the UI. Over stdio, the whole input is one session; over HTTP, `initialize` starts a session for each client.

`events_take` takes the events that haven't been taken yet, oldest first. The events of a command are there as soon as its result is. Each event has its `name`, its `data` and `request`, the engine's id for the command that caused it, the same for all events of one command:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"track_add","arguments":{"type":"audio"}}}
{"id":2,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{…}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"events_take"}}
{"id":3,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"dropped":0,"events":[{"data":{"channel":{…},"track":{"channel":4,"color":null,"id":3,"name":"Audio 01","parent":0,"timeBase":"musical","type":"audio"}},"name":"track.added","request":2},{"data":{"position":1,"redo":null,"steps":1,"undo":"Add track"},"name":"history.changed","request":2}]}}}
```

- `names` takes only the events with these names, e.g. `{"names":["track.added","track.removed"]}`. The others stay queued for later calls.
- `seconds` waits up to that long, at most 60, for the first event, and returns as soon as one arrives, e.g. to wait for a change someone makes in the UI. Without it, `events_take` returns at once.
- A session keeps the newest 1000 events. `dropped` counts the older ones that were dropped since the last call. Events were missed then, and queries such as `project_get` or `transport_get` tell the current state.

GNUbase doesn't push events to clients yet (MCP notifications over stdio, the HTTP stream, resource subscriptions): many clients don't pass them on to the model, while a tool works with every client.

## Security

The MCP server gives a client the same power over GNUbase as the person using it, and it is built so that only the intended client gets it:

- **stdio** talks only to the process that started GNUbase. Whoever can start a program as you can already do what GNUbase does.
- **HTTP listens on 127.0.0.1 only.** Requests from other addresses are refused (`403`), so the server can't be reached from the network.
- **Every HTTP request needs the access token** (`401` without it). The token is 32 random bytes from the operating system's random source, new on every start, and compared in constant time. Other users and programs on the machine can't use the server without it.
- **Web pages can't use it.** Browsers send an `Origin` header; requests from pages of other origins are refused (`403`), which stops DNS rebinding attacks, where a web page reaches a local server through a name that resolves to 127.0.0.1.
- **Tools act with your rights.** Some tools read and write files wherever you can, such as `project_load`, `project_save`, `media_add`, `media_convert`, `audio_record` and `audio_compare`. Only connect agents you trust with your files, and review what they do, as with any tool that edits files.
- **`script_run` is sandboxed:** Lua without files, processes or bytecode (no `io`, `os`, `debug`, `package`, `require`, `dofile` or `loadfile`), and at most 60 seconds of Lua code per call. Scripts that come with a project never run without your consent.

## Troubleshooting

| What happens | Why, and what to do |
|---|---|
| The client can't start GNUbase, or `claude mcp list` shows it failed | The path is wrong or misses `--headless`. Run the same command in a terminal: it should wait for input. Exit code 2 means an invalid option (`gnubase --help`), 3 that the audio device or `--project` could not be set up; stderr says which. |
| The client gets text that isn't JSON over stdio | Something else wrote to stdout, such as a wrapper script. GNUbase itself only writes MCP messages there. |
| `401 Unauthorized` over HTTP | The token is missing or old: it changes every time GNUbase starts. Use the one it printed last, as `Authorization: Bearer <token>`. |
| `403 Forbidden` over HTTP | The request came from another machine or a web page. Connect from this machine, to `127.0.0.1`, without a browser. |
| `404 Not Found` over HTTP | The session ended: the client ended it, it had no requests for 30 minutes, or GNUbase started again. MCP clients start a new session with `initialize` on their own; the events of the old one are gone. |
| `events_take` says it needs a session | The request had no `Mcp-Session-Id` header. Send `initialize` first, then its session id with every request. |
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

#### `audio_analyze`

Engine command `audio.analyze`. Read-only.

Measures the captured output of channels (default: every output) from frame start for frames frames (default: to the end of the capture). Per channel: the peak and RMS level, and a spectrum summary with the strongest frequencies (at most peaks, default 5, up to 20) and the power of octave bands. For the channels together: the integrated loudness in LUFS (ITU-R BS.1770), null below 400 ms. Levels are linear and in dBFS (a full-scale sine's peak is 0 dBFS, its RMS -3 dBFS); dBFS is null for silence.

Arguments:

- `channels`: array of integer
- `frames`: integer
- `peaks`: integer
- `start`: integer

Result:

- `channels`: array of object, required
- `frames`: integer, required
- `loudness`: number or null, required
- `sampleRate`: number, required
- `start`: integer, required

#### `audio_capture`

Engine command `audio.capture`. Read-only.

Captured output samples of one output channel of the dummy device, from frame start, at most 1048576 frames.

Arguments:

- `channel`: integer, required
- `frames`: integer, required
- `start`: integer, required

Result:

- `samples`: array, required

#### `audio_compare`

Engine command `audio.compare`. Read-only.

Null test: subtracts an audio file, reference, from the captured output and measures what remains. The file's channels are compared with channels (default: every output) in order, from frame start, for frames frames (default: the file's length); its sample rate must be the device's. peak is the largest difference of all channels, 0 if they cancel completely.

Arguments:

- `channels`: array of integer
- `frames`: integer
- `reference`: string, required
- `start`: integer

Result:

- `channels`: array of object, required
- `frames`: integer, required
- `peak`: number, required
- `peakDb`: number or null, required

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

#### `audio_record`

Engine command `audio.record`. Read-only.

Writes the captured output of channels (default: every output) from frame start for frames frames (default: to the end of the capture) to a WAV file at path, with samples in format (default float32). Returns the file's absolute path.

Arguments:

- `channels`: array of integer
- `format`: one of "float32" | "int16" | "int24" | "int32"
- `frames`: integer
- `path`: string, required
- `start`: integer

Result:

- `channels`: array of integer, required
- `frames`: integer, required
- `path`: string, required
- `sampleRate`: number, required

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

Changes a channel's audio channels, input (an input bus) or output (an output bus or a group channel; for MIDI tracks an instrument channel). 0 disconnects. instrument is the built-in instrument an instrument track plays (see processor.list), or empty for none.

Arguments:

- `channel`: integer, required
- `channels`: integer
- `input`: integer
- `instrument`: string
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

Adds controller events to a MIDI part: position from the part's start in ticks, MIDI channel 0-15 (default 0). type (default controller) says what else an event has: controller: number 0-127 and value 0-1; pitchBend: value 0-1, 0.5 is no bend; channelPressure: value; polyPressure: number (the pitch) and value; program: number (the program) and optionally bank 0-16383; sysEx: data, the message's bytes from 0xF0 to 0xF7.

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

#### `controller_set`

Engine command `controller.set`.

Changes controller events of a MIDI part: each change names a controller event by id and gives what changes: position in ticks, type, and the fields of the type as controller.add takes them (number, value, channel, bank, data). A field the type doesn't have is an error, such as value on a program change; a null bank removes it. A new type keeps the fields it shares with the old one and needs the others. Returns the changed controller events.

Arguments:

- `controllers`: array of object, required
- `part`: integer, required

Result:

- `controllers`: array, required

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

### events

#### `events_take`

Read-only.

Takes the engine events of this session that haven't been taken yet, oldest first, each with name (e.g. track.added), data (what changed) and request (the engine request that caused it). Events come from every client: this one, other MCP clients, Lua scripts and the UI. names takes only events with these names and leaves the others queued. seconds (0 to 60, default 0) waits up to that long for the first event. A session keeps the newest 1000 events; dropped counts the older ones dropped since the last call. A session starts with initialize; over HTTP it is the one of the Mcp-Session-Id header.

Arguments:

- `names`: array of string
- `seconds`: number

Result:

- `dropped`: integer, required
- `events`: array of object, required

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

### job

#### `job_cancel`

Engine command `job.cancel`.

Asks a running job to stop. It ends soon after as cancelled, with a job.finished event, and cleans up what it made; a job that has ended stays as it is.

Arguments:

- `job`: integer, required

Result:

- `job`: object, required

#### `job_get`

Engine command `job.get`. Read-only.

A background job, as job.list shows it.

Arguments:

- `job`: integer, required

Result:

- `job`: object, required

#### `job_list`

Engine command `job.list`. Read-only.

Every background job since the engine started, such as backups: id, kind, state (running, done, failed or cancelled), progress from 0 to 1, what it is doing, and its result or error once it ended.

Result:

- `jobs`: array of object, required

### media

#### `media_add`

Engine command `media.add`.

Adds an audio file. Its sample rate, channel count and length in sample frames are read from the file, or taken as given, for a file that is still being recorded. With copy, a file from outside the project folder is also copied into its media folder (see project.location); the project must have been saved. The copy runs as a job, returned as job (null without one), with the result that media.prepareArchive's job has: the media file is added at once where it is, and when the copy is done it is pointed at the copy as an undo step of its own.

Arguments:

- `channels`: integer
- `copy`: boolean
- `length`: integer
- `path`: string, required
- `sampleRate`: number

Result:

- `job`: object or null, required
- `media`: object, required

#### `media_convert`

Engine command `media.convert`.

Converts a media file to another sample rate (by default the project's), writes the result to path as a 32-bit float WAV file and adds it as a new media file. path must not be one of the project's media files.

Arguments:

- `media`: integer, required
- `path`: string, required
- `sampleRate`: number

Result:

- `media`: object, required

#### `media_peakStatus`

Engine command `media.peakStatus`. Read-only.

The state of the waveform overviews of a media file, or of every media file: pending, generating (with progress from 0 to 1), ready, failed (with the error) or recording; the frames that have peaks; the peak file they were loaded from or written to; whether they were generated from the audio file rather than loaded. Peak files are kept next to their audio file as <file name>.gbpeaks, or in fallbackFolder where that can't be written.

Arguments:

- `media`: integer

Result:

- `fallbackFolder`: string, required
- `media`: array of object, required

#### `media_peaks`

Engine command `media.peaks`. Read-only.

The waveform overview of a media file for drawing: frames sample frames of the file from start (default: the rest of the file), in pixels columns (up to 65536). Per channel (default: all of them; channels picks some, numbered from 0), the smallest and largest sample of each column, from -32767 to 32767 (full scale). Pixels times channels can be up to 262144. The columns come from the coarsest level of the file's peaks (level: 256, 4096 or 65536 frames per peak) that has a peak per column, so they cost the same at any length. status says whether the peaks are ready; while they are generated, only the first available frames have columns and the rest are 0. A file that changed since its peaks were made gets new ones; media.peaksProgress and media.peaksReady report how that goes.

Arguments:

- `channels`: array of integer
- `frames`: integer
- `media`: integer, required
- `pixels`: integer, required
- `start`: integer

Result:

- `available`: integer, required
- `channels`: array of object, required
- `level`: integer, required
- `media`: integer, required
- `status`: one of "pending" | "generating" | "ready" | "failed" | "recording", required

#### `media_prepareArchive`

Engine command `media.prepareArchive`.

Makes the project self-contained, so that its folder can be archived or moved: copies every media file from outside the project folder into its media folder and points the media at the copies. The copies run as a job: this returns at once, job.progress and job.finished events follow, and job.cancel stops it and removes the copies. Once they are done, the media are pointed at them as one undo step. The job's result lists the copies as copied, media files that couldn't be read as missing (they keep their paths), and media whose path changed while the job ran as changed (they keep the new path). The project must have been saved; save it again without embedding media to keep the new paths.

Result:

- `job`: object, required

#### `media_relink`

Engine command `media.relink`.

Points media files at other files, such as copies, as one undo step. Each new file must be an audio file with the media file's sample rate and channel count. An entry with from applies only if the media file is still there and still points at from; otherwise it is skipped. Returns the media files it relinked and the ones it skipped.

Arguments:

- `files`: array of object, required

Result:

- `relinked`: array of integer, required
- `skipped`: array of integer, required

#### `media_remove`

Engine command `media.remove`.

Removes an audio file that no clip uses.

Arguments:

- `media`: integer, required

#### `media_stretch`

Engine command `media.stretch`.

Time-stretches and pitch-shifts a media file, writes the result to path as a 32-bit float WAV file and adds it as a new media file; path must not be one of the project's media files. ratio is the new length over the old one, from 0.1 to 10 (default 1: unchanged); semitones and cents shift the pitch, together within 48 semitones either way (default 0). preset picks the algorithm: quality (default) and efficient for any material, drums, plucked, pads, vocals, mix and solo tuned for kinds of material at a lower CPU cost. formants keeps the spectral envelope when the pitch shifts, e.g. for voices. mode offline (default) analyzes the whole file first for the best result; realtime processes it the way playback does, with ratios from 0.25 to 4 and pitch shifts within 24 semitones.

Arguments:

- `cents`: number
- `formants`: boolean
- `media`: integer, required
- `mode`: one of "realtime" | "offline"
- `path`: string, required
- `preset`: one of "quality" | "efficient" | "drums" | "plucked" | "pads" | "vocals" | "mix" | "solo"
- `ratio`: number
- `semitones`: number

Result:

- `media`: object, required

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

### midi

#### `midi_bounce`

Engine command `midi.bounce`.

Bounces MIDI parts on one track into a single part from the first one's start to the last one's end, named after the first. Events keep their time; notes are cut where their part ended. Returns the tracks it added, the parts it added (parts), changed and removed.

Arguments:

- `parts`: array of integer, required

Result:

- `changedParts`: array of integer, required
- `parts`: array of integer, required
- `removedParts`: array of integer, required
- `tracks`: array of integer, required

#### `midi_deleteContinuousControllers`

Engine command `midi.deleteContinuousControllers`.

Removes the control changes of continuous controllers. Bank select, data entry, the switches 64-69 (such as the sustain pedal), data increment and decrement, NRPN, RPN and channel mode messages stay. Works on the MIDI parts in parts. Returns the parts it changed.

Arguments:

- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_deleteControllers`

Engine command `midi.deleteControllers`.

Removes control changes, pitch bend, channel and poly pressure; program changes and SysEx stay. Works on the MIDI parts in parts. Returns the parts it changed.

Arguments:

- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_deleteDoubles`

Engine command `midi.deleteDoubles`.

Of notes with the same pitch, channel and position, keeps the longest (the first added on a tie). Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_deleteNotes`

Engine command `midi.deleteNotes`.

Deletes notes shorter than minimumLength (ticks) or softer than minimumVelocity (0-1); at least one is required. removeWhen both deletes only notes below both minimums (default: either). Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `minimumLength`: integer
- `minimumVelocity`: number
- `notes`: array of integer
- `parts`: array of integer, required
- `removeWhen`: one of "either" | "both"

Result:

- `parts`: array of integer, required

#### `midi_deleteOverlaps`

Engine command `midi.deleteOverlaps`.

Shortens notes so that none still sounds when the next one on its channel starts: in mode mono the next one of the same pitch, in mode poly of any pitch. Notes that start together don't overlap. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `mode`: one of "mono" | "poly", required
- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_dissolve`

Engine command `midi.dissolve`.

Dissolves MIDI parts by MIDI channel or by pitch (by: channel or pitch): for each channel or pitch found in the parts of a track, a new MIDI track after it, named after the track and the channel or pitch (middle C, 60, is C3), routed to the same instrument, gets the events of that channel or pitch in parts at the same place. By channel SysEx stays, by pitch everything but notes and poly pressure; parts left empty are removed. optimize shortens the new parts to the bars their events play in. Returns the tracks it added, the parts it added (parts), changed and removed.

Arguments:

- `by`: one of "channel" | "pitch", required
- `optimize`: boolean
- `parts`: array of integer, required

Result:

- `changedParts`: array of integer, required
- `parts`: array of integer, required
- `removedParts`: array of integer, required
- `tracks`: array of integer, required

#### `midi_fixLengths`

Engine command `midi.fixLengths`.

Sets the length of notes, in ticks. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `length`: integer, required
- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_fixVelocities`

Engine command `midi.fixVelocities`.

Sets the velocity of notes, 0-1. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required
- `velocity`: number, required

Result:

- `parts`: array of integer, required

#### `midi_invert`

Engine command `midi.invert`.

Mirrors notes in time: each one starts where its mirror image ends. Without notes, controllers with a value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, and return to their last value where the range ends; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_legato`

Engine command `midi.legato`.

Extends notes to the start of the next note on their MIDI channel, plus overlap in ticks (negative: a gap; default 0). With betweenSelected, to the next of the given notes. The last notes keep their length. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `betweenSelected`: boolean
- `notes`: array of integer
- `overlap`: integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_merge`

Engine command `midi.merge`.

Merges the notes and controllers that start between start and end (project samples, like the locators, compared in samples as playback does) on the MIDI and instrument tracks in tracks (default: all of them) into a new part from start to end on the destination track, named name (default: the track's name). Notes keep their length, cut where their part ends. Without destination a new MIDI track is added at the end, routed to the instrument of the first track merged. eraseDestination first removes the destination's parts that lie between start and end, and the events of its other parts that start there. includeChase adds the last value of every controller and program before start at the new part's start. Returns the tracks it added, the parts it added (parts), changed and removed.

Arguments:

- `destination`: integer
- `end`: integer, required
- `eraseDestination`: boolean
- `includeChase`: boolean
- `name`: string
- `start`: integer, required
- `tracks`: array of integer

Result:

- `changedParts`: array of integer, required
- `parts`: array of integer, required
- `removedParts`: array of integer, required
- `tracks`: array of integer, required

#### `midi_pedalToNoteLength`

Engine command `midi.pedalToNoteLength`.

Extends notes that end while the sustain pedal (controller 64) on their channel is down to where it is released (the part's end if never), but not past the next note of the same pitch, and removes the sustain pedal events of their channels. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_restrictPolyphony`

Engine command `midi.restrictPolyphony`.

Shortens notes so that at most voices (1 or more) sound at once on each MIDI channel: a new note ends the one that started first, the lowest of notes that started together. A note that would lose its whole length is deleted. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required
- `voices`: integer, required

Result:

- `parts`: array of integer, required

#### `midi_reverse`

Engine command `midi.reverse`.

Reverses the order in which notes start, keeping their lengths: the last one starts where the first one started. Without notes, controllers with a value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, and return to their last value where the range ends; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_thinOut`

Engine command `midi.thinOut`.

Thins out continuous controllers, pitch bend and pressure, per controller and channel: removes events that repeat the value before them, and events closer than minimumDistance ticks (default a 64th note) to the event kept before them, unless the curve turns there, the next event is at least minimumDistance away or it is the last one. Bank select, data entry, the switches 64-69, NRPN, RPN and channel mode messages stay, like program changes and SysEx. Works on the MIDI parts in parts. Returns the parts it changed.

Arguments:

- `minimumDistance`: integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_transpose`

Engine command `midi.transpose`.

Transposes the notes of MIDI parts by semitones (-127 to 127). fromScale and toScale (a root 0-11, 0 is C, and a scale type) correct the notes from one scale to another: a note moves from its degree in fromScale to the same degree in toScale, and notes outside fromScale, or all of them when the scales have a different number of degrees, go to the closest note of toScale. lowest and highest (0-127) keep notes in range: a note that leaves it moves by octaves into it, or to the limit it crossed if no octave fits. Notes stay within 0-127 by octaves. Poly pressure follows when no notes are given. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `fromScale`: object
- `highest`: integer
- `lowest`: integer
- `notes`: array of integer
- `parts`: array of integer, required
- `semitones`: integer
- `toScale`: object

Result:

- `parts`: array of integer, required

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

#### `note_set`

Engine command `note.set`.

Changes notes of a MIDI part: each change names a note by id and gives what changes, position and length in ticks, pitch 0-127, velocity 0-1 or MIDI channel 0-15. Returns the changed notes.

Arguments:

- `notes`: array of object, required
- `part`: integer, required

Result:

- `notes`: array, required

### processor

#### `processor_list`

Engine command `processor.list`. Read-only.

Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units.

Result:

- `processors`: array, required

### project

#### `project_backup`

Engine command `project.backup`.

Backs the project up as it is now into folder, which must be empty or new: the project file, named name (by default after the project file, or else the project), and copies of its media in the folder Audio, which the project file refers to relative to itself, so that the folder opens on another machine. The project itself stays as it is. minimize keeps only the ranges of audio files that audio events play, with handles seconds (default 1) before and after; removeUnused leaves out clips that no audio event uses and media that no clip uses. currentVersionsOnly and skipVideo have no effect until clips have versions and projects have video. activate opens the backup once it is done, unless the project changed meanwhile. The backup runs as a job: this returns at once, job.progress and job.finished events follow, and job.cancel stops it and removes what it wrote. A folder that a running job writes into can't be used. The job's result is {file, files, missing, activated, activationError}: missing lists the media files that couldn't be read and keep their old paths; activated says whether the backup was opened, and activationError why not, if it was asked to be.

Arguments:

- `activate`: boolean
- `currentVersionsOnly`: boolean
- `folder`: string, required
- `handles`: number
- `minimize`: boolean
- `name`: string
- `removeUnused`: boolean
- `skipVideo`: boolean

Result:

- `job`: object, required

#### `project_get`

Engine command `project.get`. Read-only.

The whole project: busses, tracks in order, channels, media, clips, events and parts.

#### `project_load`

Engine command `project.load`.

Replaces the project with a DAWproject file and clears the undo history. Media files embedded in it are extracted to mediaFolder, by default a folder named after the file next to it. With revision, it loads only if the project is still at that revision (see project.revision), and fails otherwise, so that no change made meanwhile is lost.

Arguments:

- `mediaFolder`: string
- `path`: string, required
- `revision`: integer

#### `project_location`

Engine command `project.location`. Read-only.

Where the project is: its project file (set by project.save and project.load), the folder of that file, and the media folder in it where media.add with copy and media.prepareArchive put audio files. All null for a project that has no file yet.

Result:

- `file`: string or null, required
- `folder`: string or null, required
- `mediaFolder`: string or null, required

#### `project_new`

Engine command `project.new`.

Replaces the project with an empty one that has a stereo input and output bus, and clears the undo history.

#### `project_revision`

Engine command `project.revision`. Read-only.

A number that grows with every change of the project: every edit, undo and redo, and loading or starting a project. project.load with revision loads only if it hasn't changed.

Result:

- `revision`: integer, required

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

### settings

#### `settings_get`

Engine command `settings.get`. Read-only.

The value of a user setting; settings.list describes them all.

Arguments:

- `key`: one of "audio.backend" | "audio.blockSize" | "audio.device" | "audio.sampleRate" | "ui.scale" | "ui.theme", required. The setting.

Result:

- `key`: string, required
- `value`: any, required. The setting's value.

#### `settings_list`

Engine command `settings.list`. Read-only.

Every user setting with its key, description, JSON schema, default and value; the settings file, null when settings are kept in memory only; its version; and what happened when it was loaded: the status (inMemory, missing, loaded, migrated, reset or newer), the version the file had, the problems found, where the original file was kept, and whether the file is read-only for this session, so that changes apply but aren't saved (a newer GNUbase's file, or one that couldn't be backed up or locked).

Result:

- `file`: string or null, required
- `load`: object, required
- `settings`: array, required
- `version`: integer, required

#### `settings_reset`

Engine command `settings.reset`.

Puts a user setting back to its default, or every one without key, and saves the settings file. Returns the settings that were reset with their values; emits settings.changed with those that changed.

Arguments:

- `key`: one of "audio.backend" | "audio.blockSize" | "audio.device" | "audio.sampleRate" | "ui.scale" | "ui.theme". The setting.

Result:

- `values`: object, required

#### `settings_set`

Engine command `settings.set`.

Changes a user setting and saves the settings file, unless settings.list reports it read-only. User settings belong to the user, not to a project. Emits settings.changed with the changed value, unless it had that value already. The settings: `audio.backend` (string, default ""): The audio backend, such as wasapi or coreaudio, or empty for the preferred one; `audio.blockSize` (integer, 16 to 8192, default 256): Frames per audio block; `audio.device` (string, default ""): The audio device's id, or empty for the backend's default device; `audio.sampleRate` (number, 8000 to 768000, default 48000.0): The audio device's sample rate in Hz; `ui.scale` (number, 0.5 to 4, default 1.0): The UI scale, applied on top of each monitor's own scale; `ui.theme` (string, default "gnubase"): The theme's folder name in resources/themes.

Arguments:

- `key`: one of "audio.backend" | "audio.blockSize" | "audio.device" | "audio.sampleRate" | "ui.scale" | "ui.theme", required. The setting.
- `value`: any, required. The new value; it must fit the setting's schema.

Result:

- `key`: string, required
- `value`: any, required. The setting's value.

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

- `gnubase://audio.analyze`: Measures the captured output of channels (default: every output) from frame start for frames frames (default: to the end of the capture). Per channel: the peak and RMS level, and a spectrum summary with the strongest frequencies (at most peaks, default 5, up to 20) and the power of octave bands. For the channels together: the integrated loudness in LUFS (ITU-R BS.1770), null below 400 ms. Levels are linear and in dBFS (a full-scale sine's peak is 0 dBFS, its RMS -3 dBFS); dBFS is null for silence.
- `gnubase://audio.devices`: The audio backends of this build, the preferred one first, and the devices each has now: their ids, names, channel names, sample rates, block sizes, clock sources (and the current one), and whether the driver has a control panel. A backend that can't list its devices has an error instead.
- `gnubase://audio.get`: The audio device (its backend, id, name, format, channel names, latencies in frames, clock source and whether it has a control panel), whether it runs, how many frames it has processed and, if it is the dummy device, captured, and how many frames of audio track playback were silent because a disk stream was late.
- `gnubase://engine.commands`: Every command with its documentation, parameters and result.
- `gnubase://engine.version`: The GNUbase version.
- `gnubase://graph.get`: The project's audio processing graph: its nodes (busses, then channels in track order) with the nodes they receive audio and events from, and the order they are processed in.
- `gnubase://history.get`: The undo history: every step with its action, time (UTC) and details, the position (steps before it are done, the ones after it can be redone) and the maximum number of steps (0: no limit).
- `gnubase://job.list`: Every background job since the engine started, such as backups: id, kind, state (running, done, failed or cancelled), progress from 0 to 1, what it is doing, and its result or error once it ended.
- `gnubase://media.peakStatus`: The state of the waveform overviews of a media file, or of every media file: pending, generating (with progress from 0 to 1), ready, failed (with the error) or recording; the frames that have peaks; the peak file they were loaded from or written to; whether they were generated from the audio file rather than loaded. Peak files are kept next to their audio file as <file name>.gbpeaks, or in fallbackFolder where that can't be written.
- `gnubase://processor.list`: Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units.
- `gnubase://project.get`: The whole project: busses, tracks in order, channels, media, clips, events and parts.
- `gnubase://project.location`: Where the project is: its project file (set by project.save and project.load), the folder of that file, and the media folder in it where media.add with copy and media.prepareArchive put audio files. All null for a project that has no file yet.
- `gnubase://project.revision`: A number that grows with every change of the project: every edit, undo and redo, and loading or starting a project. project.load with revision loads only if it hasn't changed.
- `gnubase://settings.list`: Every user setting with its key, description, JSON schema, default and value; the settings file, null when settings are kept in memory only; its version; and what happened when it was loaded: the status (inMemory, missing, loaded, migrated, reset or newer), the version the file had, the problems found, where the original file was kept, and whether the file is read-only for this session, so that changes apply but aren't saved (a newer GNUbase's file, or one that couldn't be backed up or locked).
- `gnubase://transport.get`: Whether playback runs, the project cursor in samples, the locators and the modes.

<!-- End of the generated tool catalogue -->
