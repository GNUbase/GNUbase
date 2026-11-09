# Lua in the UI

The UI has a Lua runtime of its own (#208): GNUbase's documents script themselves with it, and UI tests written in Lua
drive the UI with it. It is the same `scripting::Runtime` as headless scripts use ([Lua API reference](../api-reference.md)), so the UI's
Lua reaches the engine only through the engine API, like every other client.

- **Where it lives.** `app::AppUi` (`src/app/appui.h`) makes a trusted runtime (every standard library except
  `os.exit`) before RmlUi starts and closes it after RmlUi shut down, as RmlUi's Lua plugin needs. It runs on the UI
  thread; `print` writes to the app's standard output and the script console ([scripts.md](scripts.md)), which also
  runs the user's scripts on this runtime.
- **RmlUi's Lua plugin.** RmlUi's Lua bindings, built on GNUbase's Lua (compiled as C++, `RMLUI_LUA_BINDINGS_LIBRARY`
  `lua_as_cxx` in `CMakeLists.txt`), run on that runtime: a document's `<script>` runs when the document loads, and
  event attributes such as `onclick="..."` are Lua with `event`, `element` and `document`. Lua reads and changes the
  DOM, adds event listeners and opens data models (`context:OpenDataModel(name, table)`), through the global `rmlui`
  (`rmlui.contexts[1]` is the main window's context). Lua closes the data models it opens
  (`context:CloseDataModel(name)`), since the plugin frees a model only then. The plugin's own `print`, which writes
  into RmlUi's log, is replaced by GNUbase's again. Styling stays in RCSS: Lua sets classes and attributes, not style
  properties.
- **The engine.** `gnubase.<area>.<name>` sends a command and waits for its result, so a long command, such as a
  render, holds the UI until it is done. `gnubase.on` handlers get the engine's events in each step of the shell; a
  handler that fails is logged.
- **UI automation.** `gnubase.ui` has the MCP UI tools as functions (`app::addUiFunctions`, `src/app/uitools.h`):
  `ui_click` is `gnubase.ui.click`, with the tool's arguments as a table and its result as a table, a screenshot as the
  PNG file's bytes. Both come from one list of tools, and the arguments are checked against the same schemas, so Lua and
  MCP behave the same ([ui-automation.md](ui-automation.md), [mcp.md](../mcp.md#ui-tools)). The app adds
  `gnubase.ui.command`, which runs a key command as its keys would, by `command` and optional
  `parameters` ([key-commands.md](key-commands.md)), for documents' buttons such as the project window's Add Track
  (`gnubase.ui.command{command = "project.addTrack"}`). It runs in the window of the document that called it, so a
  dialog that a document in a window of its own opens shows in that window (#448): the nearest Lua local `document` up
  the caller's stack that is one of RmlUi's documents, as event handlers such as `onclick` and `onshow` have, names it
  (`scripting::Runtime::caller`). The walk stops at the call of a gnubase function, so Lua that one runs, such as the script console's prompt (`gnubase.ui.runLua`) or a script that `gnubase.ui.command` starts, doesn't see the console's `document`; without one, as in a script, it runs in the main window. It checks the command first, as `KeyCommands::check` does for
  keys, menu items and buttons (#669): a command that doesn't exist, parameters that aren't a table with keys (a list
  such as `{1, 2}`, or a string), parameters an engine command doesn't take or has of the wrong type, parameters for a
  UI command, and anything but `command` and `parameters` in its table raise a Lua error at the call, in users'
  scripts too, and nothing runs. The app also adds `gnubase.ui.formatTimecode` and
  `gnubase.ui.parseTimecode`, which the project setup dialog converts timecodes with
  ([project-setup.md](project-setup.md)), and `gnubase.ui.parseTime`, which the add track dialog reads a ruler track's
  time offset with ([ruler-tracks.md](ruler-tracks.md)), and `gnubase.ui.projectSelection` and
  `gnubase.ui.selectInProject`, which read and set the project window's selection (`tracks` of the track list, `events`
  of the event display) for the project rule editor ([project-rules.md](project-rules.md)). On the UI thread the functions run at once;
  `gnubase.ui.wait` steps the shell between its checks. A step can't run inside a step (`Shell::step` throws), so
  waiting fails inside the UI's own event handlers, instead of handling events within one.
- **Lua UI tests.** Every `tests/lua/ui/*.lua` is a ctest test with the labels `ui` and `lua`. It runs in
  `test::UiApp`, the UI test harness ([ui-tests.md](ui-tests.md)), through the hidden Catch2 test case
  `[lua-ui]` (`tests/uiapp_tests.cpp`), which `GNUBASE_LUA_UI_TEST` tells which file, in the source folder. The file
  runs in the app's UI runtime: it loads documents with `rmlui`, drives them with `gnubase.ui`, checks the engine with
  `gnubase` and reports with `gnubase.test`, like the other Lua tests. Without an SDL_GPU device it is skipped, like
  the other UI tests. `tests/lua/ui/dialog.lua` opens a dialog by clicking, types a new name into a field bound to a
  data model, and checks that the dialog's Lua renamed the track in the engine. `tests/lua/ui/command.lua` runs a UI
  command and an engine command with its parameters through `gnubase.ui.command`, and checks that it refuses the
  calls above without running anything.
