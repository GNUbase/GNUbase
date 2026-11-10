# Edit history

The edit history lists the undo steps and jumps to any of them (#95, reference manual pp. 110–111). The reference
shows it in a dialog from Edit > History; GNUbase shows it as the panel History of the project window's right zone
([docking.md](docking.md)), so it can also open in a window of its own. The code is `ui::HistoryPanel`
(`src/ui/historypanel.h`), which `app::AppUi` gives the docking system.

- **Opening it.** The UI command `window.history` shows the panel as the right zone's active tab and shows the zone;
  a panel in a window of its own stays there, and its window comes to the front with the focus, restored if it is
  minimized ([docking.md](docking.md)). The reference has no key for it; the main menu's Edit menu will run the
  command.
- **What it shows.** A table (a `<listview>`, [widgets.md](widgets.md)) with a row for the initial state, then a row per
  step from `history.get`: Action, Time (the local time of day, from the step's UTC time), State (Done or Undone) and
  Details. It follows `history.changed`, so undo and redo from MCP, Lua or key commands show at once, and a new or
  loaded project, which clears the history, empties it.
- **Separator.** A separator follows the row of the last done step (p. 111), and the rows of undone steps after it have
  the state class `.undone`, which the themes dim, their Details fields too. The rows are in two tables, done and
  undone, with the separator between them: the panel moves rows from one to the other when the position changes, so the
  separator needs no geometry from code, and RmlUi gives both tables the same columns from their first rows. The list
  view finds the rows of both tables in the document's order ([widgets.md](widgets.md)). The separator is a thin line
  (`.history-separator`, which the themes color) in a grip (`.history-grip`) as tall as a splitter, which reaches over
  the rows next to it and is in front of them, so that it is easy to grab.
- **Jumping.** Dragging the separator up undoes and dragging it down redoes. While it is dragged it moves to the row
  boundary nearest the mouse, measured from where the rows were when the drag started, and the rows show Done or Undone
  as they will be; one `history.undo` or `history.redo` with `steps` goes to the engine when the drag ends, and none if
  it ends where it started or if the steps changed under it (another client added one, so the rows were made again). The
  rows' places are measured in the list's content, so the separator follows them when the wheel scrolls the list during
  a drag. Held above or below the list, the mouse scrolls it (the list view's `dragscroll`, [widgets.md](widgets.md)),
  faster the further it is past the edge, so the separator reaches rows that a long history has outside the panel:
  dropped above the list once it shows its first row, the drag undoes everything, and dropped below the last row, it
  redoes everything. If RmlUi drops the drag without its end, as it does when the docking moves the panel during the
  drag, the list loses the focus that the press on the separator gave it (the grip takes none): the drag is over,
  nothing is sent, and the rows show the steps as they are again. The row of the last done step is also selected:
  selecting another row, with a click or the arrow keys, undoes the steps after it or redoes the steps up to it. The
  initial state's row undoes everything.
- **Details.** Each step's Details field is a note for the user: what is typed in it becomes the step's details
  (`history.describe`) with Enter or when the field loses the focus. Clicks and keys in the field don't select its row,
  and the field keeps the focus and what was typed so far when another client's undo or redo moves its row across the
  separator. What is typed belongs to its step, whatever another client does meanwhile: when a new action adds a step,
  the text, the caret and the focus go on in the field of that step, also when the rows are made again or the oldest
  step went past the maximum number of steps, so that the step is a row higher; when the step itself is gone (a new
  action drops the undone steps, or the step was the oldest; a gesture that ends where it started removes its step),
  what was typed goes with it and the list gets the focus. A gesture that joins the last step (below) keeps that step.
  Nothing is sent before Enter or the user moving the focus. `history.get` has no step ids, so the panel recognizes a
  step by its action and time and by where it is: steps only go at the end (the undone ones) and, at the maximum, at the
  start. At the maximum, a run of steps of the same action within one second can't be told apart, and Enter racing
  another client's action sends the note by the step's index; step ids in `history.get` and `history.describe` would
  settle both.
- **Gestures.** A `history.transaction` with a `merge` key joins the last step instead of adding one when that step is a
  transaction with the same key and nothing was undone since; a gesture that ends where it started leaves no step. The
  UI makes each gesture on a value widget one step this way (`EngineBinding::sendStep`, with the gesture number of the
  widget's `change`, [widgets.md](widgets.md)): a drag of the mixer's faders, of the inspector's volume and pan and of
  the info line's volume ([inspector.md](inspector.md)), wheel turns until the mouse leaves, an arrow key until it is
  released. The steps of a gesture still go to the engine one by one, so that it is heard while it lasts.
- **Joining steps.** Each step has an id in `history.get`, growing with each new step. `history.join` with `after`, an
  id, and `action` joins the done steps whose id is greater into one step named action, so that a client's several
  commands are undone and redone at once: a user script is one undo step this way ([scripts.md](scripts.md)). Steps
  that end where they started leave none, and steps that can be redone stay.
- **Marker activation.** A `history.transaction` announces `marker.trackActivated` once after all its commands, only
  if the final active marker track differs from the initial one. Intermediate activations are not announced, so MCP,
  Lua and UI clients can remove marker tracks in any order. `tests/mcpevents_tests.cpp` covers removals through MCP,
  switching back to the initial track without an event, and a failed transaction without events (#983).
- **Through the engine.** Everything the panel does goes through these engine commands, so MCP and Lua clients jump with
  `history.undo` and `history.redo` by the difference between the position they want and `history.get`'s.
- **Not yet:** the maximum number of undo steps in the preferences (p. 112), which `history.set` already sets; and a
  wider default for the panel, whose zone the user sizes with its splitter.
- **Tests:** `tests/history_tests.cpp` checks merged transactions: one undo step per key, none after another step or an
  undo, and none for a gesture that ends where it started; and joined steps: one undo and one redo for three steps,
  none for steps that end where they started, undone steps kept, and the steps found by id after the oldest were
  dropped. `tests/historypanel_tests.cpp` opens the panel through its UI
  command in the app's project window and drives it with `ui::Automation`: it lists the steps with their action, time
  and state with the separator after the done ones, clicks undo several steps, undo everything and redo some again,
  typing details reaches the engine without moving the position, undo, redo and details from another client show, and a
  new project empties it. Dragging the separator up and down undoes and redoes with one command each, checked against
  `history.get` and `history.changed`, also dropped above the list and below the last row, and a drag that ends where it
  started sends none; it is grabbed just above and just below its line, over the rows. Driving the window's context
  directly, the rows show where the separator goes before the button is released, and a drag the steps changed under
  sends nothing; with 60 steps, the separator follows the wheel during a drag, and the mouse held below and above the
  list scrolls it to the last step and the initial state; and a drag the docking drops by moving the panel's tab puts
  the rows back and sends nothing, with the focus taken from the list and never on the grip. Typing details while
  another client undoes and redoes keeps the focus and sends the whole text once; at position 1 of 3, a note for step 2
  isn't sent when another client's new step takes that step's place, a note for a step that stays goes on in its field
  when the rows are made again, at the maximum number of steps it moves up a row with its step, and a note for the
  oldest step goes with it when a new step drops it. `tests/uithemes_tests.cpp` checks that every theme dims the undone
  rows and their details and draws the separator.
