# MCP server

GNUbase has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server, so AI agents such as Claude Code can use everything the engine can do: create projects, add tracks and notes, play, render and measure audio, and run Lua.

- **Tools come from the engine's command registry.** Every engine command is a tool, named with `_` instead of `.` (`track.add` is `track_add`), with the command's parameter schema as its input schema and its result schema as its output schema. A command that only reads is marked read-only (`readOnlyHint`). The [tool catalogue](#tool-catalogue) at the end lists them all; the registry itself is `engine_commands`.
- **`script_run`** runs Lua code with the same API (the Lua API reference, [api-reference.md](api-reference.md)) in a sandbox, for work that takes many steps or for test cases written with `gnubase.test`.
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
- `ui_click`, `ui_move`, `ui_drag` and `ui_wheel` use the mouse on an element, by `selector` (and `index` among its matches), or on a point `x`, `y`. `ui_key` presses keys such as `Enter` or `Ctrl+S`; `ui_text` types text into the focused field. `ui_drop` drops files on an element or a point, as if dragged there from the OS's file manager, e.g. audio files on the event display to import them. Each answers with the element it reached, so the agent sees what it hit.
- `ui_wait` waits until an element is visible, or has a text, or until none is, e.g. for a dialog that a click opens.
- `ui_close` asks to close a `window` as its title bar's close button does, or with `quit: true` asks GNUbase to quit as the OS does; quitting asks about unsaved changes first, so `ui_wait` for `#discard-changes-dialog` afterwards.

Boxes and points are in the window's pixels, as in its screenshots; on a monitor with a scale of 200 %, 1 dp is 2 pixels. An agent opening a dialog and reading it back:

```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"ui_click","arguments":{"selector":"#rename"}}}
{"id":5,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"element":{"attributes":{"title":"Rename the track"},"box":{"height":30,"width":120,"x":10,"y":10},"id":"rename","pseudoClasses":["hover"],"tag":"button","text":"Rename track","visible":true}}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"ui_wait","arguments":{"selector":"#rename-dialog"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"ui_dom","arguments":{"selector":"#rename-dialog"}}}
{"id":7,"jsonrpc":"2.0","result":{"content":[…],"isError":false,"structuredContent":{"elements":[{"box":{…},"children":[{"box":{…},"children":[{"text":"Enter a new name for Audio 01."}],"id":"message","tag":"p","visible":true},{"attributes":{"type":"text"},"box":{…},"id":"name","tag":"input","value":"Audio 01","visible":true},…],"id":"rename-dialog","tag":"body","visible":true}]}}}
```

## Events

Every session has a queue of the engine events since it started: what changed in the project, the transport, the audio device and the undo history, with the same names and data as in Lua (the Events section of the Lua API reference, [api-reference.md](api-reference.md)). The events come from every client of the engine: this MCP client, other MCP clients, Lua scripts (those run with `script_run` too) and the UI. Over stdio, the whole input is one session; over HTTP, `initialize` starts a session for each client.

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
| The catalogue misses a tool | It's generated from the server by the unit tests ([api-reference.md](api-reference.md)); regenerate it from your build. |
| The `ui_` tools are missing | GNUbase runs headless, which has no UI. Start it without `--headless`, with `--mcp-http`. |
| A `ui_` tool says the UI didn't answer | The UI thread was busy for 10 seconds, e.g. loading a large project, or GNUbase is quitting. Try again. |
| `ui_click` reports another element than the one asked for | Something covers it, such as a modal dialog; the click went to what is on top. `ui_screenshot` shows what the window looks like. |

## Tool catalogue

Every tool by area, its engine command, whether it only reads, its arguments and its result are in the generated MCP tool catalogue, `mcp-tools.md` ([API references](api-reference.md)). A running server lists the same with `tools/list`.
