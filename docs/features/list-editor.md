# List editor

The list editor (#130, reference manual pp. 1243-1258): the MIDI editors' second content
([midi-editors.md](midi-editors.md)), beside the key editor ([key-editor.md](key-editor.md)). It lists every event of
the editor's parts in playback order and edits their values as numbers, SysEx messages too, with the filters bar, the
event display and the value display.

## In the engine

An editor view's `content` says what shows its parts: `key`, the key editor, or `list`, the list editor. `editor.open`
takes it (an open editor keeps its content unless it is given, a closed one opens with the key editor, also when
`editor.set` gave it another content while it was closed) and so does `editor.set`; `editor.close` puts it back to
`key`. The list editor selects notes and controller events: notes in the view's `selection`, as the key editor does, and
controller events in `controllerSelection`, each `{part, controller}` of the editable parts. Removed events leave it,
and closing the editor clears it. Like the rest of a view it is view state: no undo steps, not saved.

Edits are the existing commands: `note.add`, `note.set`, `note.remove`, `controller.add`, `controller.set` and
`controller.remove` (SysEx with its whole message as `data`).

## In the UI

`ui::ListEditor` (`src/ui/listeditor.h`) fills the editor of a `ui::MidiEditor` while its view's content is `list`;
`MidiEditor` swaps it with the key editor when the content changes. It shows the project and the editor view as
`MidiEditor` hands them on and sends `note.*`, `controller.*` and `editor.set`; the edits of several parts go in one
`history.transaction`.

```
.list-editor[tool=objectSelection|draw|erase]
  .toolbar.list-editor-toolbar
    button.list-tool[list-tool]     the tools; .checked for the current one
    #list-insert-type               Insert Event Type, menu #list-insert-type-menu (menuitem[insert-type])
    #list-length-quantize           Length Quantize, menu #list-length-menu, the view's lengthQuantize
    valuefield#list-insert-velocity Note Insert Velocity, the view's insertVelocity
  .toolbar.list-editor-filters
    #list-focus                     the Focus menu #list-focus-menu (menuitem[focus=none|type|subtype|channel])
    button.list-filter[event-type]  the event type options; .checked while the type is hidden
  .list-row.list-editor-header      the columns' names
  .list-editor-body                 scrolls vertically
    .list-editor-rows               a .list-row[part][note|controller][type] per event shown (.selected, .inactive
                                    outside the part editing mode, .at-cursor for the auditioning column's arrow):
                                    span.list-audition with svg.list-audition-arrow, span.list-type,
                                    textdisplay.list-cell[field=start|
                                    end|length|subtype|value|secondary|channel|attributes] (disabled where the type
                                    has no such value), .list-event-lane with the .list-event, .list-value-lane with
                                    the .list-value bar
    .list-row.list-filler           the empty rest of the event display, where drawing can start
```

Code places the events in percent of the event display, which fits the parts' range into its width, and sets the
value bars' width; `listeditor.rcss` lays the columns out and the themes color the states.

| Type | Subtype | Main Value | Secondary Value | Value display | Attributes |
|---|---|---|---|---|---|
| Note | pitch (C3) | velocity | release velocity | velocity | |
| Controller | controller number | value 0-127 | | value | |
| Program Change | program number | bank 0-16383, empty for none (#719) | | program number | |
| Aftertouch | pressure 0-127 | | | pressure | |
| Poly Pressure | pitch | pressure | | pressure | |
| Pitchbend | bend -8192 to 8191 | | | bend | |
| SysEx | | | | | the bytes in hexadecimal |
| Articulation | | | | | the direction's name, with (Reset) for a reset |

Start, end and length are in bars and beats; end and length only for notes. The channel is 1-16, SysEx and
directions (Articulation, [articulation-maps.md](articulation-maps.md)) have none. A program change's bank select,
which the reference manual's list leaves out (p. 1253), is in its Main Value: typed for a selected one, every selected
program change takes it, by as much where both have a bank, with Ctrl (Cmd) the same, at most 16383; typed empty, the
banks go. A note's Attributes are its
attributes of the track's articulation map, names separated by commas; typed, they replace them, and typed empty, they
go (p. 1291). A direction's name can be typed there too; the Insert Event Type menu has no Articulation, as a
direction needs a name: the Articulations lane draws them. A click on the column opens the pop-up menu of the map's
attributes or directions under it (#1658, p. 1291), a right click at the mouse, and a warning sign shows names the map
doesn't have (#1580).

- **Event list** (pp. 1251-1252, 1254). A click on a row selects its event, Ctrl (Cmd) or Shift and a click adds or
  removes it, a click in the empty rest deselects; a click also activates the event's part, as `MidiEditor` handles
  clicks on `[part]`. While a selection the list sent is unanswered, a view older than it (such as the `editor.changed`
  of the click before a Shift-click) leaves the selection as clicked, as in the key editor (#1817), so that a move right
  after it takes every event the clicks selected. A double click on a value edits it. A value typed for a selected event
  changes every selected event that has it by as much (the subtype and the values only those of the event's type); with
  Ctrl (Cmd) held with Enter, they all take the value. A pitch can be typed as a name, in either case, and spaces around
  a typed value don't count; a typed velocity is at least 1, as the value display sets it. A changed start moves the
  event, and the list sorts it again. SysEx bytes are typed in hexadecimal, with or without the F0 and F7 around them
  (p. 1255).
- **Auditioning column** (p. 1251, #719). Its arrow points at the last event shown that starts at the project cursor or
  before it, also while playing: the list editor follows `transport.changed` and, while playing, the playback position
  (`Engine::playbackPosition`, which `app::AppUi` hands the editors), and moves the arrow only when it points at
  another row. A click there moves the cursor to the event's start (`transport.locate`), a double click also starts or
  stops playback (`transport.toggle`); neither selects, draws or erases.
- **Filters bar** (pp. 1250, 1254). A click on a type hides or shows it; with Ctrl (Cmd) it shows that type alone, and
  a second time all of them. The Focus menu shows only events of the first selected event's type (Event Types), also
  of its pitch or controller number (Event Types and Subtype), or of its channel (Event Channels); SysEx and directions
  have no channel, so with one of them first Event Channels hides nothing.
- **Event display** (pp. 1252, 1254-1255). Object Selection: a click selects as in the list, a drag moves the selected
  events by as far as the event dragged went, snapped to the quantize grid; with Alt (Opt) held at the press it copies
  them there instead, each with all its values, in one undo step; a drag at a note's end (its last 6 dp) gives the
  selected notes as much more or less length, snapped, but at least a tick. A click without a drag only selects, also on
  an event or a note's end off the grid. Draw: a click draws an event of the Insert Event Type at the snapped position
  into the editable part there, the active one first: a note on C3 of the length quantize's length (a drag a multiple of
  it) with the insert velocity, a controller event 1 (modulation) or poly pressure on C3 at 64, aftertouch at 64, pitch
  bend in the middle, program 0, an empty SysEx message. Erase: a click deletes the event, also on its row.
- **Delete and Backspace** (pp. 1255, 1258) delete the selected events, those of several parts in one undo step, while
  the list, the event display or the value display has the keyboard focus, as a press there gives it: the list editor
  takes them before the key commands. While a value is typed they edit its text.
- **Value display** (pp. 1252, 1255): a press and a drag set the value of every event they cross from the mouse's
  place in the bar, in one undo step; a velocity is at least 1. While the drag goes on only the bars change, and the
  rows are built once per project and view the editor is given, with the project's time context read once for them.
- **Undo names** (#2138): an edit's undo step is named as the engine names its command, "Change notes" or "Change
  controllers" for typed values, a value display drag, a move and a resize, "Add notes" or "Add controllers" for a copy,
  "Remove notes" or "Remove controllers" for Delete and Backspace, and "Change events", "Add events" or "Remove events"
  when notes and controller events go together. A lone command still goes on its own, so that an edit that changes
  nothing counts as no change; several, for events of several parts or notes and controller events together, go in one
  `history.transaction` of that name, so a gesture keeps its name however many commands it sends.
- **Gestures** (#722, #835). A move, a draw or a value display drag ends where the left button goes up, also outside
  the list editor: the editor hears the mouseup in its whole document while the gesture goes on. Past the window's
  edge RmlUi has no element to send the mouseup to, but while a gesture goes on the editor's body is an RmlUi drag
  element (`drag: drag`, the class `in-gesture`), and a drag's end comes wherever the button goes up: a gesture dragged
  out of the window ends there, with its effect. Only then, since RmlUi doesn't scroll on a touch move during a drag:
  a finger pan from where a press starts no gesture, such as the type column, scrolls the list, also when that press
  ends a gesture whose end never came or follows a mouseup within the same frame (#1012).
  A draw released outside the event display draws where it was pressed. A gesture whose drag never started, as when
  the window loses the focus before the mouse moved, gets no dragend: it ends without its effect as the mouse leaves
  the window (the document's mouseout), so a value display drag doesn't follow the mouse back (#1756). One whose end
  never comes at all ends without its effect at the next left press anywhere in that document, which the editor hears
  in the capture phase, before anything else; the value display shows the events' values again at once. Other
  buttons don't end a gesture.
- **Open List Editor** (`editor.openList`, Mod+G in the project window and MIDI > Open List Editor, p. 1243) opens the
  selected MIDI parts in the editor window's list editor. Open in Lower Zone and Open in Separate Window keep the
  content.

## Not yet

The setup panes of the toolbar and the info line (p. 1575), which only the project window has so far
([docking.md](docking.md)); the MIDI SysEx Editor dialog (address, ASCII, decimal and binary displays, Insert and
Delete, importing and exporting .syx files); the project cursor in the event display (p. 1245); the Focus menu's user
presets and Setup (the transform editor); text, lyrics and SMF events, which the project has no type for; muting events
and the mute tool (notes have no mute yet); the selection rectangle in the event display; key commands other than Delete
and Backspace; the ruler, zooming and scrolling the event display; Set up Window Layout, the status line, nudging, snap
and quantize on the toolbar; and windows of their own for the key and list editors,
which share the editor window.

## Checks

- `tests/listeditor_tests.cpp`: an editor's content, given to `editor.open` and `editor.set`, kept when an open editor
  opens other parts, back to the key editor once closed, also after `editor.set` gave the closed editor one, and refused
  when unknown; the selected controller events as view state of the editable parts, each once, refused for events that
  don't exist or parts that aren't editable, leaving with removed events, a change of the editing mode and closing.
- `tests/listeditor_ui_tests.cpp` drives the app with `ui::Automation`: Open List Editor (its key and menu item) lists a
  note and a controller event of each type in playback order, with their columns, the events at their time and the
  velocity as a bar, and once closed Open/Close Editor opens the key editor; typing values for two selected notes keeps
  their distance, with Ctrl sets the same value, a pitch typed as a name, a start that sorts the list again, a pitch
  bend and a SysEx message typed in hexadecimal; the filters hiding a type, Ctrl showing one type and all again, and the
  Focus menu's subtype and channel; the draw tool drawing a controller event and a note, the erase tool, moving a note
  in the event display and a drag in the value display; a move, a draw and a value display drag released outside the
  list editor and past the window's edge, each with its effect, also a move with a Focus filter on, and ones whose
  mouseup never comes, ended by the next press without it, and a value display drag whose release was lost ended as the
  mouse leaves the window; a touch pan over the type column scrolling the list; a draw dragged in the event display
  being a multiple of the length quantize's length; a typed end, length, channel and release velocity, a value with
  spaces around it, a pitch name in lower case and a velocity of 0 that stays 1; the Insert Velocity and Length Quantize
  controls and the note the draw tool then draws; a typed value for notes of two parts in one undo step; a value display
  drag across two rows in one undo step that builds no time context while it goes on; a Shift-click's selection staying
  while the view of the click before it comes in, and a press whose selection the engine refuses showing the view's
  selection again; a move of two selected events that would take one before its part's start moving nothing, and one
  that moves both; the erase tool erasing in the event display; Focus on Event Channels with a SysEx event selected;
  Delete deleting a selected note and controller event in one undo step, and Backspace a SysEx message, but not while a
  value is typed; Alt and a drag copying a selected note and controller event in one undo step; a drag at a note's end
  resizing both selected notes in one undo step; typed values, a value display drag, a move, a copy and Delete keeping
  their undo names over notes, controller events and both, of one part and of two, and a resize over notes; a program
  change's bank typed where it has none, for two selected ones keeping their distance, held at 16383, and typed empty
  taking both away in one undo step; a click without a drag at the end of a note off the grid, and an
  Alt-click and a click on the SysEx message off it, changing nothing; the auditioning column's arrow at the event at
  or before the cursor, a click there locating to an event, a double click starting and stopping playback, and the arrow
  following the playback position.
