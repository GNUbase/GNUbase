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
  and Details. It follows `history.changed`, so undo and redo from MCP, Lua or key commands show at once, and a new
  or loaded project, which clears the history, empties it.
- **Separator.** A separator follows the row of the last done step (p. 111), and the rows of undone steps after it have
  the state class `.undone`, which the themes dim, their Details fields too. The rows are in two tables, done and
  undone, with the separator between them: the panel moves rows from one to the other when the position changes, so the
  separator needs no geometry from code, and RmlUi gives both tables the same columns from their first rows.
- **Jumping.** Dragging the separator up undoes and dragging it down redoes. While it is dragged it moves to the row
  boundary nearest the mouse, measured from where the rows were when the drag started, and the rows show Done or Undone
  as they will be; one `history.undo` or `history.redo` with `steps` goes to the engine when the drag ends, and none if
  it ends where it started or if the steps changed under it (another client added one, so the rows were made again). The
  row of the last done step is also selected: selecting another row, with a click or the arrow keys, undoes the steps
  after it or redoes the steps up to it. The initial state's row undoes everything.
- **Details.** Each step's Details field is a note for the user: what is typed in it becomes the step's details
  (`history.describe`) with Enter or when the field loses the focus. Clicks and keys in the field don't select its row,
  and the field keeps the focus and what was typed so far when another client's undo or redo moves its row across the
  separator.
- **Through the engine.** Everything the panel does goes through these engine commands, so MCP and Lua clients jump
  with `history.undo` and `history.redo` by the difference between the position they want and `history.get`'s.
- **Not yet:** the maximum number of undo steps in the preferences (p. 112), which `history.set` already sets; and a
  wider default for the panel, whose zone the user sizes with its splitter.
- **Tests:** `tests/historypanel_tests.cpp` opens the panel through its UI command in the app's project window and
  drives it with `ui::Automation`: it lists the steps with their action, time and state with the separator after the
  done ones, clicks undo several steps, undo everything and redo some again, typing details reaches the engine without
  moving the position, undo, redo and details from another client show, and a new project empties it. Dragging the
  separator up and down undoes and redoes with one command each, checked against `history.get` and `history.changed`,
  and a drag that ends where it started sends none. Driving the window's context directly, the rows show where the
  separator goes before the button is released, and a drag the steps changed under sends nothing. Typing details while
  another client undoes and redoes keeps the focus and sends the whole text once. `tests/uithemes_tests.cpp` checks that
  every theme dims the undone rows and their details and draws the separator.
