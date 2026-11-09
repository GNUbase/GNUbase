# List editor

The list editor (#130, reference manual pp. 1243-1258): the MIDI editors' second content
([midi-editors.md](midi-editors.md)), beside the key editor ([key-editor.md](key-editor.md)). It lists every event of
the editor's parts in playback order and edits their values as numbers, SysEx messages too, with the filters bar, the
event display and the value display.

## In the engine

An editor view's `content` says what shows its parts: `key`, the key editor, or `list`, the list editor. `editor.open`
takes it (an open editor keeps its content unless it is given, a closed one opens with the key editor) and so does
`editor.set`; `editor.close` puts it back to `key`. The list editor selects notes and controller events: notes in the
view's `selection`, as the key editor does, and controller events in `controllerSelection`, each `{part, controller}`
of the editable parts. Removed events leave it, and closing the editor clears it. Like the rest of a view it is view
state: no undo steps, not saved.

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
                                    outside the part editing mode): span.list-type, textdisplay.list-cell[field=start|
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
| Program Change | program number | | | program number | |
| Aftertouch | pressure 0-127 | | | pressure | |
| Poly Pressure | pitch | pressure | | pressure | |
| Pitchbend | bend -8192 to 8191 | | | bend | |
| SysEx | | | | | the bytes in hexadecimal |

Start, end and length are in bars and beats; end and length only for notes. The channel is 1-16, SysEx has none.

- **Event list** (pp. 1251-1252, 1254). A click on a row selects its event, Ctrl (Cmd) or Shift and a click adds or
  removes it, a click in the empty rest deselects; a click also activates the event's part, as `MidiEditor` handles
  clicks on `[part]`. A double click on a value edits it. A value typed for a selected event changes every selected
  event that has it by as much (the subtype and the values only those of the event's type); with Ctrl (Cmd) held
  with Enter, they all take the value. A pitch can be typed as a name. A changed start moves the event, and the list
  sorts it again. SysEx bytes are typed in hexadecimal, with or without the F0 and F7 around them (p. 1255).
- **Filters bar** (pp. 1250, 1254). A click on a type hides or shows it; with Ctrl (Cmd) it shows that type alone, and
  a second time all of them. The Focus menu shows only events of the first selected event's type (Event Types), also
  of its pitch or controller number (Event Types and Subtype), or of its channel (Event Channels).
- **Event display** (pp. 1252, 1254-1255). Object Selection: a click selects as in the list, a drag moves the selected
  events by as far as the event dragged went, snapped to the quantize grid. Draw: a click draws an event of the Insert
  Event Type at the snapped position into the editable part there, the active one first: a note on C3 of the length
  quantize's length (a drag a multiple of it) with the insert velocity, a controller event 1 (modulation) or poly
  pressure on C3 at 64, aftertouch at 64, pitch bend in the middle, program 0, an empty SysEx message. Erase: a click
  deletes the event, also on its row.
- **Value display** (pp. 1252, 1255): a press and a drag set the value of every event they cross from the mouse's
  place in the bar, in one undo step.
- **Gestures** (#722, #835). A move, a draw or a value display drag ends where the left button goes up, also outside
  the list editor: the editor hears the mouseup in its whole document while the gesture goes on. Past the window's
  edge RmlUi has no element to send the mouseup to, but while a gesture goes on the editor's body is an RmlUi drag
  element (`drag: drag`, the class `in-gesture`), and a drag's end comes wherever the button goes up: a gesture dragged
  out of the window ends there, with its effect. Only then, since RmlUi doesn't scroll on a touch move during a drag:
  a finger pan from where a press starts no gesture, such as the type column, scrolls the list.
  A draw released outside the event display draws where it was pressed. One whose end never comes ends without its
  effect at the next left press anywhere in that document, which the editor hears in the capture phase, before
  anything else; the value display shows the events' values again at once. Other buttons don't end a gesture.
- **Open List Editor** (`editor.openList`, Mod+G in the project window and Edit > Open List Editor, p. 1243) opens the
  selected MIDI parts in the editor window's list editor. Open in Lower Zone and Open in Separate Window keep the
  content.

## Not yet

The MIDI SysEx Editor dialog (address, ASCII, decimal and binary displays, Insert and Delete, importing and exporting
.syx files); the auditioning column and the project cursor; the Focus menu's user presets and Setup (the transform
editor); text, lyrics and SMF events, which the project has no type for; muting events and the mute tool (notes have
no mute yet); Alt-dragging a copy, resizing notes and the selection rectangle in the event display; Delete and
Backspace and the other key commands; the ruler, zooming and scrolling the event display; Set up Window Layout, the
status line, nudging, snap and quantize on the toolbar; editing a program change's bank; a MIDI menu of its own
(Open List Editor is in the Edit menu for now) and windows of their own for the key and list editors.

## Checks

- `tests/listeditor_tests.cpp`: an editor's content, given to `editor.open` and `editor.set`, kept when an open editor
  opens other parts, back to the key editor once closed, and refused when unknown; the selected controller events as
  view state of the editable parts, each once, refused for events that don't exist or parts that aren't editable,
  leaving with removed events, a change of the editing mode and closing.
- `tests/listeditor_ui_tests.cpp` drives the app with `ui::Automation`: Open List Editor (its key and menu item)
  lists a note and a controller event of each type in playback order, with their columns, the events at their time
  and the velocity as a bar, and once closed Open/Close Editor opens the key editor; typing values for two selected
  notes keeps their distance, with Ctrl sets the same value, a pitch typed as a name, a start that sorts the list
  again, a pitch bend and a SysEx message typed in hexadecimal; the filters hiding a type, Ctrl showing one type and
  all again, and the Focus menu's subtype and channel; the draw tool drawing a controller event and a note, the erase
  tool, moving a note in the event display and a drag in the value display; a move, a draw and a value display drag
  released outside the list editor and past the window's edge, each with its effect, also a move with a Focus filter
  on, and ones whose mouseup never comes, ended by the next press without it; a touch pan over the type column
  scrolling the list; a draw dragged in the event display being a multiple of the length quantize's length.
