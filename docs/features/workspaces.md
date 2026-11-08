# Workspaces

Named window layouts (#163, reference manual pp. 1576–1579): which windows are open, where they are and on which
monitor, and how their zones, tabs, strips and sections are set up. Global workspaces are for every project, project
workspaces are saved with the project. The setup panes (p. 1575), with their presets, Show All and Reset All, are in
[docking.md](docking.md), profiles and the safe start mode in [profiles.md](profiles.md).

## The layout

A workspace is a copy of the user setting `ui.layout` ([docking.md](docking.md), "The layout and the engine"), which
now also keeps the app's top-level windows under `windows`, by name, next to the docking windows' zones:

```json
{
  "windows": {
    "project": {"bounds": {"x": 0, "y": 0, "width": 1920, "height": 1040}, "monitor": "DELL U2720Q"},
    "mixer1": {"open": true, "bounds": {"x": 1920, "y": 0, "width": 1000, "height": 560}, "monitor": "LG HDR 4K",
      "sections": {"channel": {"hidden": ["notes"]}}}
  },
  "panels": {"pool": {"floating": true, "bounds": {"x": 200, "y": 150, "width": 800, "height": 500}, "monitor": "DELL U2720Q"}}
}
```

- **Windows** (`ui::Docking::addWindow`): `project` (the main window), `mixer1` to `mixer4`, `markers`,
  `onScreenKeyboard`, `tempoTrack`, `arranger`, `transportPanel` and `timeDisplay` have `open`, `bounds` and
  `monitor`; `channelSettings`, `editor` and `sampleEditor`, which open and close with what they show, have only
  `bounds` and `monitor`. A window's entry gets its bounds and its monitor's name (the monitor it overlaps most) once
  it opens, moves or resizes, checked twice a second and at once when a window opens or closes through its command,
  and `open` when that changes. A window that never moved adds nothing, so the layout keeps only what the user did.
  When GNUbase starts, a window that `ui.layout` from the last session has open but that is closed gets `open: false`
  at once (and the other way round), so that a workspace added then has the windows as they are.
  Panels in windows of their own keep their monitor too.
- **Opening.** Each window opens where its entry says, and the main window goes there when GNUbase starts. A window
  whose monitor, by name, is connected goes where it was (moved onto a monitor if it can't be reached, as for every
  window, architecture.md, "UI shell"); one whose monitor isn't connected goes onto the primary monitor, centred and
  fully visible, shrunk to fit (`ui::restore` in `src/ui/displays.h`).
- **Another client's layout**, such as a workspace's, applies at once: windows open or close as `open` says (missing
  is closed), and open windows move to their bounds as above. The docking system tells its own saves of `ui.layout`
  by their request ids; a layout from anyone else wins, and if one of its own saves was still on its way, it saves
  the new layout again, so that it comes last.

## In the engine

`src/engine/document/workspaces.cpp`. Global workspaces are the user setting `ui.workspaces`, a list of
`{name, layout}`, changed in the settings file as it is (`Engine::modifySetting`), so another GNUbase process's
workspaces stay. Project workspaces and the active workspace are settings of the project, not undo steps, saved in
GNUbase's extra entry (`workspaces` in `project.get`), none for files from before.

| Command | What it does |
|---|---|
| `workspace.get` | `global`, `project` (each a list of `{name, layout}`) and `active` (`{type, name}`, type empty for none) |
| `workspace.add` | Adds a workspace of a type with `ui.layout` as it is now, after the others, and makes it active (Add Workspace, p. 1577) |
| `workspace.update` | Gives the active workspace `ui.layout` as it is now (Update Workspace, p. 1578) |
| `workspace.activate` | Sets `ui.layout` to a workspace's layout and makes it active; type `""` is No Workspace (p. 1577) |
| `workspace.remove`, `workspace.rename`, `workspace.move` | The organizer's Delete, rename, Move Up and Move Down (p. 1578) |

Names are unique and not empty within a type. Each command emits `workspace.changed` with what `workspace.get`
returns.

## The UI

- **Workspaces menu** (`resources/ui/project.rml`, `ui::WorkspacesMenu`): Add Workspace... (`window.addWorkspace`),
  Update Workspace (`workspace.update`, disabled without an active workspace) and Organize... (`window.workspaces`),
  then No Workspace and the workspaces, global ones marked G, then project ones marked P, the active one checked
  (`menuitem[workspace-type][workspace]`). Choosing one activates it.
- **New Workspace dialog** (`resources/ui/add-workspace.rml`, p. 1577): a name and Global or Project Workspace; a name
  that is taken shows the engine's error and keeps the dialog open.
- **Workspaces Organizer** (`resources/ui/workspaces.rml`, p. 1578): the global and the project workspaces in two
  lists; the name field renames the selected one with Return, Add opens the New Workspace dialog, Delete, Move Up and
  Move Down. It follows `workspace.changed`.
- **Windows dialog** (Window > Windows..., `window.windows`, `resources/ui/windows.rml`, pp. 1583–1584): GNUbase's
  open windows by their documents' titles; Mode chooses the selected window or all of them, and Activate (the selected
  one), Minimize, Restore and Close Windows act on them through the UI tools (`ui_window`, `ui_close`; Lua
  `gnubase.ui.window`, `gnubase.ui.close`), which MCP clients use the same way. Close Windows with All closes every
  window but the project window. `ui_window` minimizes and restores through the window system
  (`WindowSystem::minimize`, `restore`).

## Not yet

Key commands for the workspaces (the Workspace category of the key commands dialog, and the organizer's key column);
dragging workspaces in the organizer and renaming with a double click; locking workspaces; Open Projects in Last Used
View (p. 1609) and the view of external projects, which come with the preferences' General page; the editors'
windows opening from a workspace, which follow what the engine's editor views show; and the Windows dialog's Cascaded
mode, which acts on a window's associated windows, and Reset Layout. The toolbar's and the info line's setup panes
(p. 1575) come later (#942).

## Checks

- `tests/workspaces_tests.cpp`: workspaces added with `ui.layout` as it is, names unique within a type, activated,
  updated, No Workspace, renamed, moved and deleted with the active one following; project workspaces saved and loaded
  with the active one, global ones in `ui.workspaces` for every project, neither an undo step.
- `tests/workspaces_ui_tests.cpp`: on the fake window system with two monitors, windows that never moved add nothing,
  a window that the last session's layout has open but that is closed at the start not open in a new workspace,
  the main window's and a tool window's bounds and monitors kept, the tool window opened and closed by a workspace,
  and recalled where they were with both monitors, or without the right one fully visible on the left; in the app, the
  Workspaces menu with No Workspace and Update Workspace disabled, workspaces added through the New Workspace dialog,
  the mixer window and the lower zone recalled from the menu with the active one checked, Update Workspace, and the
  organizer renaming with Return, moving and deleting; the Windows dialog listing the project, markers and tempo track
  windows, activating and closing the selected one, and closing all but the project window.
- `tests/uitools_tests.cpp`: `ui_window` minimizing, restoring and activating a window, on the fake window system.
