# GNUbase architecture

How GNUbase is put together: processes, modules, threads, the engine API and the rules that keep audio real-time safe. Read this before changing module boundaries, threads or the engine API. [CLAUDE.md](../CLAUDE.md) has the short version of the rules; this document has the details and the reasoning.

Parts marked **decided in #N** are open until that ticket lands; the ticket refines the section.

## Principles

1. The engine is a headless library. The UI, the MCP server, Lua scripts and tests are all clients of the same engine API.
2. Everything a user can do is a command; everything that changes is reported as an event.
3. Third-party plugins never run in the GNUbase process.
4. Audio threads never wait: no allocations, locks, blocking calls or I/O.
5. Output is deterministic: the same project and input give the same samples for any number of threads.
6. Projects are DAWproject files.

## Processes

```mermaid
flowchart LR
  subgraph app["gnubase (app process)"]
    UI["UI (SDL3 + RmlUi)"]
    Engine["Engine"]
    MCP["MCP server"]
    Lua["Lua runtime"]
  end
  Host1["gnubase-pluginhost<br>(native)"]
  Host2["gnubase-pluginhost<br>(32-bit or emulated)"]
  Scan["gnubase-pluginhost --scan"]
  Device["Audio and MIDI devices"]
  Agent["AI agent (MCP client)"]
  UI <-->|"commands and events"| Engine
  MCP <-->|"commands and events"| Engine
  Lua <-->|"commands and events"| Engine
  Engine <-->|"shared-memory IPC"| Host1
  Engine <-->|"shared-memory IPC"| Host2
  Engine -.->|"starts, reads results"| Scan
  Engine <--> Device
  Agent -->|"localhost HTTP or stdio"| MCP
```

- **gnubase** is the application: the engine, the UI (left out with `--headless`), the MCP server and the Lua runtime.
- **gnubase-pluginhost** runs third-party plugins. How plugins are grouped into host processes is a user setting: all in one, per vendor, per plugin or per instance. There is no in-process mode (#64). Variants for 32-bit plugins (Windows, Linux) and for x86-64 plugins on Apple Silicon (Rosetta) and Windows ARM64 (emulation) speak the same protocol (#151, #152, #153).
- **Plugin scanning** uses the host binary in scan mode: one short-lived process per plugin file, with a timeout. A crash only blocklists that plugin (#66).
- When a host process crashes or misses its deadline, its plugins output silence, the UI is told, and the plugins can be reloaded with their last state (#65). GNUbase keeps running.

## Modules

```mermaid
flowchart TD
  app --> ui & mcp & scripting & engine
  ui --> engine & scripting
  mcp --> engine
  scripting --> engine
  engine --> ipc & platform
  pluginhost --> ipc & platform
  ipc --> platform
```

| Module | Folder | Contents | May depend on |
|---|---|---|---|
| platform | `src/platform` | OS abstractions: audio and MIDI backends, threads and priorities, shared memory, clocks. Real-time primitives (lock-free queues, RT checker), logging, assertions, crash reports | OS libraries |
| ipc | `src/ipc` | Shared-memory buffers and rings, wake-up primitives, message protocol | platform |
| engine | `src/engine` | Command registry, document model, undo, time, audio graph, transport, file I/O, plugin slots | ipc, platform |
| scripting | `src/scripting` | Lua runtime and the bindings generated from the command registry | engine API |
| mcp | `src/mcp` | MCP protocol and transports, tools generated from the command registry | engine API, UI automation interface |
| ui | `src/ui` | SDL3 windows, RmlUi documents, docking, widgets, UI automation | engine API, scripting, SDL3, RmlUi |
| pluginhost | `src/pluginhost` | Host process, scanner, format adapters (VST3, CLAP, AU, LV2, FST) | ipc, platform, plugin SDKs |
| app | `src/app` | `main` and the wiring between modules | all of the above |

- The engine never depends on UI code, SDL or RmlUi. CMake checks this when configuring (`gnubase_require_no_ui`).
- The MCP server does not depend on the UI either: it defines a UI automation interface, the UI implements it and the app connects the two. A headless app simply has no UI tools.
- The plugin host does not link the engine; the two only share the IPC protocol.
- Third-party code lives in `ext/`, see [dependencies.md](dependencies.md).

## Threads in the app process

| Thread | Runs | Rules |
|---|---|---|
| UI (main thread) | SDL event loop, RmlUi layout and rendering, Lua UI scripts | Sends commands, renders from events, never blocks on the engine |
| Engine command thread | Every command: document changes, undo, building render snapshots | The only thread that writes the document |
| Audio thread | The device callback, one processing cycle at a time | Real-time rules |
| Audio workers | Track chains in parallel (#212) | Real-time rules, real-time priority |
| Disk threads | Streaming audio files in and recordings out (#17) | Work ahead of the audio thread |
| Background threads | Peak files, analysis, saving projects, plugin scan coordination | Low priority |
| Log writer | Writes what audio threads logged, every 10 ms (#8) | Low priority |
| MCP I/O thread | The MCP server's HTTP or stdio transport | Only forwards to the command thread |
| Script thread | Lua in headless mode; with a UI, Lua runs on the UI thread | Never an audio thread |

Data moves between threads only through:

- lock-free queues: commands to the engine, events to clients, messages to the audio thread (#14);
- immutable render snapshots, published with an atomic pointer swap; the command thread frees an old snapshot once the audio thread no longer uses it;
- triple buffers for continuous values such as meters and the playback position, read by the UI once per frame.

## Logging and crash reports

- Every process (the app, each plugin host) writes its own log file in the per-user log folder, through a `logging::Session` created in `main`. The previous run's file is kept as `<name>.previous.log` (#8).
- Audio threads log without allocating or locking: the message and its strings are copied into a lock-free queue, and the log writer thread writes them. If the queue is full, messages are dropped and the number is logged.
- `GNUBASE_ASSERT` checks in every build: a failure is logged, and debug builds stop there. `GNUBASE_ASSERT_REALTIME` marks code that must run on an audio thread; `rt::assertNotRealtime` marks code that must not.
- A crash adds a backtrace to the log (macOS, Linux) or writes a minidump next to it (Windows).

## Engine API

Every action is a **command** in the command registry, with a name, a version, typed parameters and results (JSON schema), documentation and undo behaviour (#10). The MCP tools (#25) and the Lua bindings (#205) are generated from the registry, and CI fails if a command is missing from either (#31).

```mermaid
sequenceDiagram
  participant C as Client (UI, MCP, Lua, test)
  participant Q as Command thread
  participant A as Audio thread
  C->>Q: command (request id, parameters)
  Q->>Q: change the document, record an undo step
  Q->>A: publish a new render snapshot
  Q-->>C: events (what changed), to every client
  Q-->>C: result (request id), to the sender
  A-->>C: meters and position (lock-free buffers, no JSON)
```

- The API is `gnubase::engine::Engine` and `Client` (`src/engine/engine.h`). Commands are asynchronous: `send` returns a request id, then the events and the result arrive in the client's queue; `call` sends and waits for the result.
- Parameters are checked against the command's JSON schema before it runs, and results against theirs; a failure becomes an error result with the reason. A command's events reach every client before its result reaches the sender, and a failed command emits none.
- **Events** name what changed (object ids and properties), so a client updates only what it shows.
- **Meters and the playback position** are not events and never JSON. The audio thread writes them into lock-free buffers, holding each meter's peak until it is read so none is lost between frames, and the UI reads all of them at once per frame (#111). MCP and Lua get them through a query when they need them.
- **Queries** are read-only commands. They also run on the command thread, so they always see a consistent document.
- Because commands, results and events are serializable, the same API works in-process (UI, tests), over MCP and from Lua, and could later run across processes.

## Document model and undo

- A project is a tree of objects with stable ids: tracks, channels, busses, parts, events, clips, automation and plugin slots (#11). The document (`src/engine/document`) has busses, tracks in folders, each track's channel, media files, clips, audio events, and MIDI parts with notes and controllers; automation and plugin slots come with their milestones. Ids count up per project, are never reused, and 0 means none.
- Only commands change it: `project.*`, `bus.*`, `track.*`, `channel.set`, `media.*`, `clip.*`, `audioEvent.*`, `midiPart.*`, `note.*` and `controller.*`. `project.get` returns the whole document. Every change emits events such as `track.added`, `channel.changed` or `note.removed`. A command checks everything before it changes anything, so a failed command changes nothing.
- Positions are in their track's time base: ticks when musical, samples when linear. Notes and controllers are in ticks from their part's start; audio event offsets and lengths are in the clip's sample frames. Linear positions keep their time when the sample rate changes.
- Audio events have a volume in dB (-144 to +6) and can be muted. In DAWproject a muted event is a disabled clip; the volume is GNUbase-only data.
- MIDI controllers have a type: `controller` (a number and a value), `pitchBend`, `channelPressure`, `polyPressure` (a key and a value), `program` (a number and an optional bank, 0 to 16383) and `sysEx` (the whole message, from 0xF0 to 0xF7). Values are normalized: 0 to 1, pitch bend centered at 0.5. In DAWproject controllers, pitch bend and both kinds of pressure are `Points` lanes; program changes and SysEx are GNUbase-only data. An instrument track's channel names the built-in instrument it plays (`channel.set` with `instrument`), until plugin slots come.
- Every command that changes the project is one undo step (#12). Undo steps are stored as **differences**: before a command runs, the model copies the project; afterwards it records each object that changed with its state before and after (`src/engine/document/history.h`). Notes and controllers count as objects, so editing one note doesn't store its whole part. Undo and redo put recorded states back instead of computing inverse operations, so they restore the project exactly. The copy also makes every command atomic: if it fails halfway, the project is restored. Copying costs time in proportion to the project's size, like compiling the render snapshot; if that gets slow for large projects, commands can record what they touch instead and keep the same steps.
- `history.transaction` runs several commands as one step, and none of them if one fails. Each step has an action name, a time and the user's details; `history.undo` and `history.redo` move one or more steps, and `history.set` limits their number. Undoing an addition doesn't free its ids, so ids stay unique. Selection and view state are not in the document and so not in undo.
- The audio thread never reads the document. After each change the command thread compiles an immutable **render snapshot** (`RenderSnapshot`): busses, media files, and the channels in track order with their routing as indices, audio events, notes and controllers in the project's samples, sorted by time. `rt::SnapshotPublisher` hands it to the audio thread. The processing graph is built from it (see "Audio processing").

## Audio processing

One cycle on the audio thread:

1. Read the current render snapshot and the transport state (#16).
2. Schedule MIDI and automation for the cycle, sample-accurately (#19).
3. Run the task graph: track chains in parallel on the workers, each bus once all its inputs are done (#15, #212). A plugin slot hands its block to its host process and waits for the result.
4. Sum busses in a fixed order, apply latency compensation (#71), write the outputs, and feed meters and capture taps.

The **processing graph** (`src/engine/graph`) is what the audio thread runs:

- Its nodes are busses, channels, built-in processors and plugin slots, joined by audio connections and event connections (notes, controllers, parameter changes). `describe` turns a render snapshot into a graph description: a node per bus, then a node per channel, with each channel's input and output routing; MIDI channels send events to their instrument channel instead of audio. `graph.get` returns it.
- The command thread compiles the description into a `CompiledGraph`: it orders the nodes, allocates every audio and event buffer, prepares the processors and computes each node's path latency, the hook for latency compensation (#71). Compiling a new graph never stops the old one; `rt::SnapshotPublisher` swaps it in between two blocks, and the old one is freed on the command thread.
- Processors (`graph::Processor`) belong to the descriptions, not to a compiled graph, so the next graph reuses them with their state and a rebuild causes no glitch. They are prepared again only when the sample rate or the block size changes, which only happens while audio is stopped.
- Processing a block takes the nodes in a fixed order: every node after the nodes it receives from, otherwise by index. A node's input is the sum of its audio inputs in the order of their node indices, so the output is bit-identical for the same input. GCC and Clang build without fused multiply-adds (`-ffp-contract=off`), which round differently, so every platform renders the same samples. Events carry their sample offset within the block, and a node's events arrive sorted by offset.
- **Multi-processing** (#212): the audio thread and a pool of real-time workers (`rt::WorkerPool` in `src/platform/rt`) process the nodes as tasks. Each node counts the nodes it still waits for; the last one to finish puts it on a lock-free ready queue, so a bus starts as soon as all its inputs are done, and nothing allocates, locks or blocks. Of the nodes that are ready at the start of a block, those with the most measured work ahead of them (a moving average of earlier blocks) start first. Because a node sums its inputs in index order once all are done, the output is bit-identical for any number of threads; a random-graph test checks this from 1 to 8 threads, and the TSan job runs it.
- Workers get real-time priority: the "Pro Audio" MMCSS task on Windows, a time-constraint policy on macOS (joining the device's audio workgroup comes with the CoreAudio backend), SCHED_FIFO on Linux where the user may (rtkit follows with the Linux backends). Between blocks they spin for half a block period, then sleep on a semaphore that the audio thread releases without blocking. `audio.setProcessing` turns multi-processing on or off and sets the number of threads, the audio thread included; the default is one per hardware thread.
- `gnubase-bench` (`tests/bench`) measures the engine. Its `workers` case renders 64 instrument tracks with 1, 2, 4 ... threads up to the hardware's and prints the speedup.
- The **audio engine** (`src/engine/audio`) plays the project through an audio device (`audio::AudioCallback` in `src/platform/audio`). After every command that changes the project it compiles a new graph for the device's sample rate and block size. Once per block it copies the device inputs into the input busses, processes the graph and copies the output busses to the device outputs. Busses take consecutive device channels in bus order until the hardware backends (M03) let users pick ports.
- The **dummy device** (`audio::DummyDevice`) has no hardware. Its input is silent or a built-in generator, its output and MIDI output are captured into memory allocated up front, and MIDI is injected at frame positions. It runs offline (`audio.render`: blocks on the calling thread as fast as possible) or in simulated real time (`audio.start`, `audio.stop`: its own thread processes one block per block duration). Both modes produce the same samples, because the output depends only on the project, the input and the block size. `audio.setDevice`, `audio.get` and `audio.capture` complete its commands.
- The **transport** (`Transport` in `src/engine/audio`) holds the project cursor, the left and right locators and the playback modes. Commands (`transport.play`, `stop`, `toggle`, `locate`, `set`, `exchangeLocators`, `get`) send requests through a lock-free queue; the audio thread applies them at the start of its next block, or the command thread right away while no device runs. For every block it hands the graph a `graph::TransportBlock`: whether it plays and, while playing, segments of consecutive project positions. In cycle mode a new segment starts at the exact sample where playback reaches the right locator and jumps to the left one, so loop points are sample-accurate for any block size; with the right locator before the left one the range is skipped instead. Processors read the block from `ProcessContext::transport`; the built-in `positionProbe` outputs each frame's position, so tests check where every frame played. Start from cycle start and return to start position on stop are settings; the selection-based start modes belong to the UI, which knows the selection. Every transport command emits `transport.changed`.
- **Audio tracks** play their audio events from disk streams (`Playback` and `TrackPlayer` in `src/engine/audio/playback.h`). When the graph is rebuilt, each audio track gets a **playlist**: its events in the project's samples, sorted and cut where they overlap. A track plays one event at a time, and where events overlap the one added last (the highest id) is in front; muted events don't play and hide nothing. A clip at another sample rate is converted by its disk stream, and its offset and length are scaled to the project's rate. A clip's channels go one to one to the track's channels, a mono clip goes to all of them, extra clip channels are left out; a mono track reaches a stereo bus through the graph's mono-to-stereo mix.
- Each audio track has a few **voices**: disk streams of its clips. The command thread positions them ahead of playback ("cueing") at every point where playback enters an entry: the cursor, the start of each next entry whose clip doesn't simply continue the one before, and the left locator after a cycle jump. The audio thread takes the voice that is waiting at the frame it needs, so events and cycle jumps start at their exact sample; within an entry it reads on from the same voice. A voice that isn't there in time, or a late disk stream, leaves silence, counted as `playbackDropouts` in `audio.get`.
- Offline rendering (`audio.render`) cues and then waits until every voice's buffer is full before each block, so it never drops out and renders the same samples every time. In real time a cue thread cues the next two seconds every 5 ms; `transport.play` and `transport.locate` cue the start position first and wait up to two seconds for it. Real time and offline rendering produce the same samples as long as the disks keep up.
- **MIDI and instrument tracks** play their parts through a `MidiSequencer` per channel (`src/engine/audio/sequencer.h`), the channel's processor. It reads the channel's notes and controllers from an immutable sequence that the command thread publishes after every change, and turns the transport's segments into events at their sample offset: at one sample note-offs first, then controller events, then note-ons, so a note that ends where the next one starts is retriggered cleanly. It keeps every sounding note with its end and sends its note-off even if the note was edited or removed meanwhile. A stop, a locate and a cycle jump end all sounding notes at their exact sample; notes that started before the new position don't sound again (chasing notes and controllers comes later). An instrument channel (`InstrumentChannel`) runs its sequencer, adds the events of the MIDI channels routed to it and feeds them to its instrument. Sequencers and instruments are kept per channel across rebuilds, so editing never cuts a sound; when a MIDI track is routed elsewhere, the instrument releases the notes it still holds. SysEx events point into the sequence, which stays alive until the next block.
- Events are MIDI 2.0 resolution: 16-bit velocities and 32-bit controller values (`src/engine/midi/midivalues.h`). Converting to and from MIDI 1.0 follows the MIDI 2.0 min-center-max rule: scaling up repeats bits so the center and the maximum stay exact, scaling down shifts.
- GNUbase's built-in processors (`src/engine/processors`) are `graph::Processor`s that run in the engine: for now signal generators (sine, noise, impulse), a position probe for transport tests, a note probe that outputs how many notes sound and the sum of their pitches, gain and pan, and a polyphonic test synth, so MIDI-to-audio paths are tested before plugin hosting. Each one handles parameter changes and notes at their sample offset. `processor.list` returns them with their parameters' ids, ranges, defaults and units, and `processors::createBuiltin` creates one by id.

Time is kept in samples and in musical time (PPQ ticks), with tempo and time signature maps (#13). Tracks that are not monitored live can be processed ahead with larger buffers; how is **decided in #23**.

## Audio files and disk streaming

- `AudioFileReader` and `AudioFileWriter` (`src/engine/audiofile`) read WAV, RF64, Wave64, AIFF, FLAC, Ogg Vorbis and MP3, and write WAV (also BWF and RF64), AIFF and FLAC (#17). Formats are recognized by content; paths are UTF-8 everywhere.
- The audio thread never touches files. A `DiskStream` holds blocks of frames, allocated once, that pass between the disk thread (`DiskStreamer`) and the audio thread through lock-free queues; the disk thread keeps them filled ahead of playback. `read()` never waits: missing frames are silence and count as dropouts, and later frames stay at their time.
- A seek starts a new generation of blocks: the disk thread takes back the older ones and reads from the new position, and the audio thread ignores blocks of older generations. `ready()` says when the buffer is full again, and `full()` when it also holds again every block the audio thread gave back since; offline rendering waits for `full()` before every block.
- Files at another sample rate are converted by the disk thread with r8brain-free-src (`Resampler`). After a seek, reading starts early enough for the filters and on the same frame grid as from the start, so the output is the same as when playing through. `media.convert` writes a converted copy, for importing at the project's sample rate.

## Plugin hosting and IPC

```mermaid
flowchart LR
  subgraph app["gnubase"]
    Slot["Plugin slot"]
  end
  subgraph shm["Shared memory"]
    Audio["Audio buffers"]
    Rings["Event and parameter rings"]
  end
  subgraph host["gnubase-pluginhost"]
    Adapter["Format adapter"]
    Plugin["Third-party plugin"]
  end
  Slot --> Audio & Rings
  Audio & Rings --> Adapter --> Plugin
  Slot -.->|"wake-up"| Adapter
  Slot -.->|"control channel: load, state, editor"| Adapter
```

- **Audio path:** audio, notes and parameter changes for a block go through shared memory. Wake-up uses OS primitives: events on Windows, semaphores with the device's audio workgroup on macOS, futexes on Linux. No allocations or locks on this path (#62).
- **Control path:** loading, state, presets, editor windows and scans use a separate, versioned message channel that is not real-time.
- The binary layout is fixed (no pointers, explicit sizes and alignment), so 32-bit and emulated host processes use the same protocol.
- A watchdog gives every host process a deadline per cycle (#65).
- Plugin editors are windows of the host process, kept in front of and positioned relative to GNUbase's windows (#69).
- Inside the host, every format is adapted to the **GNUbase plugin API** (#61), modeled on CLAP's extension mechanism. GNUbase's own built-in processors implement the same API but run inside the engine (#114).

## Scripting, MCP and tests

- **Lua** runs on the UI thread, or on the script thread when headless, with bindings generated from the command registry. RmlUi's Lua plugin shares the same runtime (#205, #208). Scripts that come with a project never run without the user's consent.
  - `scripting::Runtime` (`src/scripting/runtime.h`) is a Lua 5.4 state with the global table `gnubase`: one function per command (`gnubase.track.add{type = "audio"}` for `track.add`), returning the result as a table or raising an error with the reason; `gnubase.on(name, handler)` for events, `gnubase.waitFor(name, seconds)` and `gnubase.events()`. Commands run synchronously on the script's thread, so coroutines can call them too.
  - Trusted scripts (`gnubase --headless --script file.lua`) get every standard library except `os.exit`, which would skip the engine's shutdown. Untrusted ones, such as scripts in projects, get no `io`, `os`, `debug`, `package`, `require`, `dofile` or `loadfile`, and `load` only accepts source code.
  - MCP clients run untrusted scripts with the tool `script_run`, at most 60 seconds of Lua code each (a count hook stops longer ones). The app adds it through `mcp::Server::addTool`, which also takes the UI tools later; the MCP module itself doesn't depend on scripting.
  - [lua.md](lua.md) is the API reference, generated from the command registry. A test fails when it's out of date (`GNUBASE_UPDATE_DOCS=1` rewrites it), and another fails when a command lacks its MCP tool, its Lua function, a parameter schema or a description (#31).
- **MCP:** streamable HTTP on localhost with an access token, or stdio in headless mode (#25, #26). Its tools come from the command registry plus the UI automation interface: DOM tree, screenshots, input injection (#58).
  - `gnubase --headless` (`src/app/headless.h`) sets up the audio device and loads `--project`, then serves MCP on stdio until stdin ends, or on HTTP (`--mcp-http`) until Ctrl+C or SIGTERM. Stdout belongs to MCP; messages go to stderr and the log file `gnubase-headless.log`. Exit codes are 0 done, 1 unexpected error, 2 invalid command line, 3 audio device or project not set up. Everything except `main` is in the `gnubase-app` library, which has no UI dependency, so tests run it in-process as well as through the real executable.
  - `mcp::Server` (`src/mcp/server.h`) speaks MCP revision 2025-06-18 over JSON-RPC. Every command is a tool named with `_` instead of `.` (MCP clients accept only letters, digits, `_` and `-`), with the command's parameter schema as input schema, its result schema as output schema, and queries marked read-only. Queries without required parameters are also resources, `gnubase://<command>`. A failing command is a tool result with `isError`, so the agent sees why; protocol mistakes are JSON-RPC errors.
  - Each request uses its own engine client, so concurrent requests are safe and no events pile up between requests.
  - `mcp::HttpServer` listens on 127.0.0.1 only. It refuses requests from other addresses, from web pages of other origins (DNS rebinding), and without `Authorization: Bearer <token>` (`mcp::makeToken`). POST /mcp answers with JSON, or 202 for notifications. A stream for server-initiated messages (GET) comes later. `mcp::serveStdio` reads one message per line and writes one response per line.
- **Tests** take the same paths: the dummy device and offline rendering (#21), golden files, and the UI automation API from C++ (#59) or Lua (#206, #208).
  - `gnubase.test` is the Lua test framework (`src/scripting/testing.lua`, compiled in as bytes by CMake): test cases, assertions, fixtures and golden files ([lua.md](lua.md#tests)). The runtime loads it into every Lua state, trusted or not, and runs the test cases a script registered when it ends; the report is printed in TAP format, and a failure fails the script. So `script_run` runs test cases as well, and no command area may be called `test`.
  - Every `tests/lua/*.lua` is a ctest test (labels `integration` and `lua`) that runs `gnubase --headless --script` in the source folder, so failures name `tests/lua/<file>.lua` and the line. Sanitizer builds don't build the app and skip them; the C++ tests of the framework still run there.

## Project format

- Projects are `.dawproject` files: a zip container with XML and media (#22). `project.save` and `project.load` write and read them (`src/engine/dawproject`), with miniz for the container and pugixml for the XML.
- `project.xml` gets everything DAWproject can describe: tracks with their folders, channels with their routing (groups as `submix`, output busses as `master` channels), audio events and MIDI parts as clips, notes, MIDI controllers as `Points`, markers, and tempo and time signature with their changes. Media files are embedded under `audio/` or referenced where they are. GNUbase's output validates against the format's `Project.xsd`.
- GNUbase data that DAWproject cannot express goes into an extra entry in the same container, so other applications can still open the file. The entry is **`gnubase.json`**: the whole document as `project.get` returns it, the id counter, the media paths, and a checksum of `project.xml`. If the checksum matches, loading restores the project exactly (ids, input busses, time bases, MIDI tracks, exact sample positions); if another application changed `project.xml`, the entry is ignored and the project is built from `project.xml`, so stale GNUbase data never overrides someone else's edits.
- Plugin states are stored as DAWproject specifies (#72).

## Open decisions

| Topic | Ticket |
|---|---|
| Anticipative processing | #23 |
| UI render backend | #47 |
| Plugin API details | #61 |
| IPC wake-up details, default hosting mode | #62, #76 |
