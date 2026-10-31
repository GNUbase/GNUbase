# MCP server

GNUbase has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server, so AI agents such as Claude Code can use everything the engine can do: create projects, add tracks and notes, play, render and measure audio, and run Lua.

- **Tools come from the engine's command registry.** Every engine command is a tool, named with `_` instead of `.` (`track.add` is `track_add`), with the command's parameter schema as its input schema and its result schema as its output schema. A command that only reads is marked read-only (`readOnlyHint`). The [tool catalogue](#tool-catalogue) at the end lists them all; the registry itself is `engine_commands`.
- **`script_run`** runs Lua code with the same API ([lua.md](lua.md)) in a sandbox, for work that takes many steps or for test cases written with `gnubase.test`.
- **UI tools:** with its UI, GNUbase also lets agents read the UI back and use it: windows, the DOM, screenshots, mouse and keyboard (`ui_dom`, `ui_click`, ...). See [UI tools](#ui-tools).
- **Events:** `events_take` returns what changed in the engine since the last call, such as `track.added` or `history.changed`, whoever changed it: this client, other MCP clients, Lua scripts or the UI. See [Events](#events).
- **Resources:** queries without required parameters can also be read as resources, `gnubase://<command>`, e.g. `gnubase://project.get`.
- **Errors:** a command that fails is a tool result with `isError: true` and the reason, such as `parameters.type must be one of [...]`, so the agent can correct itself. Malformed messages get JSON-RPC errors.
- The server speaks MCP revision 2025-06-18; HTTP clients may also send `MCP-Protocol-Version: 2025-03-26`.

How it fits into GNUbase is described in [architecture.md](architecture.md#scripting-mcp-and-tests).

## Starting the server

GNUbase serves MCP in headless mode, without UI, or with its UI over HTTP (below). Headless, the engine plays through the dummy audio device, which has no hardware and captures its output in memory, so agents can render audio offline and measure it.

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

**With the UI**: `--mcp-http` without `--headless` opens GNUbase's main window as usual and serves MCP over HTTP in the same way, with the [UI tools](#ui-tools) besides the engine's. It prints the URL and the token on stdout and serves until GNUbase quits. Over stdio only headless GNUbase serves MCP.

```bash
gnubase --mcp-http=8765 --project song.dawproject
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

## UI tools

GNUbase started with its UI and `--mcp-http` also has tools to look at the UI and use it, as a person would with the mouse and the keyboard, e.g. to test a feature end to end or to check how it looks. The UI is made of RmlUi documents (HTML-like, styled with CSS), so agents find elements with CSS selectors such as `#open`, `.track.selected` or `button`.

- `ui_windows` lists the windows: their ids, which is the main window, their size in pixels and their dp ratio, and the documents each shows. The other tools take a `window`; without one they use the main window.
- `ui_dom` reads the DOM: every element's tag, id, classes, other attributes, a form control's value, pseudo-classes such as `hover`, `focus` or `checked`, whether it's visible and its box. With a `selector` it returns the elements that match; `depth` limits how many levels of children come with them, and `depth: 0` returns the elements with their text.
- `ui_screenshot` returns a PNG of the window, or with a `selector` of one element.
- `ui_click`, `ui_move`, `ui_drag` and `ui_wheel` use the mouse on an element, by `selector` (and `index` among its matches), or on a point `x`, `y`. `ui_key` presses keys such as `Enter` or `Ctrl+S`; `ui_text` types text into the focused field. Each answers with the element it reached, so the agent sees what it hit.
- `ui_wait` waits until an element is visible, or has a text, or until none is, e.g. for a dialog that a click opens.

Boxes and points are in the window's pixels, as in its screenshots; on a monitor with a scale of 200 %, 1 dp is 2 pixels. An agent opening a dialog and reading it back:

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"ui_click","arguments":{"selector":"#rename"}}}
{"id":5,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"element":{"attributes":{"title":"Rename the track"},"box":{"height":30,"width":120,"x":10,"y":10},"id":"rename","pseudoClasses":["hover"],"tag":"button","text":"Rename track","visible":true}}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"ui_wait","arguments":{"selector":"#rename-dialog"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"ui_dom","arguments":{"selector":"#rename-dialog"}}}
{"id":7,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"elements":[{"box":{…},"children":[{"box":{…},"children":[{"text":"Enter a new name for Audio 01."}],"id":"message","tag":"p","visible":true},{"attributes":{"type":"text"},"box":{…},"id":"name","tag":"input","value":"Audio 01","visible":true},…],"id":"rename-dialog","tag":"body","visible":true}]}}}
```

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
- **UI tools act as you would.** With the UI, an agent can click, type and take screenshots in GNUbase's windows. They only reach GNUbase's own windows, never other programs or the OS's dialogs, such as file dialogs. Serving MCP with the UI is opt-in (`--mcp-http`), with the same token rules.
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
| The `ui_` tools are missing | GNUbase runs headless, which has no UI. Start it without `--headless`, with `--mcp-http`. |
| A `ui_` tool says the UI didn't answer | The UI thread was busy for 10 seconds, e.g. loading a large project, or GNUbase is quitting. Try again. |
| `ui_click` reports another element than the one asked for | Something covers it, such as a modal dialog; the click went to what is on top. `ui_screenshot` shows what the window looks like. |

## Tool catalogue

Generated from what the MCP server lists, with `script_run`: every tool by area, its engine command, whether it only reads, its arguments and its result. The tests fail when it is out of date; running them with `GNUBASE_UPDATE_DOCS=1` writes it again.

<!-- Generated from the MCP server's tools/list: run the tests with GNUBASE_UPDATE_DOCS=1 to update it. -->

### audioEvent

#### `audioEvent_add`

Engine command `audioEvent.add`.

Adds an audio event that plays a section of a clip on an audio track. position is in the track's time base (ticks or samples); offset and length are in the clip's sample frames, length defaulting to the rest of the clip. volume is in dB, from -144 to 6 (default 0); a muted event doesn't play. A track plays one event at a time: where events overlap, the one added last plays, unless they crossfade.

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

#### `audioEvent_setFades`

Engine command `audioEvent.setFades`.

Changes an audio event's fade in (from its start) or fade out (up to its end): length in the clip's sample frames (0: none) and shape: linear, exponential (slow, then fast), logarithmic (fast, then slow), sCurve (half a cosine) or equalPower (a quarter sine). Together they fit into the event. A crossfade replaces the fades at the ends it joins while it plays.

Arguments:

- `event`: integer, required
- `fadeIn`: object
- `fadeOut`: object

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

#### `audio_captureInput`

Engine command `audio.captureInput`. Read-only.

What an input channel of the audio device recorded at project positions from start, at most 1048576 frames: its input goes where the transport played the record offset before it arrived (see audio.get), only while the transport plays, and the last cycle lap over the ones before. Up to the device's captureSeconds (audio.setDevice) from position 0; positions not recorded are silent. Only while the device stands.

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
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_devices`

Engine command `audio.devices`. Read-only.

The audio backends of this build, the preferred one first, and the devices each has now: their ids, names, channel names, sample rates, block sizes, clock sources (and the current one), whether the driver has a control panel, whether it can monitor inputs directly (canMonitorDirectly), and whether the device is available. An unavailable device, e.g. one whose hardware isn't connected, has an unavailableReason; audio.setDevice still tries it. Some backends only learn that by opening or starting the device (ASIO). A backend that can't list its devices has an error instead.

Result:

- `backends`: array, required

#### `audio_get`

Engine command `audio.get`. Read-only.

The audio device (its backend, id, name, format, channel names, latencies in frames, clock source, whether it has a control panel, whether it can monitor directly and does (directMonitoring), the record shift and the record offset: inputLatency + outputLatency - recordShift, from 0 to 196608), whether it runs, how many frames it has processed and, if it is the dummy device, captured, how many frames of audio track playback were silent because a disk stream was late, and the processing threads (see audio.setProcessing). projectSampleRate is the project's sample rate; with sampleRateMismatch the device runs at another one, as it doesn't support the project's or was asked for another, and plays faster or slower. lost: the device that went away (its backend, device id, the reason it can't be opened and whether it ran and restarts), while the engine waits for it to return (see audio.reset); otherwise null. While a device is lost, device is the fallback that plays meanwhile (the user setting audio.fallbackDevice), or null.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_performance`

Engine command `audio.performance`. Read-only.

Performance data for the performance monitor. Loads are the share of a block's duration spent processing it, so above 1 the device drops out. load: averaged over about 0.3 s; peak: the highest block; overloads: blocks above 1 plus deviceOverloads, the dropouts the driver reported, also those it recovers from with a reset (a block that was both counts twice; a reset after a settings change counts nothing); cycles: blocks measured; threads: the load of each processing thread, the audio thread first. Peak, overloads and cycles count since the last audio.resetPerformance, also across audio.reset; everything starts over with a new device or new processing threads. diskLoad: how far the disk stream furthest behind has run empty, 0 to 1; diskDropouts: frames of audio track playback that were silent because a disk stream was late. blockSize: the device's block size in frames, 0 without a device.

Result:

- `blockSize`: integer, required
- `cycles`: integer, required
- `deviceOverloads`: integer, required
- `diskDropouts`: integer, required
- `diskLoad`: number, required
- `load`: number, required
- `overloads`: integer, required
- `peak`: number, required
- `threads`: array of number, required

#### `audio_ports`

Engine command `audio.ports`. Read-only.

The audio device's ports by name, in order, each with the name it is shown as and whether it is visible (audio.setPort), and the busses that use it (the bus ids); each bus's ports on this device, null where it has none (a child bus's are its parent's); and the missing ports: port names that busses use but the device doesn't have, with those busses. bus.remapPorts re-routes them. beyondDevice: busses without ports of their own (consecutive ports in bus order) whose channels run past the device's ports, with those channels, which are silent; bus.set gives them ports. Without a device there are no ports.

Result:

- `beyondDevice`: array, required
- `busses`: array, required
- `inputs`: array, required
- `missing`: array, required
- `outputs`: array, required

#### `audio_reconnect`

Engine command `audio.reconnect`.

Tries once to open the lost audio device again (lost in audio.get), as audio.reset does; does nothing unless a device is lost. While one is, the engine runs it on its own about every half second, one try at a time. While a fallback device plays, it tries only once the backend lists the lost device as available again, so that the fallback isn't interrupted, or every few seconds if the backend learns only by opening it whether it is there (ASIO). Tries that find the device still gone change nothing and are left out of engine.commandLog. Sends audio.changed when the device opens.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
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
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_reset`

Engine command `audio.reset`.

Opens the audio device again with the same settings and restarts it if it ran. Drivers ask for this when their settings change, e.g. in their control panel, or when their device goes away; the engine then resets on its own. A device that can't be opened, e.g. because it was unplugged, is lost (lost in audio.get): the engine goes on without it and tries again about every half second until it opens (audio.reconnect), then restarts it if it ran, so playback goes on where it stopped; audio.start and audio.stop meanwhile decide whether it does. With the user setting audio.fallbackDevice, a fallback device plays meanwhile and the engine switches back once the lost device opens; a reset while one plays tries the lost device and opens the fallback again if that fails. audio.setDevice ends the wait. The performance measurement goes on across a reset, which itself counts nothing: a driver that resets because of a dropout reports the dropout (see audio.performance). A reset that loses the device ends the measurement. Sends audio.changed when the device closes, opens or is lost, also when it fails after the device opened again.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_resetPerformance`

Engine command `audio.resetPerformance`.

Starts a new performance measurement, like clicking the overload indicator: clears the peak, the overloads and the cycles; the averaged loads go on. Immediately while the device is stopped, otherwise with its next block. Returns the measurement that ended, as audio.performance does, and sends it as the event audio.performanceReset.

Result:

- `blockSize`: integer, required
- `cycles`: integer, required
- `deviceOverloads`: integer, required
- `diskDropouts`: integer, required
- `diskLoad`: number, required
- `load`: number, required
- `overloads`: integer, required
- `peak`: number, required
- `threads`: array of number, required

#### `audio_set`

Engine command `audio.set`.

Changes the audio device's settings and keeps the others: the parameters of audio.setDevice except backend and device, e.g. sampleRate, blockSize, clockSource, recordShift or directMonitoring. The device opens again with them, as for audio.reset, and runs on if it ran. If it can't open with them, it opens again with the settings it had and the command fails; if those fail too, there is no device. Fails while a fallback plays for a lost device (see audio.reset). Sends audio.changed, also when it fails after the device closed: with the device opened again with its old settings, at position 0, or with none.

Arguments:

- `blockSize`: integer
- `captureSeconds`: number
- `clockSource`: string
- `directMonitoring`: boolean
- `input`: object
- `inputChannels`: integer
- `inputLatency`: integer
- `loopback`: boolean
- `outputChannels`: integer
- `outputLatency`: integer
- `recordShift`: integer
- `sampleRate`: number

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_setDevice`

Engine command `audio.setDevice`.

Replaces the audio device. backend: one from audio.devices, by default "dummy". device: its id, by default the backend's default device. sampleRate: by default the project's if the device supports it, otherwise the driver's current rate; when the project's sample rate changes, the device follows it where it can. blockSize: by default the driver's preferred size. clockSource: a name from the device's clockSources. The device starts stopped at position 0. Another backend's device is closed before the new one opens, since drivers often allow only one; if opening fails, there is no device.
Recorded input goes where the transport played the device's input + output latency before it arrived, so what is played along to or looped back lines up with what played (recordOffset in audio.get). recordShift (frames, -65536 to 65536) moves recordings that much later, for drivers that report their latency wrong. captureSeconds (0 to 600, by default 0, or 60 for the dummy device) keeps that much of what the inputs record in memory for audio.captureInput, e.g. for a loopback test. directMonitoring: the driver monitors audio tracks' inputs itself, without the engine's latency, for tracks that play into an output bus (canMonitorDirectly in audio.devices says which can); their fader and pan set its level and position, automation doesn't reach it.
The dummy device has no hardware: its output is captured in memory (audio.capture) and it renders offline (audio.render). It supports 44100, 48000, 88200, 96000, 176400 and 192000 Hz and opens with any rate from 8000 Hz. Its defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 outputs (inputChannels, outputChannels), 60 s of capture (captureSeconds, also of its output). Its input is silent, or a built-in generator (see processor.list) with parameters by name. It reports inputLatency and outputLatency (frames, 0 to 65536, default 0); with loopback its inputs carry its outputs back that much later, as a cable from each output to the input of the same number would, which needs at least a block. It monitors directly by adding inputs to its outputs.
Sends audio.changed, also when it fails after closing the old device.

Arguments:

- `backend`: string
- `blockSize`: integer
- `captureSeconds`: number
- `clockSource`: string
- `device`: string
- `directMonitoring`: boolean
- `input`: object
- `inputChannels`: integer
- `inputLatency`: integer
- `loopback`: boolean
- `outputChannels`: integer
- `outputLatency`: integer
- `recordShift`: integer
- `sampleRate`: number

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_setPort`

Engine command `audio.setPort`.

Changes how a port of the audio device is shown, given by whether it is an input and its system name (name in audio.ports). showAs: the name routing menus show; empty shows the system name. visible: whether routing menus offer it; busses on a hidden port keep it. The settings are kept per device while GNUbase runs. Returns the port as audio.ports lists it, and sends audio.ports as the event audio.portsChanged.

Arguments:

- `input`: boolean, required
- `name`: string, required
- `showAs`: string
- `visible`: boolean

Result:

- `busses`: array, required
- `name`: string, required
- `showAs`: string, required
- `visible`: boolean, required

#### `audio_setProcessing`

Engine command `audio.setProcessing`.

Multi-processing: threads is how many threads process the graph, the audio thread included (1 to 64; default: half the hardware threads, about one per core). Independent tracks then run in parallel; the output is the same for any number of threads. The device must be stopped. activeThreads in the result is how many process, and limitedBy why that is fewer: multiProcessing is off, the hardware has fewer threads, worker threads didn't get real-time priority (on Linux: no real-time privileges and no rtkit), or the system refused to start them (system). Fails, keeping the previous settings, if threads can't be started.

Arguments:

- `multiProcessing`: boolean
- `threads`: integer

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_simulateDeviceLoss`

Engine command `audio.simulateDeviceLoss`.

Only the dummy device: simulates its hardware being unplugged (connected false) and its driver asking for a reset, after which the device is lost (see audio.reset), or plugged in again (connected true), after which the engine opens it again on its own. While it is unplugged, audio.setDevice can't open it either. The dummy device as a fallback (audio.fallbackDevice) can't be unplugged.

Arguments:

- `connected`: boolean, required

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_simulateOverload`

Engine command `audio.simulateOverload`.

Only the dummy device: simulates its driver reporting a dropout, which audio.performance counts in deviceOverloads and overloads. Returns the performance data, as audio.performance does.

Result:

- `blockSize`: integer, required
- `cycles`: integer, required
- `deviceOverloads`: integer, required
- `diskDropouts`: integer, required
- `diskLoad`: number, required
- `load`: number, required
- `overloads`: integer, required
- `peak`: number, required
- `threads`: array of number, required

#### `audio_start`

Engine command `audio.start`.

Starts the audio device; the dummy device processes one block per block duration (simulated real time) until stopped. While the device is lost (see audio.reset), it starts once it returns (restart in lost), and audio.changed announces that.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_stop`

Engine command `audio.stop`.

Stops the audio device. Does nothing if it isn't running. While the device is lost (see audio.reset), it stays stopped once it returns (restart in lost), and audio.changed announces that.

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

#### `audio_updateLatency`

Engine command `audio.updateLatency`.

Reads the audio device's latencies again and places recordings by the new record offset (see audio.get); a running device keeps running and switches at its next block. Drivers that learn their latency after the device opens, or whose latency changes with the graph, report the change; the engine then runs this on its own. Only for the dummy device, and only while it is stopped: inputLatency and outputLatency (default: as they are) first change what it reports, and with loopback its round trip, as such a driver's would. Sends audio.changed if anything changed.

Arguments:

- `inputLatency`: integer
- `outputLatency`: integer

Result:

- `capturedFrames`: integer, required
- `device`: object or null, required
- `droppedFrames`: integer, required
- `lost`: object or null, required
- `playbackDropouts`: integer, required
- `position`: integer, required
- `processing`: object, required
- `running`: boolean, required

### autoFades

#### `autoFades_set`

Engine command `autoFades.set`.

Changes the auto fades and crossfades of the project, or of an audio track: fadeIn and fadeOut where an event has no fade of its own at that end (they never stack), crossfades where an event starts on top of another one or ends above one that goes on, unless they crossfade already; there an event's own fade is the crossfade's, mirrored by the event behind. length is 1 to 500 ms; shapes as for audioEvent.setFades; crossfadeMode equalGain or equalPower, with crossfadeShape the fade in as for crossfade.set. A track uses the project's settings until it gets its own; useProject: true goes back to them. Returns the settings that apply.

Arguments:

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

Result:

- `autoFades`: object, required

### automationLane

#### `automationLane_add`

Engine command `automationLane.add`.

Adds an automation lane to a track: the curve of one parameter. target.type says which: volume (the channel's fader, in dB), pan, panLeft and panRight (-1 to 1, see channel.set) on tracks with audio channels; instrument, a parameter of an instrument track's built-in instrument (target.parameter, an id from processor.list, in its range); controller (target.number 0-127), pitchBend (0.5 is no bend) and channelPressure, all 0 to 1, on MIDI and instrument tracks, with target.channel 0-15 (default 0). A track has one lane per target. read (default true): the curve plays; without it, or without points, the parameter keeps its static value.

Arguments:

- `read`: boolean
- `target`: object, required
- `track`: integer, required

Result:

- `lane`: object, required

#### `automationLane_list`

Engine command `automationLane.list`. Read-only.

The automation lanes of a track, or of every track in track order, with their points sorted by position.

Arguments:

- `track`: integer

Result:

- `lanes`: array, required

#### `automationLane_remove`

Engine command `automationLane.remove`.

Removes an automation lane with its points.

Arguments:

- `lane`: integer, required

#### `automationLane_set`

Engine command `automationLane.set`.

Turns reading an automation lane on or off.

Arguments:

- `lane`: integer, required
- `read`: boolean

Result:

- `lane`: object, required

### automationPoint

#### `automationPoint_add`

Engine command `automationPoint.add`.

Adds points to an automation lane: position in the track's time base (ticks on musical tracks, so automation follows tempo changes; samples on linear ones, so it keeps its time), value in the target's unit, shape of the curve to the next point (step, linear or curve; default linear) and for curves the bend, curve, from -1 to 1 (above 0 the value moves fast at first). Before the first point the curve has its value, after the last point the last one's; points at one position make a jump. A ramp of volume moves the gain in a straight line.

Arguments:

- `lane`: integer, required
- `points`: array of object, required

Result:

- `points`: array, required

#### `automationPoint_remove`

Engine command `automationPoint.remove`.

Removes points from an automation lane.

Arguments:

- `lane`: integer, required
- `points`: array of integer, required

#### `automationPoint_set`

Engine command `automationPoint.set`.

Changes points of an automation lane: their position, value, shape or curve.

Arguments:

- `lane`: integer, required
- `points`: array of object, required

Result:

- `points`: array, required

### automation

#### `automation_get`

Engine command `automation.get`. Read-only.

The automation settings. followsEvents (default false): when an audio event or MIDI part moves, the automation of its track in its range moves with it and replaces the points where it lands.

Result:

- `followsEvents`: boolean, required

#### `automation_set`

Engine command `automation.set`.

Changes the automation settings (see automation.get). A setting, so it stays when a new project starts.

Arguments:

- `followsEvents`: boolean

Result:

- `followsEvents`: boolean, required

### bus

#### `bus_add`

Engine command `bus.add`.

Adds an input or output bus. format is its speaker arrangement (default stereo): mono, stereo, or a surround format with a fixed speaker order, e.g. 5.1 as L R C LFE Ls Rs. channels (1 to 8) picks mono, stereo, LRC, quadro, 5.0, 5.1, 7.0 or 7.1 instead. Without a name it is named after its format and direction, e.g. "Stereo In 2". ports: the device port of each channel by name (see audio.ports), an empty name for none; without ports the bus takes consecutive device ports in bus order. A port can serve several busses. The first output bus becomes the main mix. A child bus routes to some channels of its parent, a top-level bus of the same direction with more channels (direction may be left out): parentChannels are the parent's channel indices, one per channel of the child, by default the speakers both formats have; it uses its parent's ports.

Arguments:

- `channels`: integer
- `direction`: one of "input" | "output"
- `format`: one of "mono" | "stereo" | "LRC" | "LRS" | "LRC+LFE" | "LRS+LFE" | "LRCS" | "LRCS+LFE" | "quadro" | "quadro+LFE" | "5.0" | "5.1" | "6.0 cine" | "6.0 music" | "7.0" | "7.1"
- `name`: string
- `parent`: integer
- `parentChannels`: array of integer
- `ports`: array of string

Result:

- `bus`: object, required

#### `bus_applyPreset`

Engine command `bus.applyPreset`.

Replaces the project's busses of a direction with a preset's (see bus.presets). Channels that used the old busses move to the new bus at the same place, or to none (inputs) or the main mix (outputs). Returns the new busses.

Arguments:

- `direction`: one of "input" | "output", required
- `name`: string, required

Result:

- `busses`: array, required

#### `bus_presets`

Engine command `bus.presets`. Read-only.

The bus presets of a direction: standard ones with one bus of a common format, hardware ones made from the audio device's ports (all mono, or stereo pairs), and the user's own (bus.savePreset), which hide others of their name. Each has a name, a kind and its busses: name, format, ports and children (name, format, parentChannels).

Arguments:

- `direction`: one of "input" | "output", required

Result:

- `presets`: array, required

#### `bus_remapPorts`

Engine command `bus.remapPorts`.

Re-routes ports, e.g. missing ones (see audio.ports) after opening a project made on other hardware: ports maps port names to the names that replace them in every bus of the direction. Returns the busses that changed.

Arguments:

- `direction`: one of "input" | "output", required
- `ports`: object, required

Result:

- `busses`: array, required

#### `bus_remove`

Engine command `bus.remove`.

Removes a bus and its child busses; channels connected to them are disconnected. Removing the main mix makes the next output bus the main mix.

Arguments:

- `bus`: integer, required

#### `bus_removePreset`

Engine command `bus.removePreset`.

Removes one of the user's own bus presets.

Arguments:

- `direction`: one of "input" | "output", required
- `name`: string, required

#### `bus_rename`

Engine command `bus.rename`.

Renames several busses with an incrementing number or letter: name ends in a number ("Out 1", "Out 01") or in a space and a capital letter ("Bus A", up to Z). The first bus given gets the name as given, the others follow in bus order from there, continuing from the top.

Arguments:

- `busses`: array of integer, required
- `name`: string, required

Result:

- `busses`: array, required

#### `bus_savePreset`

Engine command `bus.savePreset`.

Saves the project's busses of a direction as a user preset, for every project (the user setting audio.busPresets), replacing one of the same name. A preset named Default is applied to new projects.

Arguments:

- `direction`: one of "input" | "output", required
- `name`: string, required

#### `bus_set`

Engine command `bus.set`.

Renames a bus, changes its ports (as in bus.add; [] for consecutive ports in bus order) or makes it the main mix (mainMix: true), the output bus new channels are routed to and that is monitored.

Arguments:

- `bus`: integer, required
- `mainMix`: boolean
- `name`: string
- `ports`: array of string

Result:

- `bus`: object, required

### channel

#### `channel_set`

Engine command `channel.set`.

Changes a channel's format (or channels, as in bus.add), input (an input bus) or output (an output bus or a group channel; for MIDI tracks an instrument channel). 0 disconnects. Audio converts between formats where a channel feeds a destination of another one: speakers both formats have connect, the others are mixed down (5.1 to stereo: C and LFE into L and R at -3 dB, Ls into L and Rs into R at -6 dB; 7.1 to 5.1: Sl and Sr into the front and surround sides at -3 dB) or up (mono to C, or to L and R at the pan law's center). instrument is the built-in instrument an instrument track plays (see processor.list), or empty for none. volume is the fader in dB, from -144 (silence) to the project's maxVolume (default 0); values beyond them stop there. The panner depends on the routing: a mono channel on a stereo destination has its position pan, -1 (left) to 1 (right), placed with the project's pan law; a stereo channel on a stereo or mono destination has the balance panner (panner balance, the default: pan turns down the other side) or the combined panner (panner combined: panLeft and panRight place its left and right channel, by default -1 and 1). panBypass plays mono channels at the center and stereo ones hard left and right. Destinations wider than stereo have no panner yet. Automation of volume, pan, panLeft and panRight overrides them while it plays; changes glide over 5 ms. MIDI channels have no audio: midiVolume and midiPan (0 to 127, 64 is the center pan, null for none, the default) are sent to the instrument as MIDI volume (CC 7) and pan (CC 10) on MIDI channel 1, whenever they change and when playback starts or jumps; automation of those controllers overrides them. monitor and recordEnable are an audio track's buttons: monitor hears the input bus, recordEnable arms the track for recording and, by the user setting audio.autoMonitoring, monitors too: manual never, whileRecordEnabled always, whileRecording while the transport records, tapeMachine while it stands or records (the default). Monitoring glides in and out over one audio block.

Arguments:

- `channel`: integer, required
- `channels`: integer
- `format`: one of "mono" | "stereo" | "LRC" | "LRS" | "LRC+LFE" | "LRS+LFE" | "LRCS" | "LRCS+LFE" | "quadro" | "quadro+LFE" | "5.0" | "5.1" | "6.0 cine" | "6.0 music" | "7.0" | "7.1"
- `input`: integer
- `instrument`: string
- `midiPan`: integer or null
- `midiVolume`: integer or null
- `monitor`: boolean
- `output`: integer
- `pan`: number
- `panBypass`: boolean
- `panLeft`: number
- `panRight`: number
- `panner`: one of "balance" | "combined"
- `recordEnable`: boolean
- `volume`: number

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

#### `clip_setFades`

Engine command `clip.setFades`.

Changes a clip's fade in or fade out, at its media file's start and end: length in sample frames (0: none) and shape, as for audioEvent.setFades. Every event that plays the clip gets them.

Arguments:

- `clip`: integer, required
- `fadeIn`: object
- `fadeOut`: object

Result:

- `clip`: object, required

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

### crossfade

#### `crossfade_remove`

Engine command `crossfade.remove`.

Removes the crossfades into and out of an audio event.

Arguments:

- `event`: integer, required

#### `crossfade_set`

Engine command `crossfade.set`.

Crossfades from an audio event into the next one on its track, where they overlap: to must start inside from and end after it. Both play there; from fades out up to its end over outLength (from's clip frames), to fades in from its start over length (to's clip frames); 0, the default, is the whole overlap, so the crossfade is symmetric. mode equalGain (default): the fade out mirrors the fade in (shape linear, exponential, logarithmic or sCurve), so their gains add up to 1; equalPower: quarter sines whose power adds up to 1; custom: shape and outShape of their own. An event fades out into one event only. Changing a crossfade keeps what isn't given. After an edit that moves the events apart, the crossfade rests and both play their own fades until they overlap like that again. Returns to.

Arguments:

- `from`: integer, required
- `length`: integer
- `mode`: one of "equalGain" | "equalPower" | "custom"
- `outLength`: integer
- `outShape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `shape`: one of "linear" | "exponential" | "logarithmic" | "sCurve" | "equalPower"
- `to`: integer, required

Result:

- `event`: object, required

### engine

#### `engine_commandLog`

Engine command `engine.commandLog`. Read-only.

The last 256 commands that ran, oldest first, without queries: what the UI and other clients did. Each with its request id (events name it too), the client that sent it (ui, mcp, lua, app for the command line, engine for the engine's own commands), the command, its parameters, and the error if it failed. Commands the engine ran on its own that changed nothing, such as tries to open a lost audio device again, are left out.

Result:

- `commands`: array of object, required

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

### event

#### `event_copy`

Engine command `event.copy`.

Copies audio events and MIDI parts, moved by delta project samples or by ticks and onto track as event.move moves them. Copies of audio events play the same clip; shared copies of MIDI parts share their notes and controllers with the original, so that editing one edits all. Returns the copies.

Arguments:

- `delta`: integer
- `events`: array of integer, required
- `shared`: boolean
- `ticks`: integer
- `track`: integer

Result:

- `events`: array of integer, required

#### `event_duplicate`

Engine command `event.duplicate`.

Duplicates audio events and MIDI parts count times (default 1, at most 1000; Edit > Repeat) one after the other, from the end of the last one, keeping their positions relative to each other; shared makes shared copies of MIDI parts. Returns the copies.

Arguments:

- `count`: integer
- `events`: array of integer, required
- `shared`: boolean

Result:

- `events`: array of integer, required

#### `event_fillLoop`

Engine command `event.fillLoop`.

Fills the range from start to end (project samples, default the locators) with copies of audio events and MIDI parts, one after the other on the grid of their length, cutting the last one at end. shared makes shared copies of MIDI parts. Returns the copies.

Arguments:

- `end`: integer
- `events`: array of integer, required
- `shared`: boolean
- `start`: integer

Result:

- `events`: array of integer, required

#### `event_get`

Engine command `event.get`. Read-only.

Tells where audio events and MIDI parts are, as the info line shows them: for each its kind (audio or midi), track, name (an audio event's clip's), start, end and length in project samples, start and end in ticks, mute, lock and its shared copies' group (0: none).

Arguments:

- `events`: array of integer, required

Result:

- `events`: array of object, required

#### `event_glue`

Engine command `event.glue`.

Glues audio events or MIDI parts of one track into the first of them. Audio events glue where one continues the other in the same clip, as after a split; MIDI parts become one part from the first start to the last end, with all their notes and controllers. Returns the event.

Arguments:

- `events`: array of integer, required

Result:

- `event`: integer, required

#### `event_move`

Engine command `event.move`.

Moves audio events and MIDI parts (events, by id) by delta project samples or by ticks, which are exact on musical tracks, keeping their positions relative to each other, and onto track if given, which must be of their type. Locked events don't move. Returns the events.

Arguments:

- `delta`: integer
- `events`: array of integer, required
- `ticks`: integer
- `track`: integer

Result:

- `events`: array of integer, required

#### `event_remove`

Engine command `event.remove`.

Removes audio events and MIDI parts. Locked ones can't be removed.

Arguments:

- `events`: array of integer, required

#### `event_resize`

Engine command `event.resize`.

Moves the start or the end (edge) of audio events and MIDI parts by delta project samples or by ticks, within the clip and at least a sample frame or a tick long. normal sizing (the default) reveals or hides content; moveContents moves the content with the edge. A MIDI part's notes and controllers that end up before its start are removed. With zeroCrossing, normal sizing moves audio events' edges to the nearest point within 50 ms where the audio crosses zero. Locked events don't change. Returns the events.

Arguments:

- `delta`: integer
- `edge`: one of "start" | "end", required
- `events`: array of integer, required
- `mode`: one of "normal" | "moveContents"
- `ticks`: integer
- `zeroCrossing`: boolean

Result:

- `events`: array of integer, required

#### `event_set`

Engine command `event.set`.

Mutes or unmutes and locks or unlocks audio events and MIDI parts. Muted ones don't play; locked ones can't be moved, resized, split, glued or removed by the event commands. Returns the events.

Arguments:

- `events`: array of integer, required
- `lock`: boolean
- `mute`: boolean

Result:

- `events`: array of integer, required

#### `event_setSnapPoint`

Engine command `event.setSnapPoint`.

Sets audio events' snap point at position (project samples, default the project cursor), which must be inside each of them (Snap Point to Cursor): where the event snaps to the grid when it moves, and attracts other events. It is a point in the clip, so it stays with the audio when the event is resized or split. Returns the events.

Arguments:

- `events`: array of integer, required
- `position`: integer

Result:

- `events`: array of integer, required

#### `event_shuffle`

Engine command `event.shuffle`.

Shuffle (snap type shuffle): events that adjoin each other on one track change places with the events that adjoin them. They go where position (project samples, where the first of them would start) puts them: after each other event whose middle it is past. The others close up and make room, so the row keeps its start and has no gaps. Locked events end the row. Returns the events.

Arguments:

- `events`: array of integer, required
- `position`: integer, required

Result:

- `events`: array of integer, required

#### `event_split`

Engine command `event.split`.

Splits audio events and MIDI parts at positions (project samples): events, or without it every unlocked one that a position cuts. A MIDI part's notes stay in the part they start in, or with splitNotes are cut in two. With zeroCrossing, audio events split at the nearest point within 50 ms and inside the event where their audio crosses zero. Returns the new events after the splits.

Arguments:

- `events`: array of integer
- `positions`: array of integer, required
- `splitNotes`: boolean
- `zeroCrossing`: boolean

Result:

- `events`: array of integer, required

#### `event_toRealCopy`

Engine command `event.toRealCopy`.

Converts shared copies to real copies: a MIDI part gets notes and controllers of its own, an audio event a clip of its own. Returns the events.

Arguments:

- `events`: array of integer, required

Result:

- `events`: array of integer, required

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

### marker

#### `marker_activateTrack`

Engine command `marker.activateTrack`.

Makes a marker track the active one, which marker commands use when they are given no track.

Arguments:

- `track`: integer, required

#### `marker_add`

Engine command `marker.add`.

Adds a position marker, or with type cycle a cycle marker, to a marker track (default: the active one; without a marker track one is added). position and end are in the track's time base (ticks or samples) or "cursor". A position marker defaults to the cursor, a cycle marker to the locators; a cycle marker ends at least one sample after its position. Position and cycle markers are numbered separately on each track; a new marker gets the first free number unless one is given.

Arguments:

- `color`: string or null
- `end`: integer or string
- `name`: string
- `number`: integer
- `position`: integer or string
- `track`: integer
- `type`: one of "position" | "cycle"

Result:

- `marker`: object, required

#### `marker_list`

Engine command `marker.list`. Read-only.

The markers of a marker track (default: the active one; track 0 if there is none) in timeline order, with their position and end also in the project's samples.

Arguments:

- `track`: integer

Result:

- `markers`: array, required
- `track`: integer, required

#### `marker_locate`

Engine command `marker.locate`.

Moves the project cursor to a marker, given by id (marker) or by its number on a marker track (default: the active one) and type (default: position). Cycle markers locate to their start.

Arguments:

- `marker`: integer
- `number`: integer
- `track`: integer
- `type`: one of "position" | "cycle"

Result:

- `marker`: object or null, required
- `position`: integer, required

#### `marker_locateNext`

Engine command `marker.locateNext`.

Moves the project cursor to the next marker after it on a marker track (default: the active one): position markers and the start and end of cycle markers. Without one the cursor stays and marker is null. Returns the marker and the cursor in samples.

Arguments:

- `track`: integer

Result:

- `marker`: object or null, required
- `position`: integer, required

#### `marker_locatePrevious`

Engine command `marker.locatePrevious`.

Moves the project cursor to the previous marker before it on a marker track (default: the active one), like marker.locateNext.

Arguments:

- `track`: integer

Result:

- `marker`: object or null, required
- `position`: integer, required

#### `marker_range`

Engine command `marker.range`. Read-only.

The range between the markers around a position in samples, on a marker track (default: the active one): from the last marker at or before it to the next one, counting position markers and the start and end of cycle markers. A range selection between markers spans it.

Arguments:

- `position`: integer, required
- `track`: integer

Result:

- `end`: integer, required
- `start`: integer, required

#### `marker_remove`

Engine command `marker.remove`.

Removes markers.

Arguments:

- `markers`: array of integer, required

#### `marker_renumber`

Engine command `marker.renumber`.

Numbers a marker track's position or cycle markers again from 1, in timeline order (default: the active track).

Arguments:

- `track`: integer
- `type`: one of "position" | "cycle", required

#### `marker_set`

Engine command `marker.set`.

Changes a marker: moves it (position, in the track's time base or "cursor"; a cycle marker keeps its length unless end is given), renames it, changes its color or number, or moves it to another marker track, where it takes the first free number if its own is taken.

Arguments:

- `color`: string or null
- `end`: integer or string
- `marker`: integer, required
- `name`: string
- `number`: integer
- `position`: integer or string
- `track`: integer

Result:

- `marker`: object, required

#### `marker_setLocators`

Engine command `marker.setLocators`.

Sets the left and right locators to a cycle marker's start and end, given by id (marker) or by its number on a marker track (default: the active one).

Arguments:

- `marker`: integer
- `number`: integer
- `track`: integer

Result:

- `left`: integer, required
- `marker`: object, required
- `right`: integer, required

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

### midiFile

#### `midiFile_export`

Engine command `midiFile.export`. Read-only.

Exports MIDI and instrument tracks (tracks, default all of them) as a standard MIDI file of type 0 (one track with every MIDI channel) or 1 (default: a tempo track, then a track per exported track), with ppq ticks per quarter note (24-960, default 480). The file has the notes and controllers as they play, converted to MIDI 1.0, the track names, tempo and time signatures, and the markers unless markers is false. A note ends where the next note of its pitch and channel starts, so that overlapping notes read back the same. range {start, end} in ticks exports only what starts in it, moved to the file's start. Returns the number of tracks in the file.

Arguments:

- `markers`: boolean
- `path`: string, required
- `ppq`: integer
- `range`: object
- `tracks`: array of integer
- `type`: one of 0 | 1

Result:

- `tracks`: integer, required

#### `midiFile_import`

Engine command `midiFile.import`.

Imports a standard MIDI file (type 0, 1 or 2) into new MIDI tracks (trackType midi, the default) or instrument tracks, one part per track from the bar of its first event to the bar after its last. A type 0 file gets a track per MIDI channel; the sequences of a type 2 file are laid out one after another, each from a bar start. Names that aren't UTF-8 are read as Windows-1252. Unless tempo is false, the file's tempo and time signatures replace the project's; unless markers is false, its markers are added to the active marker track (one is added if there is none). One undo step. Returns the new tracks and parts.

Arguments:

- `markers`: boolean
- `path`: string, required
- `tempo`: boolean
- `trackType`: one of "midi" | "instrument"

Result:

- `parts`: array, required
- `tracks`: array, required

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

#### `midi_capture`

Engine command `midi.capture`. Read-only.

What instruments played since the last call, at most 4096 MIDI 1.0 messages, oldest first, each with its instrument track's channel, its frame position on the audio device and its bytes: the events of the track's parts, of the MIDI tracks routed to it and what midi.inject played into them, with controller automation and the note-offs of a stop. A program change with a bank comes as bank select MSB and LSB, then the program change; SysEx isn't captured. dropped counts the messages that didn't fit: while nobody reads, newer messages are dropped. start: true starts a fresh capture; it drops what was captured so far and returns nothing. Each call empties what it returns, so a repeated call doesn't return the same messages again.

Arguments:

- `start`: boolean

Result:

- `captured`: array, required
- `dropped`: integer, required

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

#### `midi_inject`

Engine command `midi.inject`.

Plays MIDI 1.0 channel messages into a MIDI or instrument track's channel as if they came from the track's MIDI input: the track sends them on to its instrument, before the events of its parts, also while the transport stands. bytes: a status byte from 0x80 to 0xEF and its data bytes. position: the message's frame on the audio device, as audio.capture and midi.monitor count them; messages for frames the device has processed already play at the start of its next block, those for one frame in the order they came. They wait until the device processes their block, at most 4096 at a time, and are dropped when the device changes; a call whose messages don't all fit fails and injects none. midi.capture returns what instruments played.

Arguments:

- `channel`: integer, required
- `messages`: array of object, required

#### `midi_invert`

Engine command `midi.invert`.

Mirrors notes in time: each one starts where its mirror image ends. Without notes, controllers with a value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, and return to their last value where the range ends, or on the part's last tick if the range reaches its end; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

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

Merges the notes and controllers that start between start and end (project samples, like the locators, compared in samples as playback does) on the MIDI and instrument tracks in tracks (default: all of them) into a new part on the destination track from start, or from the first merged event if that is earlier, to end, named name (default: the track's name). Notes keep their length, cut where their part ends. Without destination a new MIDI track is added at the end, routed to the instrument of the first track merged. eraseDestination first removes the destination's parts that lie between start and end, and the events of its other parts that start there. includeChase adds the last value of every controller and program before start at the new part's start. Returns the tracks it added, the parts it added (parts), changed and removed.

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

#### `midi_monitor`

Engine command `midi.monitor`. Read-only.

The MIDI input received since the last call from every open input, at most 4096 messages, each with its port, its frame position on the audio device and its bytes. dropped counts what didn't fit: while nobody reads, newer messages are dropped. start: true starts afresh; it drops what was received so far and returns nothing. Each call empties what it returns. Input reaches the monitor while an audio device processes blocks, which places it in time.

Arguments:

- `start`: boolean

Result:

- `dropped`: integer, required
- `received`: array, required

#### `midi_pedalToNoteLength`

Engine command `midi.pedalToNoteLength`.

Extends notes that end while the sustain pedal (controller 64) on their channel is down to where it is released (the part's end if never), but not past the next note of the same pitch, and removes the sustain pedal events of their channels. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_ports`

Engine command `midi.ports`. Read-only.

The MIDI ports of every MIDI backend, inputs and outputs: backend, id, system name, the name they are shown as (showAs), whether they are visible, whether an input belongs to all MIDI inputs, whether the port is present now and open, and whether the system speaks MIDI 2.0 with it. Ports that were unplugged stay listed with their settings, not present.

Result:

- `ports`: array, required

#### `midi_refresh`

Engine command `midi.refresh`.

Lists the MIDI ports again: opens new and returning visible inputs and marks unplugged ports as not present. Runs on its own when a backend reports that its ports changed.

Result:

- `ports`: array, required

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

Reverses the order in which notes start, keeping their lengths: the last one starts where the first one started. Without notes, controllers with a value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, and return to their last value where the range ends, or on the part's last tick if the range reaches its end; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place. Works on the MIDI parts in parts; notes limits it to these notes of the parts. Returns the parts it changed.

Arguments:

- `notes`: array of integer
- `parts`: array of integer, required

Result:

- `parts`: array of integer, required

#### `midi_setPort`

Engine command `midi.setPort`.

Changes a MIDI port's settings. The port is given by its id, whether it is an input, and its backend if ids repeat. showAs: the name it is shown as; empty shows the system name. visible: hidden inputs are closed. inAllInputs, for inputs: whether tracks set to all MIDI inputs receive it. Returns the port.

Arguments:

- `backend`: string
- `id`: string, required
- `inAllInputs`: boolean
- `input`: boolean, required
- `showAs`: string
- `visible`: boolean

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

Adds notes to a MIDI part: position from the part's start and length in ticks, pitch 0-127, velocity 0-1, release (note-off) velocity 0-1 (default 0.5), MIDI channel 0-15 (default 0).

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

Changes notes of a MIDI part: each change names a note by id and gives what changes, position and length in ticks, pitch 0-127, velocity 0-1, release velocity 0-1 or MIDI channel 0-15. Returns the changed notes.

Arguments:

- `notes`: array of object, required
- `part`: integer, required

Result:

- `notes`: array, required

### parameter

#### `parameter_get`

Engine command `parameter.get`. Read-only.

One parameter of a channel, named by its target, as parameter.list returns it.

Arguments:

- `channel`: integer, required
- `target`: object, required

Result:

- `parameter`: object, required

#### `parameter_list`

Engine command `parameter.list`. Read-only.

The automatable parameters of a channel: on channels with audio the fader's volume (in dB, up to the project's maxVolume), pan, panLeft and panRight (-1 to 1, see channel.set), and on instrument tracks the parameters of their built-in instrument (see processor.list; plugins come later). Each has its target, as automationLane.add takes it, its name, minimum, maximum, default, unit, whether it is stepped (whole numbers only), options (the names of a stepped parameter's values from the minimum on, if it has any), its static value and text, the value as it is displayed. Automation overrides the static value while it plays. MIDI channels have none: their volume and pan are MIDI controllers (channel.set midiVolume and midiPan).

Arguments:

- `channel`: integer, required

Result:

- `parameters`: array, required

#### `parameter_set`

Engine command `parameter.set`.

Sets the static value of a channel's parameter (see parameter.list), named by its target, from either value or text: the name of one of its options, or a number, optionally followed by its unit, in any case ("-6 dB"). Values beyond the range stop at its ends, and stepped parameters round to whole numbers. Volume and pan changes glide over 5 ms. A channel that gets another instrument starts with its parameters at their defaults. Returns the parameter.

Arguments:

- `channel`: integer, required
- `target`: object, required
- `text`: string
- `value`: number

Result:

- `parameter`: object, required

### pluginhost

#### `pluginhost_check`

Engine command `pluginhost.check`.

Reports the plugins that failed since the last check because their host process crashed, missed a processing deadline (reason "hung") or broke the protocol, and emits plugin.failed for each with its instance, plugin id, process and reason. A failed plugin is bypassed and outputs silence. Also emits pluginhost.changed for each hosting mode change whose moves are done. The engine runs this on its own as soon as a failure is detected or the moves are done.

Result:

- `failures`: array of object, required

#### `pluginhost_get`

Engine command `pluginhost.get`. Read-only.

How plugins are grouped into plugin host processes, and the processes. mode: the hosting mode, "shared" (one process for every plugin), "perVendor", "perPlugin" or "perInstance"; defaultMode: the mode GNUbase starts with; overrides: plugin ids with a mode of their own. hosts: each process with its id (process), its group ("shared", "vendor:<vendor>", "plugin:<id>" or "instance:<instance>"), its state ("running", "hung" or "exited"), its log file's name in the log folder (log) and its plugin instances, whether they failed.

Result:

- `defaultMode`: string, required
- `errors`: array of string
- `hosts`: array of object, required
- `mode`: string, required
- `overrides`: object, required

#### `pluginhost_setMode`

Engine command `pluginhost.setMode`.

Sets the hosting mode (see pluginhost.get) and returns at once. Plugins whose process changes then move to their new process in the background, with their state, between two blocks, so they keep sounding the same. When they did, pluginhost.changed is emitted with the new status, and with the plugins that could not move and stay where they are in errors.

Arguments:

- `mode`: one of "shared" | "perVendor" | "perPlugin" | "perInstance", required

Result:

- `defaultMode`: string, required
- `errors`: array of string
- `hosts`: array of object, required
- `mode`: string, required
- `overrides`: object, required

#### `pluginhost_setOverride`

Engine command `pluginhost.setOverride`.

Gives a plugin (its id, e.g. "clap:com.example.synth") a hosting mode of its own, e.g. "perInstance" to isolate an unstable plugin, or removes its override with null. Moves the plugin's instances like pluginhost.setMode, which emits pluginhost.changed when they moved.

Arguments:

- `mode`: one of "shared" | "perVendor" | "perPlugin" | "perInstance" | null, required
- `plugin`: string, required

Result:

- `defaultMode`: string, required
- `errors`: array of string
- `hosts`: array of object, required
- `mode`: string, required
- `overrides`: object, required

### processor

#### `processor_list`

Engine command `processor.list`. Read-only.

Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units. A stepped parameter takes whole numbers only; its options, if any, name its values from the minimum on.

Result:

- `processors`: array, required

### project

#### `project_autoSave`

Engine command `project.autoSave`.

Saves a copy of the project if it changed since it was last saved or auto saved, as an auto save: into the folder Auto Saves of its project folder as <name>-01.bak, <name>-02.bak and so on up to the user setting project.autoSaveFiles, after which the oldest is overwritten. A project without a file goes to Auto Saves in the user settings' folder, named after the project, and isn't auto saved without one. Media files aren't copied, and the project itself stays unsaved. An auto save opens with project.load. With due, as the engine asks every 30 seconds, only if the user setting project.autoSave is on and project.autoSaveInterval minutes have passed since the project was saved, opened or auto saved. Returns the file, or null if nothing was saved; emits project.autoSaved with the file.

Arguments:

- `due`: boolean

Result:

- `file`: string or null, required

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

#### `project_forgetRecent`

Engine command `project.forgetRecent`.

Removes file from the recent projects (see project.recent), or every project without file. The files stay. Returns the recent projects left.

Arguments:

- `file`: string

Result:

- `projects`: array of object, required

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

Replaces the project with an empty one that has a stereo input and output bus, or the busses of the user's bus presets named Default (see bus.savePreset), or with template, a copy of the project template of that name (see project.templates), and clears the undo history. The new project has no file until it is saved.

Arguments:

- `template`: string

#### `project_recent`

Engine command `project.recent`. Read-only.

The recent projects: the project files saved (not auto saved) or opened last, the newest first, at most 10, kept in the user settings' folder. exists says whether the file is still there.

Result:

- `projects`: array of object, required

#### `project_revert`

Engine command `project.revert`.

Discards every change since the project was last saved: opens its file again and clears the undo history, as project.load does. Fails for a project without a file.

#### `project_revision`

Engine command `project.revision`. Read-only.

A number that grows with every change of the project: every edit, undo and redo, and loading or starting a project. project.load with revision loads only if it hasn't changed. modified says whether the project changed since it was last saved, loaded or started.

Result:

- `modified`: boolean, required
- `revision`: integer, required

#### `project_save`

Engine command `project.save`.

Saves the project as a DAWproject file: as path, which is then the project's file (save as), or without path to the project's file (save). Media files are copied into it unless embedMedia is false; then the file refers to them where they are. The file becomes the first of the recent projects (project.recent).

Arguments:

- `embedMedia`: boolean
- `path`: string

#### `project_saveNewVersion`

Engine command `project.saveNewVersion`.

Saves the project as a new version of its file and goes on with that: in the same folder, with the same name and the next number after the highest one there, "Song-01.dawproject" after "Song.dawproject", "Song-03.dawproject" after "Song-02.dawproject". The earlier versions stay as they are. Returns the new file. Fails for a project without a file: save it first.

Arguments:

- `embedMedia`: boolean. Copy the media files into the file (the default), as project.save does.

Result:

- `file`: string, required

#### `project_saveTemplate`

Engine command `project.saveTemplate`.

Saves the project as the project template name (see project.templates), replacing a template of that name. Templates refer to media files where they are instead of copying them. The project itself, its file and whether it is saved stay as they are. Returns the template's file.

Arguments:

- `name`: string, required

Result:

- `file`: string, required

#### `project_set`

Engine command `project.set`.

Changes the project's name, sample rate, stereo pan law or maximum volume. Linear positions keep their time at a new sample rate. panLaw is how much a mono channel panned to the center is turned down on each side: 0dB, -3dB (the default), -4.5dB, -6dB or equalPower (-3.01 dB, the power stays the same wherever it is panned); the side panned toward is always at 0 dB. maxVolume is the top of the faders, 6 (the default) or 12 dB; faders above a lower maximum come down to it.

Arguments:

- `maxVolume`: number
- `name`: string
- `panLaw`: one of "0dB" | "-3dB" | "-4.5dB" | "-6dB" | "equalPower"
- `sampleRate`: number

#### `project_templates`

Engine command `project.templates`. Read-only.

The project templates, which project.new starts new projects from, sorted by name: each a name and its file in the folder Project Templates in the user settings' folder. None without that folder.

Result:

- `templates`: array of object, required

### quantize

#### `quantize_apply`

Engine command `quantize.apply`.

Quantizes note starts: moves them toward the nearest position of the grid, or of the groove, and keeps their lengths. Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's (see quantize.set), or preset's, changed by settings. Quantizing starts from the notes' original timing unless settings.soft is true, and keeps it, so quantize.reset puts it back. Notes moved by other means since they were quantized start from where they are; velocities and lengths changed by other means stay. Returns the notes that changed. Grids are in project time and start again at every bar; notes don't move before their part's start or to or past its end.

Arguments:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer
- `preset`: string
- `settings`: object

Result:

- `notes`: array, required

#### `quantize_ends`

Engine command `quantize.ends`.

Quantizes note ends: moves them toward the nearest grid position and keeps the starts. Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's (see quantize.set), or preset's, changed by settings. Quantizing starts from the notes' original timing unless settings.soft is true, and keeps it, so quantize.reset puts it back. Notes moved by other means since they were quantized start from where they are; velocities and lengths changed by other means stay. Returns the notes that changed.

Arguments:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer
- `preset`: string
- `settings`: object

Result:

- `notes`: array, required

#### `quantize_extractGroove`

Engine command `quantize.extractGroove`.

Makes a groove preset from a MIDI part's rhythm: a groove position at every note start, with the highest velocity and the longest length of the notes there. The groove is length ticks long, by default the part's length, and the preset is named name, by default after the part. The groove's origin is the part's start, so it repeats in phase with the part.

Arguments:

- `length`: integer
- `name`: string
- `part`: integer, required

Result:

- `preset`: object, required

#### `quantize_freeze`

Engine command `quantize.freeze`.

Makes quantized notes' timing permanent: quantizing starts from it from now on, and quantize.reset no longer goes back. Takes the notes of part (only notes, if given) or of every part in parts and returns the notes that changed.

Arguments:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer

Result:

- `notes`: array, required

#### `quantize_get`

Engine command `quantize.get`. Read-only.

The project's quantize settings and whether auto quantize is on (see quantize.set for the settings).

Result:

- `autoQuantize`: boolean, required
- `settings`: object, required

#### `quantize_lengths`

Engine command `quantize.lengths`.

Sets note lengths and keeps the starts: to length, a note value ("1/8", "1/8T", "1/8.") or ticks, or without length to the step of the grid at each note's start, so tuplets and swing count and notes outside the catch range keep their length. Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's (see quantize.set), or preset's, changed by settings. quantize.reset puts the lengths back. Returns the notes that changed.

Arguments:

- `length`: string or integer
- `notes`: array of integer
- `part`: integer
- `parts`: array of integer
- `preset`: string
- `settings`: object

Result:

- `notes`: array, required

#### `quantize_presets`

Engine command `quantize.presets`. Read-only.

The quantize presets: the factory presets (straight grids "1/1" to "1/128", triplets such as "1/8 Triplet" and dotted grids such as "1/4 Dotted") and the user's, each with its settings. The user's presets are kept in the settings folder, so they come back when GNUbase starts again: file is where (null if they are kept in memory only), readOnly whether changes are saved (not while the file belongs to a newer GNUbase), and problems what was wrong with it, such as presets that this GNUbase can't read and skips; they stay in the file.

Result:

- `file`: string or null, required
- `presets`: array, required
- `problems`: array, required
- `readOnly`: boolean, required

#### `quantize_removePreset`

Engine command `quantize.removePreset`.

Removes one of the user's quantize presets, and the presets of that name that this GNUbase can't read (see quantize.presets).

Arguments:

- `name`: string, required

#### `quantize_renamePreset`

Engine command `quantize.renamePreset`.

Renames one of the user's quantize presets.

Arguments:

- `name`: string, required
- `newName`: string, required

#### `quantize_reset`

Engine command `quantize.reset`.

Puts quantized notes back to their timing before quantizing: starts, lengths and velocities. Notes moved by other means since keep their timing, and velocities and lengths changed by other means stay. Takes the notes of part (only notes, if given) or of every part in parts and returns the notes that changed.

Arguments:

- `notes`: array of integer
- `part`: integer
- `parts`: array of integer

Result:

- `notes`: array, required

#### `quantize_savePreset`

Engine command `quantize.savePreset`.

Saves the project's quantize settings, changed by settings, as a preset, replacing the user's preset of that name. Factory presets can't be replaced. Presets belong to the user, not to the project: they are saved in the settings folder.

Arguments:

- `name`: string, required
- `settings`: object

Result:

- `preset`: object, required

#### `quantize_set`

Engine command `quantize.set`.

Changes the project's quantize settings: to preset's, if given, then changed by settings. Settings: grid, a note value from "1/1" to "1/128", "T" for a triplet and "." for dotted; tuplet, 0 (off) or 3 to 15 notes in the time of the largest power of two below it; swing 0-1, moving every second grid position later, at 1 by a third of a step (straight grids without tuplets only); catchRange, ticks from a grid position within which notes are quantized (0: all); safeRange, ticks within which they stay; randomize, ticks by which quantized notes move at random, by seed; soft, starting from the current positions instead of the original ones; strength 0-1, the part of the way notes move. groove {length, points: [{position, velocity 0-1, length}]} quantizes to a groove instead of the grid, with preQuantize (a note value), maxMove (ticks), velocityStrength, lengthStrength and origin (where it starts repeating, in project ticks). autoQuantize is whether recording quantizes. Settings are not undo steps; they are saved with the project.

Arguments:

- `autoQuantize`: boolean
- `preset`: string
- `settings`: object

Result:

- `autoQuantize`: boolean, required
- `settings`: object, required

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

- `key`: one of "audio.autoMonitoring" | "audio.backend" | "audio.blockSize" | "audio.busPresets" | "audio.device" | "audio.fallbackDevice" | "audio.sampleRate" | "midi.chaseBeyondParts" | "midi.chaseControllers" | "midi.chaseNotes" | "midi.chasePitchBend" | "midi.chasePrograms" | "project.autoSave" | "project.autoSaveFiles" | "project.autoSaveInterval" | "ui.layout" | "ui.scale" | "ui.theme" | "ui.zoomPresets", required. The setting.

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

- `key`: one of "audio.autoMonitoring" | "audio.backend" | "audio.blockSize" | "audio.busPresets" | "audio.device" | "audio.fallbackDevice" | "audio.sampleRate" | "midi.chaseBeyondParts" | "midi.chaseControllers" | "midi.chaseNotes" | "midi.chasePitchBend" | "midi.chasePrograms" | "project.autoSave" | "project.autoSaveFiles" | "project.autoSaveInterval" | "ui.layout" | "ui.scale" | "ui.theme" | "ui.zoomPresets". The setting.

Result:

- `values`: object, required

#### `settings_set`

Engine command `settings.set`.

Changes a user setting and saves the settings file, unless settings.list reports it read-only. User settings belong to the user, not to a project. Emits settings.changed with the changed value, unless it had that value already. The settings: `audio.autoMonitoring` (one of ["manual","whileRecordEnabled","whileRecording","tapeMachine"], default "tapeMachine"): When record-enabled audio tracks monitor their input, besides the monitor button: manual never, whileRecordEnabled always, whileRecording while the transport records, tapeMachine while it stands or records; `audio.backend` (string, default ""): The audio backend, such as wasapi or coreaudio, or empty for the preferred one; `audio.blockSize` (integer, 16 to 8192, default 256): Frames per audio block; `audio.busPresets` (object, default {"input":{},"output":{}}): The user's own bus presets, per direction (input, output) by name: each a list of busses with their name, format, ports and child busses (bus.savePreset); `audio.device` (string, default ""): The audio device's id, or empty for the backend's default device; `audio.fallbackDevice` (one of ["none","default","dummy"], default "none"): The device that plays while the chosen audio device is lost, until it returns: none, default (the lost device's backend's default device, or the dummy device if that doesn't open), or dummy (the dummy device: no sound, but the transport moves and the project plays on). The engine switches back once the lost device returns; `audio.sampleRate` (number, 8000 to 768000, default 48000.0): The audio device's sample rate in Hz; `midi.chaseBeyondParts` (boolean, default true): Chases controller events from the whole track; off, only from the MIDI parts under the position, so none outside parts; `midi.chaseControllers` (boolean, default true): Where playback starts or jumps, or the cursor moves while stopped, MIDI tracks send each control change's last value before that position; `midi.chaseNotes` (boolean, default true): Where playback starts or jumps, MIDI tracks start the notes that sound at that position; `midi.chasePitchBend` (boolean, default true): Where playback starts or jumps, or the cursor moves while stopped, MIDI tracks send each channel's last pitch bend before that position; `midi.chasePrograms` (boolean, default true): Where playback starts or jumps, or the cursor moves while stopped, MIDI tracks send each channel's last program change before that position; `project.autoSave` (boolean, default true): Saves a copy of a project with unsaved changes every project.autoSaveInterval minutes into the folder Auto Saves of its project folder (see project.autoSave); `project.autoSaveFiles` (integer, 1 to 100, default 5): How many auto saves a project keeps; the next one overwrites the oldest; `project.autoSaveInterval` (integer, 1 to 120, default 15): Minutes between auto saves; `ui.layout` (object, default {}): The layout of the windows with zones: under windows, by window (project), its zones (left, right, lower) with whether each is visible, its size in dp, the order of its tabs, the tabs hidden and the active one, and its strips (toolbar, infoLine, overviewLine, statusLine, transportBar) with whether each is shown, and its sets of sections (such as a mixer channel's) with the order of their sections and those hidden; under panels, by panel, whether it is in a window of its own and that window's bounds. What it leaves out has its default; `ui.scale` (number, 0.5 to 4, default 1.0): The UI scale, applied on top of each monitor's own scale; `ui.theme` (string, default "gnubase"): The theme's folder name in resources/themes; `ui.zoomPresets` (array, default []): The horizontal zoom presets of the event display, for every project: each a name and the seconds the view shows.

Arguments:

- `key`: one of "audio.autoMonitoring" | "audio.backend" | "audio.blockSize" | "audio.busPresets" | "audio.device" | "audio.fallbackDevice" | "audio.sampleRate" | "midi.chaseBeyondParts" | "midi.chaseControllers" | "midi.chaseNotes" | "midi.chasePitchBend" | "midi.chasePrograms" | "project.autoSave" | "project.autoSaveFiles" | "project.autoSaveInterval" | "ui.layout" | "ui.scale" | "ui.theme" | "ui.zoomPresets", required. The setting.
- `value`: any, required. The new value; it must fit the setting's schema.

Result:

- `key`: string, required
- `value`: any, required. The setting's value.

### snap

#### `snap_get`

Engine command `snap.get`. Read-only.

The project's snap settings. on: whether snap is on. type: grid (the nearest grid line), gridRelative (moves in grid steps, keeping the distance to the grid), events (the starts and ends of other events, audio events' snap points and markers, when near), shuffle (events trade places with their neighbours, see event.shuffle), cursor (the project cursor, when near), or the combinations gridCursor, eventsCursor and eventsGridCursor. grid: bar, beat, quantize (the project's quantize grid) or adaptToZoom (a note value that the view chooses); 1ms, 10ms, 100ms, 1000ms; subframe, quarterFrame, halfFrame, frame, twoFrames, second; 1sample, 10samples, 100samples, 44100samples. zeroCrossing: splitting and resizing audio events in the project window cut where the audio crosses zero.

Result:

- `grid`: one of "bar" | "beat" | "quantize" | "adaptToZoom" | "1ms" | "10ms" | "100ms" | "1000ms" | "subframe" | "quarterFrame" | "halfFrame" | "frame" | "twoFrames" | "second" | "1sample" | "10samples" | "100samples" | "44100samples", required
- `on`: boolean, required
- `type`: one of "grid" | "gridRelative" | "events" | "shuffle" | "cursor" | "gridCursor" | "eventsCursor" | "eventsGridCursor", required
- `zeroCrossing`: boolean, required

#### `snap_position`

Engine command `snap.position`. Read-only.

Snaps a position (project samples) as editing in the project window does, with the project's snap settings, changed by settings: where a point that would go to position goes. from is where it was, for gridRelative; events are the events being edited, whose edges and snap points don't attract; range is how near (project samples, default a tenth of a second) events, markers and the cursor attract; zoomStep is adaptToZoom's note value in ticks (default: bars); framesPerSecond the frame grids' rate (default 25). Returns position in project samples, and ticks, which are exact when musical, that is when a bar, beat, quantize or zoom grid line put it there.

Arguments:

- `events`: array of integer
- `framesPerSecond`: integer
- `from`: integer
- `position`: integer, required
- `range`: integer
- `settings`: object
- `zoomStep`: integer

Result:

- `musical`: boolean, required
- `position`: integer, required
- `ticks`: integer, required

#### `snap_set`

Engine command `snap.set`.

Changes the project's snap settings; what isn't given stays. on: whether snap is on. type: grid (the nearest grid line), gridRelative (moves in grid steps, keeping the distance to the grid), events (the starts and ends of other events, audio events' snap points and markers, when near), shuffle (events trade places with their neighbours, see event.shuffle), cursor (the project cursor, when near), or the combinations gridCursor, eventsCursor and eventsGridCursor. grid: bar, beat, quantize (the project's quantize grid) or adaptToZoom (a note value that the view chooses); 1ms, 10ms, 100ms, 1000ms; subframe, quarterFrame, halfFrame, frame, twoFrames, second; 1sample, 10samples, 100samples, 44100samples. zeroCrossing: splitting and resizing audio events in the project window cut where the audio crosses zero. Settings are not undo steps; they are saved with the project. Emits snap.changed.

Arguments:

- `grid`: one of "bar" | "beat" | "quantize" | "adaptToZoom" | "1ms" | "10ms" | "100ms" | "1000ms" | "subframe" | "quarterFrame" | "halfFrame" | "frame" | "twoFrames" | "second" | "1sample" | "10samples" | "100samples" | "44100samples"
- `on`: boolean
- `type`: one of "grid" | "gridRelative" | "events" | "shuffle" | "cursor" | "gridCursor" | "eventsCursor" | "eventsGridCursor"
- `zeroCrossing`: boolean

Result:

- `grid`: one of "bar" | "beat" | "quantize" | "adaptToZoom" | "1ms" | "10ms" | "100ms" | "1000ms" | "subframe" | "quarterFrame" | "halfFrame" | "frame" | "twoFrames" | "second" | "1sample" | "10samples" | "100samples" | "44100samples", required
- `on`: boolean, required
- `type`: one of "grid" | "gridRelative" | "events" | "shuffle" | "cursor" | "gridCursor" | "eventsCursor" | "eventsGridCursor", required
- `zeroCrossing`: boolean, required

### track

#### `track_add`

Engine command `track.add`.

Adds a track and its channel. Tracks go into a folder with parent, at index among its tracks (default: last). Audio channels default to stereo; format or channels set another format, as in bus.add. Folders get a channel only with groupChannel. New channels play through the main output, or through the folder's group channel, and audio tracks record from the first input bus; input, output and instrument set them as channel.set does. Marker tracks have no channel and stay at the top level; a project has at most 10, and a new one becomes the active marker track. Ruler tracks have no channel either; timeFormat is the time format their ruler shows (default barsBeats).

Arguments:

- `channels`: integer
- `color`: string or null
- `format`: one of "mono" | "stereo" | "LRC" | "LRS" | "LRC+LFE" | "LRS+LFE" | "LRCS" | "LRCS+LFE" | "quadro" | "quadro+LFE" | "5.0" | "5.1" | "6.0 cine" | "6.0 music" | "7.0" | "7.1"
- `groupChannel`: boolean
- `index`: integer
- `input`: integer
- `instrument`: string
- `name`: string
- `output`: integer
- `parent`: integer
- `timeBase`: one of "musical" | "linear"
- `timeFormat`: one of "barsBeats" | "seconds" | "timecode" | "samples" | "userFrames"
- `type`: one of "audio" | "instrument" | "midi" | "folder" | "group" | "effect" | "marker" | "ruler", required

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

Removes a track with its channel, events, parts or markers; a folder also removes the tracks inside it. Without the active marker track, the topmost marker track becomes the active one.

Arguments:

- `track`: integer, required

#### `track_set`

Engine command `track.set`.

Changes a track's name, color (#rrggbb, or null for the default), time base or, for ruler tracks, time format. A new time base keeps its events, parts and markers at the same time.

Arguments:

- `color`: string or null
- `name`: string
- `timeBase`: one of "musical" | "linear"
- `timeFormat`: one of "barsBeats" | "seconds" | "timecode" | "samples" | "userFrames"
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
- `recording`: boolean, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_get`

Engine command `transport.get`. Read-only.

Whether playback and recording run, the project cursor in samples, the locators and the modes. After a locate the cursor is where it goes, even before the audio thread has applied it.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `recording`: boolean, required
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
- `recording`: boolean, required
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
- `recording`: boolean, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_record`

Engine command `transport.record`.

Starts recording (record true, the default): playback starts as with transport.play if it stood, or recording punches in where it plays. record false punches out and playback goes on. Record-enabled tracks may monitor their input meanwhile, by the user setting audio.autoMonitoring (see channel.set).

Arguments:

- `record`: boolean

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `recording`: boolean, required
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
- `recording`: boolean, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

#### `transport_stop`

Engine command `transport.stop`.

Stops playback and recording. With returnToStart the cursor goes back to where playback started or was last located to.

Result:

- `cycle`: boolean, required
- `left`: integer, required
- `playing`: boolean, required
- `position`: integer, required
- `recording`: boolean, required
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
- `recording`: boolean, required
- `returnToStart`: boolean, required
- `right`: integer, required
- `startMode`: one of "cursor" | "cycleStart", required

### ui

#### `ui_click`

Clicks the centre of the element that the CSS selector matches, or a point. The element must be visible. Returns the element under the mouse, the one that got the click unless another covers it.

Arguments:

- `button`: one of "left" | "right" | "middle". Left if left out.
- `count`: integer. 2 for a double click; 1 if left out.
- `index`: integer. Which of the elements the selector matches; 0, the first, if left out.
- `modifiers`: array of one of "ctrl" | "shift" | "alt" | "meta". Keys held while clicking.
- `selector`: string. A CSS selector; the mouse goes to the centre of the element it matches.
- `window`: integer. The window's id from ui_windows; the main window if left out.
- `x`: integer. Instead of a selector: the point in the window's pixels, with y.
- `y`: integer. The point's y, with x.

Result:

- `element`: object or null, required

#### `ui_dom`

Read-only.

Reads a window's DOM (RmlUi documents): its documents, or with selector the elements that the CSS selector matches (like querySelectorAll), each as a tree of elements with tag, id, classes, other attributes, a form control's value, pseudo-classes such as hover, focus or checked, whether it's visible, its border box in the window's pixels and its children, depth levels deep. Text is a child {"text": ...}; an element whose children are left out has its text instead. Use depth 0 to get just the elements.

Arguments:

- `depth`: integer. Levels of children; all if left out.
- `selector`: string. A CSS selector.
- `window`: integer. The window's id from ui_windows; the main window if left out.

Result:

- `elements`: array, required

#### `ui_drag`

Drags with the left button from one element or point to another and drops there, e.g. a fader's cap, finer with shift held. Returns the element under the mouse at the end.

Arguments:

- `from`: object, required. Where to press the button: selector and index, or x and y.
- `modifiers`: array of one of "ctrl" | "shift" | "alt" | "meta". Keys held while dragging.
- `to`: object, required. Where to release it, the same way.
- `window`: integer. The window's id from ui_windows; the main window if left out.

Result:

- `element`: object or null, required

#### `ui_file_dialog`

Lists the file dialogs that wait for an answer, e.g. after File > Open: each with its id, kind (open, save or folder), title, filters and the location it starts in. With dialog, first answers that one as the user would: with files, the files chosen (the file to save as, the folder); with none, a cancel. A dialog the OS shows stays open until it is closed, and its answer is then ignored. Returns the dialogs still waiting.

Arguments:

- `dialog`: integer. The id of the dialog to answer.
- `files`: array of string. With dialog: the files chosen; none to cancel.

Result:

- `dialogs`: array, required

#### `ui_key`

Presses and releases a key with modifiers, written like "Enter", "Ctrl+Shift+S" or "Mod+Z": letters, digits, F1 to F24, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Up, Down, Left, Right, Space, . , - +, and on the keypad Num0 to Num9, NumEnter, Num* Num/ Num+ Num- Num.; modifiers Mod (the platform's command key: Cmd on macOS, Ctrl elsewhere), Ctrl, Shift, Alt and Meta (Cmd). A key that the focused element doesn't use runs its key command, e.g. Space starts or stops playback. Keys don't type text: use ui_text for that. Returns the element with the keyboard focus.

Arguments:

- `keys`: string, required
- `window`: integer. The window's id from ui_windows; the main window if left out.

Result:

- `focus`: object or null, required

#### `ui_move`

Moves the mouse to the centre of an element or to a point, e.g. to hover over it. Returns the element now under the mouse.

Arguments:

- `index`: integer. Which of the elements the selector matches; 0, the first, if left out.
- `selector`: string. A CSS selector; the mouse goes to the centre of the element it matches.
- `window`: integer. The window's id from ui_windows; the main window if left out.
- `x`: integer. Instead of a selector: the point in the window's pixels, with y.
- `y`: integer. The point's y, with x.

Result:

- `element`: object or null, required

#### `ui_screenshot`

Read-only.

Takes a screenshot of a window, or with selector of the element it matches (its border box), as a PNG image in the window's pixels.

Arguments:

- `index`: integer
- `selector`: string. A CSS selector.
- `window`: integer. The window's id from ui_windows; the main window if left out.

#### `ui_text`

Types text into the element with the keyboard focus, e.g. a text field that ui_click focused. Returns that element.

Arguments:

- `text`: string, required
- `window`: integer. The window's id from ui_windows; the main window if left out.

Result:

- `focus`: object or null, required

#### `ui_wait`

Read-only.

Waits until an element that the CSS selector matches is visible, and has the text if one is given; with visible false, until none is. Returns the elements the selector matches then (with the text), like ui_dom with depth 0, or an error after the seconds have passed.

Arguments:

- `seconds`: number. How long to wait at most; 5 if left out.
- `selector`: string, required. A CSS selector.
- `text`: string. A part of the element's text.
- `visible`: boolean. False to wait until no element is visible; true if left out.
- `window`: integer. The window's id from ui_windows; the main window if left out.

Result:

- `elements`: array, required

#### `ui_wheel`

Turns the mouse wheel over an element or a point, or where the mouse is, e.g. to scroll a list or turn a knob.

Arguments:

- `dx`: number. Lines to the right; 0 if left out.
- `dy`: number. Lines down; 0 if left out.
- `index`: integer. Which of the elements the selector matches; 0, the first, if left out.
- `modifiers`: array of one of "ctrl" | "shift" | "alt" | "meta". Keys held while turning the wheel.
- `selector`: string. A CSS selector; the mouse goes to the centre of the element it matches.
- `window`: integer. The window's id from ui_windows; the main window if left out.
- `x`: integer. Instead of a selector: the point in the window's pixels, with y.
- `y`: integer. The point's y, with x.

Result:

- `element`: object or null, required

#### `ui_windows`

Read-only.

Lists GNUbase's open windows: each window's id, whether it's the main window, its size in pixels, its dp ratio (pixels per density-independent pixel: its monitor's scale times the UI scale) and its documents (title, source file, whether visible and modal). Coordinates in the other ui_ tools are in the window's pixels, as in its screenshots.

Result:

- `windows`: array, required

### Resources

Queries without required arguments can also be read as resources:

- `gnubase://audio.analyze`: Measures the captured output of channels (default: every output) from frame start for frames frames (default: to the end of the capture). Per channel: the peak and RMS level, and a spectrum summary with the strongest frequencies (at most peaks, default 5, up to 20) and the power of octave bands. For the channels together: the integrated loudness in LUFS (ITU-R BS.1770), null below 400 ms. Levels are linear and in dBFS (a full-scale sine's peak is 0 dBFS, its RMS -3 dBFS); dBFS is null for silence.
- `gnubase://audio.devices`: The audio backends of this build, the preferred one first, and the devices each has now: their ids, names, channel names, sample rates, block sizes, clock sources (and the current one), whether the driver has a control panel, whether it can monitor inputs directly (canMonitorDirectly), and whether the device is available. An unavailable device, e.g. one whose hardware isn't connected, has an unavailableReason; audio.setDevice still tries it. Some backends only learn that by opening or starting the device (ASIO). A backend that can't list its devices has an error instead.
- `gnubase://audio.get`: The audio device (its backend, id, name, format, channel names, latencies in frames, clock source, whether it has a control panel, whether it can monitor directly and does (directMonitoring), the record shift and the record offset: inputLatency + outputLatency - recordShift, from 0 to 196608), whether it runs, how many frames it has processed and, if it is the dummy device, captured, how many frames of audio track playback were silent because a disk stream was late, and the processing threads (see audio.setProcessing). projectSampleRate is the project's sample rate; with sampleRateMismatch the device runs at another one, as it doesn't support the project's or was asked for another, and plays faster or slower. lost: the device that went away (its backend, device id, the reason it can't be opened and whether it ran and restarts), while the engine waits for it to return (see audio.reset); otherwise null. While a device is lost, device is the fallback that plays meanwhile (the user setting audio.fallbackDevice), or null.
- `gnubase://audio.performance`: Performance data for the performance monitor. Loads are the share of a block's duration spent processing it, so above 1 the device drops out. load: averaged over about 0.3 s; peak: the highest block; overloads: blocks above 1 plus deviceOverloads, the dropouts the driver reported, also those it recovers from with a reset (a block that was both counts twice; a reset after a settings change counts nothing); cycles: blocks measured; threads: the load of each processing thread, the audio thread first. Peak, overloads and cycles count since the last audio.resetPerformance, also across audio.reset; everything starts over with a new device or new processing threads. diskLoad: how far the disk stream furthest behind has run empty, 0 to 1; diskDropouts: frames of audio track playback that were silent because a disk stream was late. blockSize: the device's block size in frames, 0 without a device.
- `gnubase://audio.ports`: The audio device's ports by name, in order, each with the name it is shown as and whether it is visible (audio.setPort), and the busses that use it (the bus ids); each bus's ports on this device, null where it has none (a child bus's are its parent's); and the missing ports: port names that busses use but the device doesn't have, with those busses. bus.remapPorts re-routes them. beyondDevice: busses without ports of their own (consecutive ports in bus order) whose channels run past the device's ports, with those channels, which are silent; bus.set gives them ports. Without a device there are no ports.
- `gnubase://automation.get`: The automation settings. followsEvents (default false): when an audio event or MIDI part moves, the automation of its track in its range moves with it and replaces the points where it lands.
- `gnubase://automationLane.list`: The automation lanes of a track, or of every track in track order, with their points sorted by position.
- `gnubase://engine.commandLog`: The last 256 commands that ran, oldest first, without queries: what the UI and other clients did. Each with its request id (events name it too), the client that sent it (ui, mcp, lua, app for the command line, engine for the engine's own commands), the command, its parameters, and the error if it failed. Commands the engine ran on its own that changed nothing, such as tries to open a lost audio device again, are left out.
- `gnubase://engine.commands`: Every command with its documentation, parameters and result.
- `gnubase://engine.version`: The GNUbase version.
- `gnubase://graph.get`: The project's audio processing graph: its nodes (busses, then channels in track order) with the nodes they receive audio and events from, and the order they are processed in.
- `gnubase://history.get`: The undo history: every step with its action, time (UTC) and details, the position (steps before it are done, the ones after it can be redone) and the maximum number of steps (0: no limit).
- `gnubase://job.list`: Every background job since the engine started, such as backups: id, kind, state (running, done, failed or cancelled), progress from 0 to 1, what it is doing, and its result or error once it ended.
- `gnubase://marker.list`: The markers of a marker track (default: the active one; track 0 if there is none) in timeline order, with their position and end also in the project's samples.
- `gnubase://media.peakStatus`: The state of the waveform overviews of a media file, or of every media file: pending, generating (with progress from 0 to 1), ready, failed (with the error) or recording; the frames that have peaks; the peak file they were loaded from or written to; whether they were generated from the audio file rather than loaded. Peak files are kept next to their audio file as <file name>.gbpeaks, or in fallbackFolder where that can't be written.
- `gnubase://midi.capture`: What instruments played since the last call, at most 4096 MIDI 1.0 messages, oldest first, each with its instrument track's channel, its frame position on the audio device and its bytes: the events of the track's parts, of the MIDI tracks routed to it and what midi.inject played into them, with controller automation and the note-offs of a stop. A program change with a bank comes as bank select MSB and LSB, then the program change; SysEx isn't captured. dropped counts the messages that didn't fit: while nobody reads, newer messages are dropped. start: true starts a fresh capture; it drops what was captured so far and returns nothing. Each call empties what it returns, so a repeated call doesn't return the same messages again.
- `gnubase://midi.monitor`: The MIDI input received since the last call from every open input, at most 4096 messages, each with its port, its frame position on the audio device and its bytes. dropped counts what didn't fit: while nobody reads, newer messages are dropped. start: true starts afresh; it drops what was received so far and returns nothing. Each call empties what it returns. Input reaches the monitor while an audio device processes blocks, which places it in time.
- `gnubase://midi.ports`: The MIDI ports of every MIDI backend, inputs and outputs: backend, id, system name, the name they are shown as (showAs), whether they are visible, whether an input belongs to all MIDI inputs, whether the port is present now and open, and whether the system speaks MIDI 2.0 with it. Ports that were unplugged stay listed with their settings, not present.
- `gnubase://pluginhost.get`: How plugins are grouped into plugin host processes, and the processes. mode: the hosting mode, "shared" (one process for every plugin), "perVendor", "perPlugin" or "perInstance"; defaultMode: the mode GNUbase starts with; overrides: plugin ids with a mode of their own. hosts: each process with its id (process), its group ("shared", "vendor:<vendor>", "plugin:<id>" or "instance:<instance>"), its state ("running", "hung" or "exited"), its log file's name in the log folder (log) and its plugin instances, whether they failed.
- `gnubase://processor.list`: Every built-in processor: its id, name, description, whether it is an instrument, and its parameters with their ids, ranges, defaults and units. A stepped parameter takes whole numbers only; its options, if any, name its values from the minimum on.
- `gnubase://project.get`: The whole project: busses, tracks in order, channels, media, clips, events and parts.
- `gnubase://project.location`: Where the project is: its project file (set by project.save and project.load), the folder of that file, and the media folder in it where media.add with copy and media.prepareArchive put audio files. All null for a project that has no file yet.
- `gnubase://project.recent`: The recent projects: the project files saved (not auto saved) or opened last, the newest first, at most 10, kept in the user settings' folder. exists says whether the file is still there.
- `gnubase://project.revision`: A number that grows with every change of the project: every edit, undo and redo, and loading or starting a project. project.load with revision loads only if it hasn't changed. modified says whether the project changed since it was last saved, loaded or started.
- `gnubase://project.templates`: The project templates, which project.new starts new projects from, sorted by name: each a name and its file in the folder Project Templates in the user settings' folder. None without that folder.
- `gnubase://quantize.get`: The project's quantize settings and whether auto quantize is on (see quantize.set for the settings).
- `gnubase://quantize.presets`: The quantize presets: the factory presets (straight grids "1/1" to "1/128", triplets such as "1/8 Triplet" and dotted grids such as "1/4 Dotted") and the user's, each with its settings. The user's presets are kept in the settings folder, so they come back when GNUbase starts again: file is where (null if they are kept in memory only), readOnly whether changes are saved (not while the file belongs to a newer GNUbase), and problems what was wrong with it, such as presets that this GNUbase can't read and skips; they stay in the file.
- `gnubase://settings.list`: Every user setting with its key, description, JSON schema, default and value; the settings file, null when settings are kept in memory only; its version; and what happened when it was loaded: the status (inMemory, missing, loaded, migrated, reset or newer), the version the file had, the problems found, where the original file was kept, and whether the file is read-only for this session, so that changes apply but aren't saved (a newer GNUbase's file, or one that couldn't be backed up or locked).
- `gnubase://snap.get`: The project's snap settings. on: whether snap is on. type: grid (the nearest grid line), gridRelative (moves in grid steps, keeping the distance to the grid), events (the starts and ends of other events, audio events' snap points and markers, when near), shuffle (events trade places with their neighbours, see event.shuffle), cursor (the project cursor, when near), or the combinations gridCursor, eventsCursor and eventsGridCursor. grid: bar, beat, quantize (the project's quantize grid) or adaptToZoom (a note value that the view chooses); 1ms, 10ms, 100ms, 1000ms; subframe, quarterFrame, halfFrame, frame, twoFrames, second; 1sample, 10samples, 100samples, 44100samples. zeroCrossing: splitting and resizing audio events in the project window cut where the audio crosses zero.
- `gnubase://transport.get`: Whether playback and recording run, the project cursor in samples, the locators and the modes. After a locate the cursor is where it goes, even before the audio thread has applied it.

<!-- End of the generated tool catalogue -->
