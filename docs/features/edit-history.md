# Edit history

The edit history lists the undo steps and jumps to any of them (#95, reference manual pp. 110–111). The reference
shows it in a dialog from Edit > History; GNUbase shows it as the panel History of the project window's right zone
([docking.md](docking.md)), so it can also open in a window of its own. The code is `ui::HistoryPanel`
(`src/ui/historypanel.h`), which `app::AppUi` gives the docking system.

- **Opening it.** The UI command `window.history` shows the panel as the right zone's active tab and shows the zone;
  a panel in a window of its own stays there. The reference has no key for it; the main menu's Edit menu will run the
  command.
- **What it shows.** A table (a `<listview>`, [widgets.md](widgets.md)) with a row for the initial state, then a row
  per step from `history.get`: Action, Time (the local time of day, from the step's UTC time), State (Done or Undone)
  and Details. Rows of undone steps have the state class `.undone`. It follows `history.changed`, so undo and redo
  from MCP, Lua or key commands show at once, and a new or loaded project, which clears the history, empties it.
- **Jumping.** The row of the last done step is selected, where the reference draws its separator. Selecting another
  row, with a click or the arrow keys, undoes the steps after it (`history.undo` with `steps`) or redoes the steps up
  to it (`history.redo`). The initial state's row undoes everything.
- **Details.** Each step's Details field is a note for the user: what is typed in it becomes the step's details
  (`history.describe`) with Enter or when the field loses the focus. Clicks and keys in the field don't select its
  row.
- **Through the engine.** Everything the panel does goes through these engine commands, so MCP and Lua clients jump
  with `history.undo` and `history.redo` by the difference between the position they want and `history.get`'s.
- **Not yet:** dragging the separator, as in the reference; the maximum number of undo steps in the preferences
  (p. 112), which `history.set` already sets; and a wider default for the panel, whose zone the user sizes with its
  splitter.
- **Tests:** `tests/historypanel_tests.cpp` opens the panel through its UI command in the app's project window and
  drives it with `ui::Automation`: it lists the steps with their action, time and state, clicks undo several steps,
  undo everything and redo some again, typing details reaches the engine without moving the position, undo, redo and
  details from another client show, and a new project empties it.
