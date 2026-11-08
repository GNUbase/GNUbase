# Key commands

Keys run commands (#57, reference manual pp. 1554–1574): the engine's commands and the UI's own, by a default key map
written once for every platform, depending on where the focus is. The code is in `src/ui/keycommands.h`; the app sets
it up in `app::AppUi`.

- **Commands.** A key command is an engine command, by its name (`transport.toggle`) with the parameters its binding
  gives it, or a command of the UI's own (`ui::UiCommand`, added with `KeyCommands::add`), such as zooming or opening a
  window. Both share the engine's ids, "area.action"; a UI command can't take an engine command's id. Engine commands
  go through the UI's engine client (`EngineBinding::send`), so a key can do nothing that MCP and Lua can't, and
  `engine.commandLog` shows what a key did as sent by `ui`. A failed command is logged.
- **Key map.** A binding (`ui::KeyBinding`) has keys, a command, the command's parameters and a focus zone (none for
  everywhere). `bind` checks the keys, and the command and its parameters with `check` (#446), which menu items and
  transport controls use too: the command must exist, an engine command's parameters must fit its schema, and a UI
  command, which takes no parameters, gets an empty object; it throws `std::invalid_argument` naming the command.
  Keys bound again in the same zone replace what they did there. A command may have several keys, as in the reference
  ("Assign Additional Key"). `keysFor(command, parameters)` returns the keys bound to a command as the platform writes
  them, in the order they were bound, for menus (#54) and tooltips; `run` runs a command as a key would, for a menu
  item. `enableWhen(command, enabled)` says when a command can run and `canRun` asks, so that the main menu disables
  its items ([main-menu.md](main-menu.md)); keys run their commands either way.
- **Writing keys.** `ui::parseKeys` reads `Space`, `Mod+Z`, `Shift+Alt+F5` or `Ctrl+Num+`, in any case: letters,
  digits, F1 to F24, the named keys (Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, the
  arrows Up, Down, Left, Right, Space), the punctuation keys `. , - +`, `Alt` on its own (with Alt held, as the
  keyboard sends it, so that MCP's `ui_key` can press it to open the menu bar, [main-menu.md](main-menu.md)), and the
  keypad's own keys Num0 to Num9, NumEnter, `Num*`, `Num/`, `Num+`, `Num-` and `Num.`, which the reference uses for the transport. Letters and
  punctuation follow the keyboard layout (SDL's key codes); digits are digits on French layouts too.
- **Per platform.** `Mod` is the platform's command key: Cmd on macOS, Ctrl on Windows and Linux, the reference's
  "Ctrl/Cmd". `Ctrl` is the Control key everywhere, for the reference's few bindings with a real Ctrl on macOS; `Alt`
  is Option on macOS, `Meta` (or `Cmd`) the Command, Windows or Super key. `ui::keyName` writes keys as the platform
  does, modifiers in the order Ctrl, Alt, Shift, Cmd: `Ctrl+Shift+Z` on Windows and Linux, `Shift+Cmd+Z` on macOS
  (`Opt` for Alt). The default key map is written with `Mod`, so it is the same on every platform.
- **Elements first.** RmlUi gets every key first, at the focused element, from where it bubbles up. An element that
  uses a key stops its propagation: a text field keeps the keys typed into it, and also the arrow keys without
  modifiers that it can't use at its first or last line or character, which RmlUi lets through to move the focus
  (`nav`): where they move it nowhere, `KeyCommands::press` runs no command for them. A focused button is clicked by Space
  and Enter (not a toolbar or transport button, [widgets.md](widgets.md)), a focused knob, fader or slider turns with
  the arrow keys, menus and dialogs keep their keys, Tab moves the focus: between the zones of a docking window
  ([docking.md](docking.md)), and between controls in text fields and dialogs. A key command runs only for a key that
  no element
  used: where `Rml::Context::ProcessKeyDown` says so, `SdlWindowSystem` passes the key on with its input event, and
  the shell hands it to its handler (`Shell::onUnusedKey`, `KeyCommands::press`). `ui::Automation::key`, and so MCP's
  `ui_key` and the UI tests, does the same. Widgets that act on keys must stop them, or the key commands run too.
- **Focus zones.** An element with a `focuszone` attribute is a focus zone, e.g. `focuszone="project"` on the project
  window's document or `focuszone="keyEditor"` on an editor. Clicking an element gives it the focus, so the zone
  is where the user last clicked or tabbed to. Keys are looked up in the innermost zone around the focus first, then
  in the zones around it, then among the bindings for everywhere: the arrow keys can select events in the project
  window and move notes in an editor (the reference's Navigate commands). A UI command gets the zone element its
  binding was found in, or for a binding for everywhere the innermost zone, so it knows which editor to zoom.
- **What doesn't run.** Nothing runs while a modal document has the focus, so typing into a dialog never edits the
  project. Lock keys (Caps Lock, Num Lock, Scroll Lock) don't change what a key does. A key held down runs its command
  once; the OS's key repeats don't run it again, so holding Space doesn't start and stop playback over and over.
- **Held keys.** A UI command with a `release` runs while its key is held, such as Forward, which winds: the key going
  up ends it (`KeyCommands::released`), whatever modifiers are still down, and so does the window losing the keyboard
  focus. `ui::Automation::key` and MCP's `ui_key` press and release a key, or only press (`down`) or release
  (`up`) it.

## Default key map

The reference's defaults for the commands GNUbase has so far (`ui::defaultKeyMap`):

| Keys | Command | Reference |
|---|---|---|
| Space | `transport.toggle` | Start/Stop |
| NumEnter | `transport.play` | Start |
| Num0 | `transport.stop` | Stop |
| Num1, Num2 | `transport.locate` to the left, right locator | Go to Left/Right Locator |
| Num. | `transport.locate` to the project start | Go to Project Start |
| Mod+Num1, Mod+Num2 | `transport.set` left, right to the cursor | Set Left/Right Locator to Project Cursor Position |
| Num/, Num* | `transport.toggle` cycle, record | Cycle, Record |
| C, I, O | `transport.toggle` metronome, punchIn, punchOut | Activate Metronome, Punch In, Punch Out |
| Shift+N, Shift+B | `marker.locateNext`, `marker.locatePrevious` | Locate Next/Previous Marker |
| Shift+1 to Shift+9, Num3 to Num9 | `marker.locate` with the number | To Marker 1 to 9 |
| Mod+1 to Mod+9, Mod+Num3 to Mod+Num9 | `marker.put` with the number | Set Marker 1 to 9 |
| Shift+Num1 to Shift+Num9 | `marker.setLocators` with the number | Recall Cycle Marker 1 to 9 |
| Insert | `marker.add` | Insert Marker |
| Mod+M | `window.markers` (the app's) | Open Markers |
| Mod+T | `window.tempoTrack` (the app's) | Open Tempo Track Editor ([tempo-track.md](tempo-track.md)) |
| Mod+Z | `history.undo` | Undo |
| Mod+Shift+Z | `history.redo` | Redo |

The user's own keys go over these ([below](#the-users-key-commands-and-macros)).

UI commands are added after the key map, so their keys are bound where the app adds them. The project window's
(`app::AppUi`, [event-display.md](event-display.md)), in the zone `project`:

| Keys | Command | Reference |
|---|---|---|
| H, G | `zoom.in`, `zoom.out` | Zoom In, Zoom Out |
| Shift+H, Shift+G | `zoom.inVertically`, `zoom.outVertically` | Zoom In/Out Vertically |
| Shift+F | `zoom.full` | Zoom Full |
| F | `view.autoScroll` | Auto-Scroll On/Off |
| Delete, Backspace | `edit.delete` | Delete |
| Shift+M, Shift+U | `edit.mute`, `edit.unmute` | Mute Events, Unmute Events |
| Mod+Shift+L, Mod+Shift+U | `edit.lock`, `edit.unlock` | Lock, Unlock |
| Alt+X | `edit.splitAtCursor` | Split At Cursor |
| Mod+D | `edit.duplicate` | Duplicate |
| Mod+K | `edit.repeat` | Repeat |
| Q | `edit.quantize` (the app's, everywhere) | Quantize ([quantize.md](quantize.md)) |
| Mod+L | `edit.moveStartToCursor` | Move Event Start/Range Start to Cursor |
| Mod+Left, Mod+Right | `edit.nudgeLeft`, `edit.nudgeRight` | Nudge Left, Right |
| Alt+Left, Alt+Shift+Right | `edit.nudgeStartLeft`, `edit.nudgeStartRight` | Nudge Start Left, Right |
| Alt+Shift+Left, Alt+Right | `edit.nudgeEndLeft`, `edit.nudgeEndRight` | Nudge End Left, Right |
| Mod+A, Mod+Shift+A | `edit.selectAll`, `edit.selectNone` | Select All, Select None |
| Mod+Alt+I | `edit.invertSelection` | Invert Selection |
| Left, Right | `edit.selectPrevious`, `edit.selectNext` | Navigate Left, Right |
| Up, Down | `track.selectPrevious`, `track.selectNext`, and the first event on the track while an event is selected, unless `ui.upDownSelectsTracksOnly` ([track-list.md](track-list.md)) | Navigate Up, Down |
| Alt+F2 | `track.rename` | Rename Track |
| Mod+X, Mod+C, Mod+V | `edit.cut`, `edit.copy`, `edit.paste` | Cut, Copy, Paste ([range-editing.md](range-editing.md)) |
| Mod+Shift+X, Shift+Backspace, Mod+Shift+V | `edit.cutTime`, `edit.deleteTime`, `edit.pasteTime` | Cut Time, Delete Time, Paste Time |
| Mod+Shift+E | `edit.insertSilence` | Insert Silence |
| Shift+X | `edit.splitRange` | Split (range) |
| J | `edit.snap` | Snap On/Off ([snap.md](snap.md)) |

The other edit commands ([parts-and-events.md](parts-and-events.md)) have no default keys in the reference. The
transport controls' commands (F2, the enter commands, Exchange Time Formats, P for Locators to Selection) are in
[transport-bar.md](transport-bar.md).

The UI command `file.quit` (Quit, in the main menu's File menu) has Mod+Q, everywhere, and `window.onScreenKeyboard`
(Show/Hide On-Screen Keyboard, [onscreen-keyboard.md](onscreen-keyboard.md)) Alt+K. Commands that come with their
tickets bring their keys, such as the mixer's F3 and Alt+F3 ([mixer-window.md](mixer-window.md)), cycle, record and the
rest of the reference's tables.

## Checks

`tests/keycommands_tests.cpp`: keys read and written per platform (`Mod` as Cmd on macOS, Ctrl elsewhere; every key
name reads back as written); the default key map passes the engine's schemas on every platform and binds no keys twice
in a zone; UI commands, replacing bindings and refusing unknown commands, bad parameters and unknown keys; `check`
refusing a typo in a command, parameters an engine command doesn't take or of the wrong type, and parameters for a UI
command. In the app's
UI (`test::UiApp`, [ui-tests.md](ui-tests.md)), keys pressed through `ui::Automation` start, stop and locate the
transport and undo through the UI's client; the same keys run different commands in a project zone, an editor zone
inside a lower zone, an element of the lower zone and outside any zone; lock keys don't count; a key from SDL's own
event queue runs its command; and text fields, also their arrow keys at the ends, a focused button, a focused knob and
a modal dialog keep their keys, while Space after a click on a toolbar or transport button starts and stops playback
(`tests/transportbar_tests.cpp`).

`tests/keycommandsdialog_tests.cpp` checks the user's key commands and macros, their presets and files, and the
dialog below.

## The user's key commands and macros

The user changes keys and sets up macros in the key commands dialog (#161, reference manual pp. 1554-1559); both are
user settings ([settings.md](settings.md)), so MCP and Lua change them with `settings.set` like the dialog does.

- **`ui.keyCommands`** lists the commands whose keys the user changed: each a command, the parameters it runs with
  (left out for none) and all its keys (left out for none), which replace its default keys. Keys are written as in the
  default key map, with `Mod` for Ctrl (Cmd on macOS), so a file from Windows works on macOS: `ui::keySetting` writes
  them, modifiers in the order Mod, Ctrl, Alt, Shift, Meta. `KeyCommands::customize` puts them over the default
  bindings (those `bind` made, which stay as they are): a command's keys go into the zone of its first default
  binding, everywhere if it has none, and win over another command's same keys there. An entry whose command doesn't
  exist (a macro deleted, a command of another GNUbase) or whose keys can't be read is left out and logged, and stays
  in the setting.
- **`ui.macros`** lists the macros: each a name and the commands it runs one after the other, with their parameters.
  A macro is the command `macro.<name>`, which keys, menus and `gnubase.ui.command` run like any other
  (`KeyCommands::run`); a macro in a macro doesn't run, nor does a command that `check` refuses. Edit > Macros lists
  the macros, after them Key Commands... (`app::AppUi` fills the submenu when the setting changes).
- **User scripts** are commands too, `script.<name>`, which get keys and go into macros like any other
  ([scripts.md](scripts.md)); Edit > Scripts lists them.
- **Presets** are the setting `ui.keyCommandPresets`: by name, the key commands and macros (p. 1559).
  `keyCommands.savePreset {name}` saves those of the settings, or the `keyCommands` and `macros` it gets;
  `keyCommands.removePreset {name}` removes one. Loading one is `settings.set` with `ui.keyCommands` and `ui.macros`
  from it, which replaces both.
- **Files.** `keyCommands.export {path}` writes the settings' key commands and macros into a JSON file
  (`"format": "gnubase.keyCommands"`, version 1), and `keyCommands.import {path, name}` makes a preset of one, to take
  them to another computer. The UI commands `keyCommands.exportFile` and `keyCommands.importFile` ask for the file
  with the OS's file dialog; an imported preset is named after the file.
- The app reads both settings when it starts and follows `settings.changed`.

## The dialog

The UI command `window.keyCommands` opens `resources/ui/key-commands.rml`, modal (Edit > Key Commands...). Its Lua gets
the commands with their default keys from `gnubase.ui.keyCommands()` (`KeyCommands::commands`: the UI's commands, the
engine's that change something and run without parameters, and the default bindings' commands with their parameters)
and writes keys with `gnubase.ui.keyName`, which takes `keys` as the settings write them, or a key event's `key` with
`ctrl`, `alt`, `shift` and `meta`, and returns `keys` and the `name` the platform shows (none for a modifier on its
own).

- **The list.** The commands by category, their area (`transport`, `edit`, `macro`, ...), with their keys. Expand All
  and Collapse All open and close the categories; a search or a filter opens them all.
- **Searching.** Text typed into the search field shows the commands whose name, parameters or description contain it;
  with Press Key on, a key pressed into the key field shows the commands it runs. Reset Search clears both. The filter
  shows All, Assigned (with keys), Customized (whose keys differ from their defaults) or Unassigned commands.
- **Keys.** The selected command's keys and description. A key pressed into the key field ("Click here and press a
  key") is added to them, as Assign Additional Key does; if another command has it in the same zone, that command
  loses it, and the message says which. Escape still closes the dialog. Remove Key Command removes the key selected in
  the keys list; Reset Key Command gives the command its default keys back, taking them from any command that has
  them now. Defaults resets every command, Clear All removes every key.
- **Macros.** Macros shows the macros section: New Macro, Duplicate Macro, Delete Macro, and Rename Macro with the
  name in the field (the reference renames by double-clicking). Add Command adds the command selected in the list after
  the selected command of the macro, or at its end; Remove Command, Move Up and Move Down change the macro's commands.
  Macros are in the list under `macro`, so they get keys like any command. Run Macro runs the selected macro's
  commands as the dialog shows them.
- **Apply, OK, Cancel.** Apply sends what the dialog shows, in one `settings.set` with `ui.keyCommands` and
  `ui.macros`, or `settings.reset` for one that the dialog leaves empty (an empty Lua table would reach the engine as
  an object); OK does the same and closes; Cancel and Escape drop it.
- **Presets.** The presets list, a name field, Save (applies what the dialog shows, then `keyCommands.savePreset`),
  Delete, and Import... and Export... (the file dialogs; Export applies first). Choosing a preset puts its key commands
  and macros into the dialog, for Apply or OK.

Not yet: File > Export > Key Command Assignments, the HTML list of every key; the confirmations before Remove and
Defaults (Cancel undoes either); keys in tooltips; a frame around the focused zone. While the on-screen keyboard shows,
it takes most keys ([onscreen-keyboard.md](onscreen-keyboard.md)).
