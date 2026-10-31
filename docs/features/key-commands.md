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
  everywhere). `bind` checks the keys, that the command exists and an engine command's parameters against its schema;
  keys bound again in the same zone replace what they did there. A command may have several keys, as in the reference
  ("Assign Additional Key"). `keysFor(command, parameters)` returns the keys bound to a command as the platform writes
  them, in the order they were bound, for menus (#54) and tooltips; `run` runs a command as a key would, for a menu
  item.
- **Writing keys.** `ui::parseKeys` reads `Space`, `Mod+Z`, `Shift+Alt+F5` or `Ctrl+Num+`, in any case: letters,
  digits, F1 to F24, the named keys (Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, the
  arrows Up, Down, Left, Right, Space), the punctuation keys `. , - +`, and the keypad's own keys Num0 to Num9,
  NumEnter, `Num*`, `Num/`, `Num+`, `Num-` and `Num.`, which the reference uses for the transport. Letters and
  punctuation follow the keyboard layout (SDL's key codes); digits are digits on French layouts too.
- **Per platform.** `Mod` is the platform's command key: Cmd on macOS, Ctrl on Windows and Linux, the reference's
  "Ctrl/Cmd". `Ctrl` is the Control key everywhere, for the reference's few bindings with a real Ctrl on macOS; `Alt`
  is Option on macOS, `Meta` (or `Cmd`) the Command, Windows or Super key. `ui::keyName` writes keys as the platform
  does, modifiers in the order Ctrl, Alt, Shift, Cmd: `Ctrl+Shift+Z` on Windows and Linux, `Shift+Cmd+Z` on macOS
  (`Opt` for Alt). The default key map is written with `Mod`, so it is the same on every platform.
- **Elements first.** RmlUi gets every key first, at the focused element, from where it bubbles up. An element that
  uses a key stops its propagation: a text field keeps the keys typed into it, a focused button is clicked by Space
  and Enter, a focused knob, fader or slider turns with the arrow keys, menus and dialogs keep their keys, Tab moves
  the focus. A key command runs only for a key that no element
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
| Num-, Num+ (Shift for faster) | `transport.locate` by seconds | Rewind, Forward (Fast Rewind, Fast Forward) |
| Shift+N, Shift+B | `marker.locateNext`, `marker.locatePrevious` | Locate Next/Previous Marker |
| Shift+1 to Shift+9, Num3 to Num9 | `marker.locate` with the number | To Marker 1 to 9 |
| Mod+Z | `history.undo` | Undo |
| Mod+Shift+Z | `history.redo` | Redo |

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
| Mod+L | `edit.moveStartToCursor` | Move Event Start/Range Start to Cursor |
| Mod+Left, Mod+Right | `edit.nudgeLeft`, `edit.nudgeRight` | Nudge Left, Right |
| Alt+Left, Alt+Shift+Right | `edit.nudgeStartLeft`, `edit.nudgeStartRight` | Nudge Start Left, Right |
| Alt+Shift+Left, Alt+Right | `edit.nudgeEndLeft`, `edit.nudgeEndRight` | Nudge End Left, Right |
| Mod+A, Mod+Shift+A | `edit.selectAll`, `edit.selectNone` | Select All, Select None |
| Mod+Alt+I | `edit.invertSelection` | Invert Selection |

The other edit commands ([parts-and-events.md](parts-and-events.md)) have no default keys in the reference. The
transport controls' commands (F2, the enter commands, Exchange Time Formats, P for Locators to Selection) are in
[transport-bar.md](transport-bar.md).

Commands that come with their tickets bring their keys: opening windows such as the mixer (F3), cycle, record and the
rest of the reference's tables.

## Checks

`tests/keycommands_tests.cpp`: keys read and written per platform (`Mod` as Cmd on macOS, Ctrl elsewhere; every key
name reads back as written); the default key map passes the engine's schemas on every platform and binds no keys twice
in a zone; UI commands, replacing bindings and refusing unknown commands, bad parameters and unknown keys. In the app's
UI (`test::UiApp`, [ui-tests.md](ui-tests.md)), keys pressed through `ui::Automation` start, stop and locate the
transport and undo through the UI's client; the same keys run different commands in a project zone, an editor zone
inside a lower zone, an element of the lower zone and outside any zone; lock keys don't count; a key from SDL's own
event queue runs its command; and text fields, a focused button, a focused knob and a modal dialog keep their keys.

Not yet: the key commands editor, presets and macros, which will keep the key map in the user settings, so that MCP
and Lua reach it too; exporting the assignments; keys in tooltips; the On-Screen Keyboard, which takes most keys while
it shows; a frame around the focused zone.
