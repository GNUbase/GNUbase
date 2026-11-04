# UI-engine binding

How the UI talks to the engine (#50): user actions become engine commands, and what the UI shows follows the engine's
events. The UI is one more client of the engine API, next to MCP and Lua, so it can do nothing they can't
(CLAUDE.md, rule 1). The code is in `src/ui/enginebinding.h`; the engine side is `engine::Client`
(`src/engine/engine.h`).

- **One client.** `ui::EngineBinding` owns an engine client named `ui` and is used on the UI thread only. The UI
  module links the engine's API, never its internals: no document, audio graph or command handler is reachable from
  `src/ui`, and the engine never depends on the UI (`gnubase_require_no_ui`).
- **Commands.** `send(command, parameters, done)` queues a command and returns at once; the UI thread never waits for
  the engine. `done` runs on the UI thread with the result. A failed command without `done` is logged.
  `waiting(command)` counts the commands of that name still waiting for their results, so tests can check how many
  queries a view keeps open (queries don't show in `engine.commandLog`). `onResult(command, handler)` hears the result
  of every command of that name the UI sent, after the sender's `done`, for as long as its `Subscription` exists:
  the pool shows that way what its commands did, whether its toolbar or the Media menu sent them
  ([pool.md](pool.md)).
- **Events.** `on(name, handler)` calls `handler` with every event of that name, from whichever client caused it: a
  change made through MCP, Lua or another window reaches the UI the same way as the UI's own. The returned
  `Subscription` ends the handler when it goes, so a view that closes stops listening; subscriptions must not outlive
  the binding. A handler may end its own subscription or others.
- **Waking the UI.** The UI thread sleeps until something happens (docs/architecture.md, "UI shell"). When the
  binding's client queues a result or an event, the engine's thread calls `Shell::wake`, which pushes a wake-up event
  into SDL's event queue; more messages before the next update don't wake it again, so a burst of events can't fill
  SDL's queue. In each step the shell calls `EngineBinding::update` (`Shell::eachStep`), which hands everything that
  arrived to its handlers, in the order the engine sent it.
- **Frame rate.** Views update data models in their handlers; the window draws once in the step, after all of them,
  and drawing waits for vsync. However many events arrive between two frames, the UI lays out and draws once.
- **Drawing what arrived.** The shell draws a window only when something asks for it (docs/architecture.md,
  "UI shell"), and what the engine sends changes documents outside input, which asks for nothing by itself (#715).
  So when results or events reached handlers in an update (a command's `done`, `onResult`'s or `on`'s handlers), the
  binding calls the function the app gave it, which has the shell draw every window in that step
  (`Shell::requestRender`): a handler may change any window, and the binding can't tell which. That costs one frame
  per window for each step in which messages reached handlers, however many did; events that no handler takes and
  results that nothing waits for draw nothing, and an idle UI sleeps. Watched values (below) draw only their own
  window, since they change once a frame.
- **High-rate values.** Meters and the playback position change faster than the UI draws, and they never become
  events or JSON (docs/architecture.md, "Engine API"). `watch(shows, read, apply)` reads such a value in every update
  and calls `apply` only when it changed: the UI sees the newest value once a frame, and values in between are
  skipped. `shows` is the element that shows the value: after `apply`, its window draws in that step
  (`Rml::Context::RequestNextUpdate`), and no other window does for it. While the transport plays, the windows that
  show the position (the project window, the time display, the markers window) draw once a frame, and the others
  stay idle. While the binding watches values, the shell steps at least once a frame (`ui::g_framePeriod`, 16 ms)
  even without events, but draws only what changed; without watches it sleeps. `Engine::playbackPosition` reads the
  project cursor without a command and without waiting; the meters follow with #111. MCP and Lua get the same values
  through queries (`transport.get`).
- **Command log.** `engine.commandLog` returns the last 256 commands that ran, oldest first, without queries: each
  with its request id, the client that sent it (`ui`, `mcp`, `lua`, `app` for the command line, `engine` for the
  engine's own commands such as a driver's reset), the command, its parameters, and the error if it failed. An MCP
  client or a Lua test sees there what a click in the UI did; events name the same request id.
- **Checks.** `tests/enginebinding_tests.cpp`: results and events reach the UI thread only in `update`, after the
  binding woke it, once for any number of messages; subscriptions end with their objects, also from inside a handler;
  results reach `onResult` handlers for the UI's own commands only; watched values arrive once an update, only when
  they changed. One test changes the transport through MCP and
  checks a document shows it, then clicks the document's button and finds the command in `engine.commandLog`, sent by
  `ui`. Two more check drawing without input: a watched value draws its own window in the step it changed in, and no
  other window; events from MCP, results and posted work draw every window once in the step they reach handlers, and
  events or results that nothing takes draw nothing. `tests/transportbar_tests.cpp` plays the transport in the app's
  UI with no input and checks that the project cursor, the transport bar's and the time display window's time
  displays are drawn in every step they change in, also for a position set through Lua.
  `tests/engine_tests.cpp` checks the command log and the clients' wake-up, `tests/shell_tests.cpp` the step tasks,
  the frame rate, and that posted work and file dialog answers draw.
