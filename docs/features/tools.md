# Project window tools

The tools of the project window's toolbar (#86, reference manual pp. 57-65): what a press in the event display does.
The code is in `src/ui/eventtools.cpp` (the gestures, the selection range, the tool modifiers) and
`src/ui/eventdisplaymenus.cpp` (the buttons and menus), on top of the object selection tool in `eventediting.cpp`
([parts-and-events.md](parts-and-events.md)). Every edit goes to the engine as the `event.*`, `midiPart.add` and
`transport.*` commands that MCP and Lua have too; the tool itself, like the selection, is the display's and not in the
project or undo.

## Tools

`EventDisplay::setTool` chooses one, `tool()` reads it; the display's root names it in its attribute `tool`, for MCP's
`ui_dom`. Snapping ([snap.md](snap.md)) applies wherever a tool places something.

| Tool (`tool`) | Click | Drag | Double click | Reference |
|---|---|---|---|---|
| Object Selection (`objectSelection`) | Selects ([parts-and-events.md](parts-and-events.md)) | Moves, copies, resizes; from an empty spot a rectangle that selects what it touches (Ctrl or Shift: adds it) | Adds a part on an empty MIDI lane | pp. 253, 256-270 |
| Range Selection (`rangeSelection`) | A range of no length there | A range from where it started, across the lanes it passes | The range of the event, or of the gap between two events; with Shift added to the range | pp. 285-287 |
| Draw (`draw`) | The bar there as an empty MIDI part | An empty MIDI part from where it started, shown while dragging | | p. 250 |
| Erase (`erase`) | Deletes the event | | | p. 255 |
| Split (`split`) | Splits the event there | | | p. 268 |
| Glue (`glue`) | Glues the event to the next one on its track | | | p. 269 |
| Mute (`mute`) | Mutes or unmutes the event | A rectangle that mutes what it touches | | p. 277 |
| Zoom (`zoom`) | Zooms in by 2 around the click; Alt zooms out | A rectangle whose stretch of time it zooms to | Undoes the zoom before it | pp. 101-103 |
| Line (`line`) | | | | p. 873 |
| Play (`play`) | Plays from there until the mouse is released | | | p. 252 |

- **Modes.** The object selection tool sizes normally or with sizing moves contents (`setSizingMovesContents`);
  sizing applies time stretch waits for time stretching (M11). The line tool has the modes line, parabola, sine,
  triangle and square (`setLineMode`, the root's attribute `line-mode`); it draws on automation tracks, which the event
  display shows with #121, and does nothing on parts and events until then.
- **Draw** makes MIDI parts on MIDI and instrument tracks; on audio tracks it waits for audio parts. A drag's ends snap.
- **Play** locates the project cursor and plays the whole project until the mouse is released; the reference plays
  only the pressed track, through the control room, which waits for auditioning in the engine.
- **Cross-hair cursor.** It shows with the tools that use it (object selection, range selection, draw, split, line),
  not with the others (p. 110).
- **Selection range.** `range()` is the range: start and end in project samples and the tracks it covers by id, also
  in the root's attributes `range` (`"start end"`) and `range-tracks`, and drawn as `.range-selection` (`.shown` while
  there is one). Its edges snap. Choosing the range tool selects no events; choosing another tool selects the events in the range instead and ends
  it (p. 286). Editing ranges comes with #88, which reads `range()`.
- **Combine selection tools** (`setCombineSelectionTools`, p. 253): the object selection tool selects ranges in a
  lane's upper half and events in its lower half.
- **The rectangle** that the object selection, mute and zoom tools drag, and the part that the draw tool drags, show
  as `.tool-rectangle` (`.shown` while dragging). The zoom tool's rectangle zooms across only, the reference's
  standard mode; zooming the track heights with it waits for the track list (#81).

## Tool modifiers

The modifier keys that change what a tool does (p. 1601) are the user setting `ui.toolModifiers`, by action, each
`ctrl` (Cmd on macOS), `alt` and `shift` joined with `+`, or empty for none; what the setting leaves out has its
default. A modifier applies when exactly its keys are held. The display reads the setting when it starts and follows
`settings.changed`, so a change from MCP or Lua applies at once.

| Action | Default | Does |
|---|---|---|
| `dragDrop.copy` | Alt | A drag of events copies them |
| `dragDrop.sharedCopy` | Alt+Shift | A drag of MIDI parts makes shared copies |
| `select.splitEvent` | Alt | A click with the object selection tool splits the event |
| `select.setCursorPosition` | Alt+Shift | A click with the object selection tool moves the project cursor there |
| `sizeObjects.disableSnapping` | Ctrl | Resizing at an event's edge doesn't snap |
| `split.splitRepeated` | Alt | The split tool cuts the event into pieces as long as from its start to the click |
| `glue.glueAllFollowing` | Alt | The glue tool glues every following event on the track |
| `erase.deleteLaterEvents` | Alt | The erase tool deletes every later event on the track too |
| `range.selectFullVertical` | Alt+Shift | A range takes every track |

The reference's other modifiers belong to features that come later (slip contents, audio segments, time warp, trim,
the info line).

## Toolbar and keys

`resources/ui/project.rml` has a button per tool, `button[tool=<name>]`, checked for the tool chosen, and the toggle
`#combine-selection-tools`. A button with `tool-menu` gets that menu as its `menu` once its tool is chosen, so the
first click chooses the tool and the next opens its modes (p. 265): `#object-selection-menu` with the sizing modes and
`#line-menu` with the line modes, each with the mode checked.

The commands are in `EventDisplay::actions()`, and `app::AppUi` binds the reference's keys (p. 1569) in the project
window: `tool.objectSelection` 1, `tool.rangeSelection` 2, `tool.split` 3, `tool.glue` 4, `tool.erase` 5, `tool.zoom`
6, `tool.mute` 7, `tool.draw` 8, `tool.play` 9, `tool.line` (no key), `tool.previous` F9, `tool.next` F10 (along the
toolbar, around at the ends) and `tool.combineSelectionTools` Alt+Shift+1.

Not yet: the toolbox on a right click, tool cursors, the time warp, comp and color tools, the drumstick tool and
scrubbing.

## Checks

- `tests/eventediting_tests.cpp` (fake window system, real engine), a test per tool: the tool commands, next and
  previous around the ends, the root's attribute and the cross-hair cursor; the object selection tool's rectangle,
  with Ctrl, and setting the cursor; splitting, snapped, into equal pieces, and with a tool modifier changed through
  `settings.set`; gluing to the next and to all following events; erasing one and all later events; muting and
  unmuting with clicks and a rectangle; zooming in, out, to a rectangle and back with a double click; drawing parts,
  snapped, by dragging and with a click, none on audio tracks or with the line tool; playing on the dummy device while
  the mouse is pressed; ranges by dragging, clicking and double clicking on events and gaps, with Shift, snapped and on
  every track, and the events they select when another tool takes over; the combined selection tools.
- `tests/uiapp_tests.cpp`: the toolbar's tool buttons, the keys 1 to 9, F9, F10 and Alt+Shift+1 in the app's project
  window, the object selection tool's and the line tool's mode menus on a second click; the split tool and the range
  tool driven with the mouse as MCP's UI tools drive it. The project window's golden image shows the toolbar.
