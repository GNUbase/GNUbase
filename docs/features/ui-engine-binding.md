# UI-engine binding

How the UI talks to the engine (#50): user actions become engine commands, and what the UI shows follows the engine's
events. The UI is one more client of the engine API, next to MCP and Lua, so it can do nothing they can't
(CLAUDE.md, rule 1). The code is in `src/ui/enginebinding.h`; the engine side is `engine::Client`
(`src/engine/engine.h`).

- **One client.** `ui::EngineBinding` owns an engine client named `ui` and is used on the UI thread only. The UI
  module links the engine's API, never its internals: no document, audio graph or command handler is reachable from
  `src/ui`, and the engine never depends on the UI (`gnubase_require_no_ui`).
- **Commands.** `send(command, parameters, done)` queues a command and returns at once; the UI thread doesn't wait for
  the engine, with two exceptions: a press in the event display waits up to two seconds for the answers that held
  actions wait for (`awaitHeld`, below), and a user script settles (`settle`, [scripts.md](scripts.md)). `done` runs
  on the UI thread with the result. A failed command without `done` is logged.
  `waiting(command)` counts the commands of that name still waiting for their results, so tests can check how many
  queries a view keeps open (queries don't show in `engine.commandLog`). `onResult(command, handler)` hears the result
  of every command of that name the UI sent, after the sender's `done`, for as long as its `Subscription` exists:
  the pool shows that way what its commands did, whether its toolbar or the Media menu sent them
  ([pool.md](pool.md)).
- **Gestures** (#716). `sendStep(gesture, what, description, commands, done)` sends a step of a value widget's
  gesture, a drag, wheel turns or a held arrow key ([widgets.md](widgets.md)), as a `history.transaction` merged with
  the gesture's other steps into one undo step. The steps of one `what` (`mixer 3 channel 5 volume`) go one at a
  time: while one is in flight the next waits, and a newer step of the same gesture replaces the one waiting, so a
  fast drag sends its newest value whenever the engine has answered instead of piling up commands. Steps of other
  gestures and steps without one (gesture 0) are never replaced. A command sent with `send` that isn't a query sends
  the waiting steps first, so that an undo right after a drag undoes all of it.
- **Controls ahead of the engine.** Views read the project again after each undo step, and an answer that the engine
  gave before it had a control's newest step is older than the control. `sending(what)` is true while a step of
  `what` is in flight or waiting; a view that shows an answer in its handler, such as the mixer, doesn't set the
  control then. `handed()` counts the results and events that `update` handed on; a view that shows an answer later,
  such as the inspector, which shows the event display's project in a step task, keeps the count with the answer and
  asks `ahead(what, read)`, also true when the last step the engine took was answered after it. The project read
  after the last step's `history.changed` comes after that step's result, so the view shows the engine's value then.
  A refused step changes nothing, so the project read before it shows again. Values that other clients change (MCP,
  Lua, automation) show at once while no step of the control is under way; during a drag the widget itself keeps the
  drag's value until it ends. Each view begins its `what`s with a name of its own, `stepOwner(view)` (`mixer 3 `,
  `inspector 7 `), so only the view whose control sends waits: the other mixers and the inspector that show the same
  channel follow each step the engine took, also during the drag. A control's steps leave nothing behind: the record
  of them goes away in the `update` after the one that handed on the answer to a query sent after its last taken step,
  such as the project read after that step's `history.changed` (#746), so a session keeps no entry for every channel
  and parameter a gesture touched. Until then the views have no read newer than the control, and one that shows an
  older read again, such as the inspector after a track change, still leaves the control as it is (#1305); a view
  showing what that update handed on still asks `sending` and `ahead` about it, and a later gesture makes the record
  again.
- **Events.** `on(name, handler)` calls `handler` with every event of that name, from whichever client caused it: a
  change made through MCP, Lua or another window reaches the UI the same way as the UI's own. The returned
  `Subscription` ends the handler when it goes, so a view that closes stops listening; subscriptions must not outlive
  the binding. A handler may end its own subscription or others.
- **Waking the UI.** The UI thread sleeps until something happens (docs/architecture.md, "UI shell"). When the
  binding's client queues a result or an event, the engine's thread calls `Shell::wake`, which pushes a wake-up event
  into SDL's event queue; more messages before the next update don't wake it again, so a burst of events can't fill
  SDL's queue. In each step the shell calls `EngineBinding::update` (`Shell::eachStep`), which hands everything that
  arrived to its handlers, in the order the engine sent it.
- **Settling.** `settle` sends the steps that wait and hands on results and events until no command that isn't a query
  is out, including those that the handlers send, e.g. around a user script, so that what it ran through the UI is in
  its undo step ([scripts.md](scripts.md)). A query that what the user does next waits for goes out with
  `sendAwaited`, and settling waits for it too. Meanwhile `afterAwaited` holds what the user does, the event display's
  actions and engine key commands, and runs it in order once the answers are in, after their handlers: Delete right
  after a range is drawn edits the range where `snap.zeroCrossings` moves its edges, an undo after it comes after it,
  a script that draws a range and deletes it has the delete in its step, and a press while they wait waits for them
  before it starts another range ([tools.md](tools.md), #1086): `awaitHeld` hands on results and events until nothing
  is held, for at most two seconds, not for other commands out, and what is still held then runs on the range as it
  is (#1179). Only the event display's actions and engine key commands wait so; the UI commands of other views, such
  as the track list's, run at once ([key-commands.md](key-commands.md)), and so do a user script's own engine calls
  ([scripts.md](scripts.md)). The marker lane's double click asks for its range with `sendAwaited` too.
- **Frame rate.** Views update data models in their handlers; the window draws once in the step, after all of them,
  and drawing waits for vsync. However many events arrive between two frames, the UI lays out and draws once.
- **Drawing what arrived.** The shell draws a window only when something asks for it (docs/architecture.md,
  "UI shell"), and what the engine sends changes documents outside input, which asks for nothing by itself (#715).
  So when results or events reached handlers in an update (a command's `done`, `onResult`'s or `on`'s handlers), the
  binding calls the function the app gave it, which has the shell draw every window in that step
  (`Shell::requestRender`): a handler may change any window, and the binding can't tell which. That costs one frame
  per window for each step in which messages reached handlers, however many did; events that no handler takes and
  results that nothing waits for draw nothing. The UI's Lua `gnubase.on` handlers draw every window the same way
  (`Runtime::dispatchEvents` tells whether one ran). Two kinds of values arrive again and again and draw only what
  they changed, so that windows nothing changed in stay idle: watched values (below), which change once a frame, and
  queries a view repeats, which it sends with `sendQuietly`, e.g. the status line's record time once a second: their
  `done` asks for its own window (`Rml::Context::RequestNextUpdate(0)`) when it changed what an element shows.
- **High-rate values.** Meters and the playback position change faster than the UI draws, and they never become events
  or JSON (docs/architecture.md, "Engine API"). `watch(shows, read, apply)` reads such a value in every update and calls
  `apply` only when it changed: the UI sees the newest value once a frame, and values in between are skipped. `shows` is
  the element that shows the value: after `apply`, its window draws in that step (`Rml::Context::RequestNextUpdate`),
  and no other window does for it. An `apply` that returns `bool` tells whether it changed what an element shows, and
  the window draws only then (#745): the transport controls' time displays follow the position once a frame, but their
  window draws only when a display's text changed, so the time display window that shows only the beat draws twice a
  second at 120 BPM. A view whose element changes in some other way passes null and asks for its window itself when it
  changed something: the markers window follows the position but draws only when its locate arrow moves to another
  marker. While the transport plays, the windows whose displays change draw once a frame at most, and the others stay
  idle. Watches that end a gesture after a pause, the tempo field's wheel turns and Tap Tempo's taps, pass the transport
  controls' root: their `apply` shows the field and the Tap button again, and after a single tap no command is sent, so
  no answer would draw the window (#795). While the binding watches values, the shell steps at least once a frame
  (`ui::g_framePeriod`, 16 ms) even without events, but draws only what changed; without watches it sleeps.
  `Engine::playbackPosition` reads the project cursor without a command and without waiting; the meters follow with
  #111. MCP and Lua get the same values through queries (`transport.get`).
- **Command log.** `engine.commandLog` returns the last 256 commands that ran, oldest first, without queries: each
  with its request id, the client that sent it (`ui`, `mcp`, `lua`, `app` for the command line, `engine` for the
  engine's own commands such as a driver's reset), the command, its parameters, and the error if it failed. An MCP
  client or a Lua test sees there what a click in the UI did; events name the same request id.
- **Checks.** `tests/enginebinding_tests.cpp`: results and events reach the UI thread only in `update`, after the
  binding woke it, once for any number of messages; subscriptions end with their objects, also from inside a handler;
  results reach `onResult` handlers for the UI's own commands only; watched values arrive once an update, only when they
  changed; a gesture's steps go one at a time, the newest replacing those that wait, as one undo step, without holding
  up another control's, and steps without a gesture or of another gesture are kept, also after a refused one; the record
  of a control's steps goes away in the update after the one that handed on a read sent after their last answer, not one
  sent before it, also after a refused one, and a later gesture makes it again; settling sends the steps that wait and waits for commands that change things and
  for queries sent with `sendAwaited`, and for what their answers send; what `afterAwaited` holds runs after the answer,
  in order, what a held one runs itself at once, and after another awaited query that a held one sends.
  `tests/scripts_ui_tests.cpp` has a user script draw a range with Snap to Zero Crossing on and delete it through the
  UI: the delete is in the script's undo step, and an undo after it undoes the delete. `tests/mixerwindow_tests.cpp` and
  `tests/inspector_tests.cpp` drag faders, knobs and value fields (the inspector's and the info line's) and turn the
  wheel while the engine answers late (`test::LateDrag`, `test::lateSteps` in `tests/uiapp.h`): the control never goes
  back toward an earlier step, and the engine and the control agree at the end; while a control moves faster than the
  engine answers, the other mixers and the inspector that show its channel follow its steps. A mixer step and an info
  line volume step that the engine refuses, its track or one of the selected events being gone, show in the command log,
  leave no undo step and let the control show the engine's value again; a strip made again while its fader's step is in
  flight shows the engine's volume at once, not the widget's default. One test changes the transport through MCP and
  checks a document shows it, then clicks the document's button and finds the command in `engine.commandLog`, sent by
  `ui`. Two more check drawing without input: a watched value draws its own window in the step it changed in, and no
  other window; events from MCP, results (a command's `done` or an `onResult` handler) and posted work draw every window
  once in the step they reach handlers, events or results that nothing takes draw nothing, and a quiet query draws only
  the window its `done` asks for. An `apply` that says it changed nothing draws nothing. `tests/transportbar_tests.cpp`
  plays the transport in the app's UI with no input and checks that the project cursor, the transport bar's and the time
  display window's time displays are drawn in every step they change in, also for a position set through Lua, that a
  mixer window doesn't draw at all, idle or during playback, and that the time display window showing only the beat
  draws in the steps the beat changes in and in no other. `tests/tempotrack_ui_tests.cpp` taps Tap Tempo once and checks
  that the tap ends after the pause with no input, in a step in which the transport bar's window draws.
  `tests/markers_ui_tests.cpp` plays across markers with no input: the markers window draws in the steps its arrow moves
  in and in no other. `tests/statusline_tests.cpp` checks that the project window draws the record time in the step its
  text changes in. `tests/scripting_tests.cpp` checks that `dispatchEvents` tells whether a Lua handler ran, and
  `tests/uiscripts_tests.cpp` that a document's `gnubase.on` handler draws its window in the step its event arrives in,
  for an event that no C++ view takes. `tests/engine_tests.cpp` checks the command log and the clients' wake-up,
  `tests/shell_tests.cpp` the step tasks, the frame rate, and that posted work and file dialog answers, the OS's and
  automation's, draw.
