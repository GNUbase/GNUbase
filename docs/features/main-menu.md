# Main menu

The project window's menu bar (#392, reference manual p. 55): menus whose items run the engine's commands and the UI's
own, as keys do, show their keys and are disabled while their command can't run. The menus are in
`resources/ui/project.rml`; `ui::MainMenu` (`src/ui/mainmenu.h`) runs their items, and `app::AppUi` attaches it to the
project window.

- **Items.** A `<menuitem>` with a `command` attribute runs that command through the key commands
  (`KeyCommands::run`, [key-commands.md](key-commands.md)), with the JSON object in its `parameters` attribute:
  `<menuitem command="transport.locate" parameters='{"target": "leftLocator"}'>`. An engine command goes through the
  UI's engine client, so a menu can do nothing that MCP and Lua can't; a UI command gets the window the menu is in and
  the focus zone around the item. Adding an entry is one line in the document; a new feature adds its command (engine
  or UI) and its item, e.g. Import with #92.
- **Keys.** Each command item gets a `.menu-keys` element with the first key bound to the command with those
  parameters (`KeyCommands::keysFor`), as the platform writes it: Ctrl+Z, or Cmd+Z on macOS.
- **Disabled.** An item is `disabled` while `KeyCommands::canRun` says its command can't run: what
  `KeyCommands::enableWhen` set for it, e.g. Undo and Redo while the undo history (`history.changed`) has nothing to
  undo or redo, and the project window's edit commands without the event display. A command that doesn't exist is
  disabled too. The menu updates in every step of the shell, so the DOM that MCP's UI tools read is always current.
- **The menu bar** is a docking strip, `strip="menuBar"` ([docking.md](docking.md)): Menu Bar in the Set up Window
  Layout menu and the UI command `window.menuBar` show and hide it, and `ui.layout` keeps it per window. Its titles are
  buttons that open the menus (widgets.md, "Menus and context menus"), and submenus are menus inside items.

## Menus

| Menu | Items |
|---|---|
| File | New Project, Open...; Save, Save As..., Back up Project..., Save as Template..., Save New Version, Revert; Quit ([project-files.md](project-files.md)) |
| Edit | Undo, Redo; Delete; Functions (Split at Cursor, Split Loop, Duplicate, Repeat..., Fill Loop, Convert to Real Copy); Select (All, None, Invert, In Loop, From Start to Cursor, From Cursor to End, Events under Cursor); Move (Event Start to Cursor, Event End to Cursor); Mute, Unmute, Lock, Unlock |
| Transport | Start, Stop, Start/Stop; Go to Project Start; Locators (Go to Left/Right Locator Position, Set Left/Right Locator to Project Cursor Position, Exchange Left & Right Locator Positions); Set Project Cursor Position (Locate Next/Previous Marker) |
| Studio | Studio Setup... (`window.studioSetup`, [studio-setup.md](studio-setup.md)) |

The other menus of the reference (Project, Audio, MIDI, Media, Workspaces, Window, Help) and their items come with the
features they run. Quitting waits for the shell's next step, since it closes every window.

## Not yet

The reduced menus of other windows (the mixer and the editors, when they open in windows of their own); Show Menu Bar
in the context menu of a window's title bar on Windows, which the OS draws (it is in Set up Window Layout for now); the
system menu bar on macOS, where the window shows its own menu bar until then; arrows on items with submenus; moving
between the menus of an open menu bar with the mouse or the arrow keys, and opening them with Alt.

## Checks

`tests/mainmenu_tests.cpp` drives the project window in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) with
`ui::Automation` clicks, as MCP's `ui_click` does: Studio > Studio Setup opens the studio setup dialog and closes the
menu; items show their keys per platform; Transport > Locators > Go to Right Locator Position sends `transport.locate`
with its parameters through the UI's client; Edit > Select > All and Edit > Delete run the event display's commands;
Undo and Redo are disabled and enabled as the history changes, and a disabled item ignores clicks; `canRun` and
`enableWhen`; the menu bar hidden from Set up Window Layout and shown with `window.menuBar`; and File > Quit quits.
