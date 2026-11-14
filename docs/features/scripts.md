# User scripts and the script console

Users automate their work with Lua scripts of their own, beyond what macros can do (#209). The reference has no
scripting; its key commands and macros (reference manual pp. 1554-1574, [key-commands.md](key-commands.md)) are what
scripts plug into. The code is `src/app/appscripts.cpp` (`app::AppUi`), the console `resources/ui/script-console.rml`.

- **The script folder.** `scripts` in the settings folder ([settings.md](settings.md)), e.g.
  `%APPDATA%\GNUbase\scripts`. Each `.lua` file directly in it is a user script, named after the file without `.lua`.
  When the folder doesn't exist, GNUbase makes it with the example scripts of `resources/scripts`:
  `add-audio-tracks.lua` and `list-tracks.lua`; one that can't be copied doesn't keep the others from it. Without a
  settings folder (settings in memory only) there are no user scripts.
- **Scripts are key commands.** Each script is the UI command `script.<name>` (`KeyCommands::add`, area `script`), so
  the key commands dialog lists it, gives it keys (`ui.keyCommands`) and puts it into macros (`ui.macros`), the
  Edit menu's Scripts submenu lists it, and `gnubase.ui.command{command = "script.<name>"}` runs it. The UI finds the
  scripts when it starts and again on Reload (`gnubase.ui.scripts()`): only scripts that are gone lose their command
  and keys (`KeyCommands::remove`), and only new ones get a command, at once, so the others keep theirs, also in a
  script that finds the scripts again (#934). The keys and macros of new scripts apply in the next step of the UI.
- **Running.** A script runs in the UI's Lua (trusted, on the UI thread, [ui-lua.md](ui-lua.md)), with the whole
  engine API, `gnubase.ui` and `rmlui`, never on an audio thread. Scripts and the console's lines run in globals of
  their own (`scripting::Runtime::newEnvironment`), which read those they don't have from the UI's global table: a
  script that assigns `ScriptConsole` doesn't break the console's document. Their globals stay for the scripts and
  console lines after them. Inline handlers of a document that a script loads through `rmlui`, such as
  `onclick="f()"`, compile in the global table and don't see the script's globals: a script makes a function global
  for them with `_G.f = f`. Commands run one after the other and wait for their results. `gnubase.ui.wait` fails in a
  script run from a key or a button, since that runs inside a step of the UI.
- **Runaway scripts.** A script or console line that has run for 10 s (time in engine commands counts too) asks in a
  warning message box whether to stop it or keep it running for another 10 s; stopping fails it with "the script ran
  longer than 10000 ms", and its changes stay one undo step. Once stopped, the error comes again at every instruction
  of that run without asking, so a script that catches it with `pcall` can't go on. Scripts that it runs share its
  time. With the windows hidden, e.g. in tests, it stops without asking (`AppUi::keepScriptsRunningWith` answers in
  tests). Lua that a script leaves behind and that runs later outside a run has no time limit: `gnubase.on` handlers,
  which the UI calls from its steps, and `rmlui` event listeners. `while true do end` in one of them freezes the UI.
- **One undo step.** Whatever a script changes is one undo step named after the script, also when it fails halfway:
  before it runs, the UI notes the id of the last done step (`history.get`), and afterwards `history.join` joins the
  done steps after it ([edit-history.md](edit-history.md)). Changes that end where they started leave no step. Ids grow
  with each step, so this holds when the maximum number of undo steps drops the oldest ones meanwhile. The UI's own
  commands, which `gnubase.ui.command` sends without waiting, settle (`EngineBinding::settle`) before `history.get`
  and before `history.join`: steps of a control that wait are sent and results handed on until no command that isn't
  a query, nor a query that what the user does waits for (`sendAwaited`, such as `snap.zeroCrossings`), is out,
  including those that the results' handlers send. Only the event display's actions and engine key commands wait for
  those answers: a script's calls of the engine (`gnubase.*` outside `gnubase.ui`) go at once, before what
  `gnubase.ui.command` left waiting, and so do the UI commands of other views ([key-commands.md](key-commands.md)).
  So what the user did before the script stays apart, and what the script ran through the UI, such as engine key
  commands, is in its step.
- **Output.** What the UI's Lua prints goes to GNUbase's standard output and into the console (`app::ScriptOutput`, the
  last 64 KiB), and so do the scripts' errors with their traceback. The console adds what is new in each UI step as a
  text node at the end, and removes a node once the output dropped all of its text, so a script that prints in a loop
  doesn't lay out the whole output again.

## The script console

The UI command `window.scriptConsole` (Edit > Scripts > Script Console) opens the console in a window of its own, or
closes it. The window is a workspace window, `scriptConsole` in `ui.layout` ([workspaces.md](workspaces.md)): it opens
where it was, and workspaces open, close and place it.

- **Scripts.** The list of user scripts, each with Run, which runs its key command, and Edit, which opens the file in
  an editor (`gnubase.ui.openExternally{path, edit = true}`), not with the application for `.lua` files, which may be
  a Lua interpreter that runs it: on Windows the file's edit action, else Notepad; on macOS the default text editor
  (`open -t`); on Linux the desktop's application for Lua files (`xdg-open`), an editor. Reload finds the scripts in
  the folder again; Open Folder opens the folder with the OS.
- **Output.** The output (`#script-output`), newest lines at the bottom; Clear empties it.
- **Prompt.** Lua typed into the prompt runs with Enter (`gnubase.ui.runLua{code}`), echoed as `> code`, as one undo
  step named Script Console.
- **Lua API Reference** writes the Lua API reference of this GNUbase, generated from the command registry
  (`scripting::reference`, [api-reference.md](../api-reference.md)), as `lua-api.md` into the script folder and opens
  it with the OS.

The console's Lua uses `gnubase.ui.scripts()` (finds the scripts again and returns `folder` and `scripts`, each with
`name`, `command` and `file`), `gnubase.ui.runLua`, `gnubase.ui.openExternally{path, edit}`, `gnubase.ui.luaReference()`
(returns `file`) and `gnubase.ui.clearScriptOutput()`; users' scripts can call them too.

Not yet: scripts in subfolders, a history of the prompt's lines, values of expressions typed into the prompt shown
without `print`, and scripts that come with a project, which need the user's consent before they run (CLAUDE.md).

## Checks

`tests/lua/ui/scripts.lua` runs in the app's UI: the folder starts with the examples, each a key command, and one Undo
takes back what an example did; a script bound to a key with `ui.keyCommands` runs on the key, adds one undo step
named after it, which undo and redo take back and do again; a macro runs a script twice; the console lists, runs and
reloads scripts, shows what they print, runs prompt lines as one step with their globals kept, shows errors and
clears; a script that fails is one step too and shows its error; a script that writes a script and finds the scripts
again keeps its undo step from a key and from Edit > Scripts, and the new script runs; an engine key command that a
script runs is in its step; scripts and console lines share globals apart from the documents'; a script that adds a
script and finds the scripts again can still press another script's key. `tests/scripts_ui_tests.cpp` also checks a
new script folder getting the examples that can be copied when one can't be, a runaway script and console line asking
and stopping, a script's undo step with the UI's steps it sent and without the user's before it, the console adding
to and dropping from its output, and Edit > Scripts and the console's Edit, Open Folder and Lua API Reference, with
the OS's opening caught (`AppUi::openExternallyWith`).
`tests/history_tests.cpp` checks `history.join`, and `tests/scripting_tests.cpp` that a run inside a run leaves the
test cases to the outermost one, the time limit that asks, a stopped script that catches the error and environments;
`tests/enginebinding_tests.cpp` settling, also with only a step that waits and a query out.
