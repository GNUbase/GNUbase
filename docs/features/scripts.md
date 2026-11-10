# User scripts and the script console

Users automate their work with Lua scripts of their own, beyond what macros can do (#209). The reference has no
scripting; its key commands and macros (reference manual pp. 1554-1574, [key-commands.md](key-commands.md)) are what
scripts plug into. The code is `src/app/appscripts.cpp` (`app::AppUi`), the console `resources/ui/script-console.rml`.

- **The script folder.** `scripts` in the settings folder ([settings.md](settings.md)), e.g. `%APPDATA%\GNUbase\scripts`.
  Each `.lua` file directly in it is a user script, named after the file without `.lua`. When the folder doesn't exist,
  GNUbase makes it with the example scripts of `resources/scripts`: `add-audio-tracks.lua` and `list-tracks.lua`.
  Without a settings folder (settings in memory only) there are no user scripts.
- **Scripts are key commands.** Each script is the UI command `script.<name>` (`KeyCommands::add`, area `script`), so
  the key commands dialog lists it, gives it keys (`ui.keyCommands`) and puts it into macros (`ui.macros`), the
  Edit menu's Scripts submenu lists it, and `gnubase.ui.command{command = "script.<name>"}` runs it. The UI finds the
  scripts when it starts and again on Reload; scripts that are gone lose their command (`KeyCommands::remove`), and the
  key commands and macros are applied again for new ones.
- **Running.** A script runs in the UI's Lua (trusted, on the UI thread, [ui-lua.md](ui-lua.md)), with the whole
  engine API, `gnubase.ui` and `rmlui`, never on an audio thread. Its globals stay for the scripts and console lines
  after it. Commands run one after the other and wait for their results. `gnubase.ui.wait` fails in a script run from
  a key or a button, since that runs inside a step of the UI.
- **One undo step.** Whatever a script changes is one undo step named after the script, also when it fails halfway:
  before it runs, the UI notes the id of the last done step (`history.get`), and afterwards `history.join` joins the
  done steps after it ([edit-history.md](edit-history.md)). Changes that end where they started leave no step. Ids grow
  with each step, so this holds when the maximum number of undo steps drops the oldest ones meanwhile. Commands that a
  script sends through `gnubase.ui.command` to the engine go through the UI's client without waiting, so they may come
  after the join, as steps of their own.
- **Output.** What the UI's Lua prints goes to GNUbase's standard output and into the console (`app::ScriptOutput`, the
  last 64 KiB), and so do the scripts' errors with their traceback.

## The script console

The UI command `window.scriptConsole` (Edit > Scripts > Script Console) opens the console in a window of its own, or
closes it. The window is a workspace window, `scriptConsole` in `ui.layout` ([workspaces.md](workspaces.md)): it opens
where it was, and workspaces open, close and place it.

- **Scripts.** The list of user scripts, each with Run, which runs its key command, and Edit, which opens the file with
  the OS's application for `.lua` files, the user's editor. Reload finds the scripts in the folder again; Open Folder
  opens the folder with the OS.
- **Output.** The output (`#script-output`), newest lines at the bottom; Clear empties it.
- **Prompt.** Lua typed into the prompt runs with Enter (`gnubase.ui.runLua{code}`), echoed as `> code`, as one undo
  step named Script Console.
- **Lua API Reference** writes the Lua API reference of this GNUbase, generated from the command registry
  (`scripting::reference`, [api-reference.md](../api-reference.md)), as `lua-api.md` into the script folder and opens
  it with the OS.

The console's Lua uses `gnubase.ui.scripts()` (finds the scripts again and returns `folder` and `scripts`, each with
`name`, `command` and `file`), `gnubase.ui.runLua`, `gnubase.ui.openExternally{path}`, `gnubase.ui.luaReference()`
(returns `file`) and `gnubase.ui.clearScriptOutput()`; users' scripts can call them too.

Not yet: scripts in subfolders, a history of the prompt's lines, values of expressions typed into the prompt shown
without `print`, and scripts that come with a project, which need the user's consent before they run (CLAUDE.md).

## Checks

`tests/lua/ui/scripts.lua` runs in the app's UI: the folder starts with the examples, each a key command, and one Undo
takes back what an example did; a script bound to a key with `ui.keyCommands` runs on the key, adds one undo step
named after it, which undo and redo take back and do again; a macro runs a script twice; the console lists, runs and
reloads scripts, shows what they print, runs prompt lines as one step with their globals kept, shows errors and
clears; a script that fails is one step too and shows its error; a script that writes a script and finds the scripts
again, which replaces its own command, keeps its undo step from a key and from Edit > Scripts, and the new script
runs. `tests/scripts_ui_tests.cpp` checks Edit > Scripts and the console's Edit, Open Folder and Lua API Reference,
with the OS's opening caught (`AppUi::openExternallyWith`). `tests/history_tests.cpp` checks `history.join`, and
`tests/scripting_tests.cpp` that a run inside a run leaves the test cases to the outermost one.
