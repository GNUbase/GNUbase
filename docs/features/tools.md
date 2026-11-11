# Project window tools

The tools of the project window's toolbar (#86, reference manual pp. 57-65): what a press in the event display does,
the toolbox, the mouse pointers and the notification when a key chooses a tool (#469, pp. 65, 1602).
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
| Comp (`comp`) | Brings the take to the front; with Shift selects it | On a lane of a track whose lanes show: comps the range ([comping.md](comping.md)) | | p. 228 |
| Draw (`draw`) | The bar there as an empty part | An empty part from where it started, shown while dragging | | p. 250 |
| Erase (`erase`) | Deletes the event | | | p. 255 |
| Split (`split`) | Splits the event there | | | p. 268 |
| Glue (`glue`) | Glues the event to the next one on its track | | | p. 269 |
| Mute (`mute`) | Mutes or unmutes the event | A rectangle that mutes what it touches | | p. 277 |
| Zoom (`zoom`) | Zooms in by 2 around the click; Alt zooms out | A rectangle whose stretch of time it zooms to | Undoes the zoom before it; with Alt redoes the zoom undone last | pp. 101-104 |
| Line (`line`) | | | | p. 873 |
| Play (`play`) | Auditions the audio track from there until the mouse is released | Scrub mode: scrubs the track along | | pp. 252-253 |
| Time Warp (`timeWarp`) | Free Warp: a warp marker on the audio event there | Warp Grid: the musical position pressed plays where it is released; Free Warp: a warp marker moved there ([time-warp.md](time-warp.md)) | | pp. 1465-1467 |

- **Modes.** The object selection tool sizes normally or with sizing moves contents (`setSizingMovesContents`);
  sizing applies time stretch waits for time stretching (M11). The line tool has the modes line, parabola, sine,
  triangle and square (`setLineMode`, the root's attribute `line-mode`); it draws on automation tracks
  ([automation-tracks.md](automation-tracks.md)), as do the object selection, draw and erase tools there, and does
  nothing on parts and events. The play tool has the modes play and
  scrub (`setPlayMode`, the root's attribute `play-mode`). The time warp tool has the modes Warp Grid, Warp Grid
  (musical events follow) and Free Warp (`setWarpMode`, the root's attribute `warp-mode`: `warpGrid`,
  `warpGridFollow`, `freeWarp`).
- **Draw** makes MIDI parts on MIDI and instrument tracks and audio parts on audio tracks
  ([audio-parts.md](audio-parts.md)). A drag's ends snap.
- **Play** auditions the pressed audio track from where it presses until the mouse is released, past its channel and
  without the transport (`audition.start`, `audition.stop`, [audition.md](audition.md)). In scrub mode the press and
  every move of the drag scrub the track to the mouse's position (`audition.scrub`), forwards or backwards at the
  mouse's speed, and move the project cursor along. At an audio event's lower left or right corner, the lower half of
  its edge, a drag in scrub mode resizes the event as the object selection tool does (`event.resize`, the resize
  pointer there) and scrubs what the event plays at the edge the release gives it, snapped: its clip at that clip
  frame (`audition.scrub` with `clip`, from the drag's `event.previewResize`), also over audio the event hides yet,
  and without moving the cursor (p. 268); the upper half of the edge scrubs the track, as the rest of the event does.
  A click there without a drag does nothing more: no tool modifier of the object selection tool's clicks. On other
  tracks it does nothing yet.
- **Cross-hair cursor.** It shows with the tools that use it (object selection, range selection, draw, split, line),
  not with the others (p. 110).
- **Selection range.** `range()` is the range: start and end in project samples and the tracks it covers by id, also
  in the root's attributes `range` (`"start end"`) and `range-tracks`, and drawn as `.range-selection` (`.shown` while
  there is one). Its edges snap. With Snap to Zero Crossing on, a range that a drag, a click or a double click made
  then moves its edges to zero crossings of the audio on its tracks (`snap.zeroCrossings`, [snap.md](snap.md), p. 285),
  unless it changed meanwhile. Choosing the range tool selects no events; choosing another tool selects the events in the range instead and ends
  it (p. 286). The edit commands edit the range ([range-editing.md](range-editing.md)).
- **Combine selection tools** (`setCombineSelectionTools`, p. 253): the object selection tool selects ranges in the
  upper half of a lane at least two rows high and events in its lower half; on lower lanes it selects events only. A
  row is the least height of a track, `g_trackRow` (20 dp), and the lane's height counts with vertical zoom.
- **The rectangle** that the object selection, mute and zoom tools drag, and the part that the draw tool drags, show
  as `.tool-rectangle` (`.shown` while dragging). The zoom tool's rectangle zooms across only while the user setting
  `ui.zoomToolHorizontalOnly` is on (Zoom Tool Standard Mode: Horizontal Zooming Only, pp. 101, 1602; default on, on
  the Preferences dialog's Editing - Zoom page). Off, it zooms the track heights too, so that the tracks it spans fill
  the lanes' height, scrolled to its top, in one step of the zoom history. The display follows `settings.changed`.

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
`#combine-selection-tools` with an icon of its own (`tool-combine-selection.svg`). A button with `tool-menu` gets that menu as its `menu` once its tool is chosen, so the
first click chooses the tool and the next opens its modes (p. 265): `#object-selection-menu` with the sizing modes and
`#line-menu` with the line modes and `#play-menu` with the play modes, each with the mode checked. The time warp tool
has no button, which the toolbar has no room for at 1024 pixels: the toolbox and the context menu's Tools choose it,
and the Tools' Time Warp Mode, `#time-warp-menu`, chooses it with a mode.

The commands are in `EventDisplay::actions()`, and `app::AppUi` binds the reference's keys (p. 1569) in the project
window: `tool.objectSelection` 1, `tool.rangeSelection` 2, `tool.split` 3, `tool.glue` 4, `tool.erase` 5, `tool.zoom`
6, `tool.mute` 7, `tool.draw` 8, `tool.play` 9, `tool.line`, `tool.comp` and `tool.timeWarp` (no key), `tool.previous` F9, `tool.next` F10 (along the
toolbar, around at the ends) and `tool.combineSelectionTools` Alt+Shift+1.

A tool chosen with one of these commands, by its key or from Lua's `gnubase.ui.command`, shows its name for a
moment over the event display (p. 1602): `#tool-notification`, a `<notification>` ([widgets.md](widgets.md)), also
"Combine Selection Tools On" or "Off". The user setting `ui.toolSwitchNotification` (default on) switches it off.
The toolbar's buttons and the toolbox show none.

## Toolbox and context menu

A right click in the event lanes opens the context menu `#event-display-menu` or the toolbox `#toolbox` (p. 65), at
the mouse. With the user setting `ui.toolboxOnRightClick` (Show Toolbox on Right-Click, p. 1602; default off) a right
click opens the toolbox and a right click with any modifier key (Ctrl, Alt, Shift, Cmd) the context menu; while it is
off, the other way round. The display sets the event area's `contextmenu` and `modifiercontextmenu` attributes from the
setting, and the widget library opens the one the keys ask for ([widgets.md](widgets.md)); it follows
`settings.changed`, so MCP's and Lua's `settings.set` apply at once.

- The **toolbox** has the toolbar's tools as icons, `menuitem[tool=<name>]` with the tool chosen checked; a click
  chooses the tool and closes it.
- The **context menu** has a Tools submenu with the same items, then the edit commands that act on the selection or
  the selection range, as the Edit menu runs them ([main-menu.md](main-menu.md)): Cut, Copy, Paste, Delete, Split at
  Cursor, Split Range, Crop Range, Glue, Duplicate, Repeat, Select All, Select None, Mute, Unmute, Lock and Unlock.
  A right click that opens it on an event that isn't selected selects that event first, so that they act on it; on a
  selected event, or on nothing, the selection stays. On an event it also ends a range, as a press there does with
  the selection tools combined, so the commands act on the event. Where a press selects a range (the range selection
  tool, and a lane's upper half with the selection tools combined) the right click leaves the events alone, and the
  commands act on the range. Cut, Copy, Split Range and Crop Range only edit the selection range, so they are shown
  disabled while there is none (all but Split Range also while it has no length), here and in the Edit menu; so
  are Cut Time, Delete Time and Insert Silence (`EventDisplay::canRun`). Their keys still run them, and without a range
  they do nothing. The toolbox leaves the selection as it is.

The key editor has a toolbox of its own, `#key-editor-toolbox` with its toolbar's tools, which a right click in its
note display opens by the same setting ([key-editor.md](key-editor.md)).

## Mouse pointers

Over the event lanes, the pointer shows what a press there does. The display names it in the event area's attribute
`pointer`, by the tool and where the mouse is, and `resources/styles/base.rcss` gives each name its pointer with
RmlUi's `cursor` property; the pointers are SVG files in `resources/cursors` ([svg.md](svg.md)).

| `pointer` | Where | Pointer |
|---|---|---|
| `objectSelection`, `comp` | The object selection and comp tools | The OS's arrow |
| `resize` | The object selection tool at an event's left or right edge, where a drag resizes it (6 dp, on events wider than 18 dp), except on a fade handle ([fades.md](fades.md)), and except with Ctrl or Shift held unless they are a tool modifier (they add to the selection then) | `resize` |
| `split` | The object selection tool on an event with the tool modifier Split Event held (Alt), where a click splits it (p. 268) | `tool-split` |
| `draw` | The object selection tool on an empty spot of an audio, MIDI or instrument lane with Split Event held, where a drag draws a part (p. 250) | `tool-draw` |
| `rangeSelection` | The range selection tool, and with the selection tools combined the upper half of a lane two rows high (p. 253) | `tool-range` |
| `draw`, `erase`, `split`, `glue`, `mute`, `zoom`, `line`, `play` | That tool | `tool-<name>`, the play tool's `tool-play` |

On an automation track the pointer is the chosen tool's, also with the selection tools combined, as a press there
edits points with that tool ([automation-tracks.md](automation-tracks.md)).

The pointer is the one for a press with the modifier keys held, by the user setting `ui.toolModifiers`. It changes as
the mouse moves, and at once when a tool is chosen, the view scrolls or zooms, or a modifier key goes down or up while
the mouse rests over the lanes; a modifier stays held while another key of it is down (the other Shift key, or Cmd for
Ctrl). Once the mouse has left the lanes, also for a menu opened over them, it no longer follows; it follows again
when the mouse is back, also without moving, as when a tool chosen in the toolbox closes it. A drag keeps the one it started
with, also where it leaves the lanes: the event area has RCSS `drag: drag`, and RmlUi shows a drag's element's pointer
until the button is up. MCP's `ui_windows` reports the pointer shown, as `cursor`: empty for the OS's arrow, also where
a pointer's picture didn't load or RmlUi names one the OS doesn't have, such as its autoscroll pointers.

Not yet: the time warp and color tools, the drumstick tool, and pointers of their own for the sizing modes; the rest of the reference's context
menu, with the Audio and MIDI menus' functions, follows with those menus. Cut and Copy of selected events without a
range (the event clipboard) wait for a ticket of their own. On X11, SDL shows only a pointer's scale-1 picture. The
audio editor gets its toolbox when it comes. The Preferences dialog's Editing - Tools page
(#162) will show the two settings, which `settings.get` and `settings.set` reach until then.

## Checks

- `tests/eventediting_tests.cpp` (fake window system, real engine), a test per tool: the tool commands, next and
  previous around the ends, the root's attribute and the cross-hair cursor; the object selection tool's rectangle,
  with Ctrl, and setting the cursor; splitting, snapped, into equal pieces, and with a tool modifier changed through
  `settings.set`; gluing to the next and to all following events; erasing one and all later events; muting and
  unmuting with clicks and a rectangle; zooming in, out, to a rectangle, back with a double click and forward again
  with Alt; the rectangle zooming the track heights with `ui.zoomToolHorizontalOnly` off; drawing parts,
  snapped, by dragging and with a click, and MIDI parts with the object selection tool and Alt, none on audio tracks or with the line tool; auditioning the audio lane's
  track on the dummy device while the mouse is pressed, nothing on a MIDI lane, scrubbing with a drag, and resizing an
  audio event at its lower corners in scrub mode, with its pointer there, while the upper part of the edge scrubs, the dummy device playing the clip's hidden audio
  that an edge dragged outwards reveals, and Alt and Alt+Shift clicks there neither splitting nor locating; ranges by dragging, clicking and double clicking on events and gaps, with Shift, snapped and on
  every track, and the events they select when another tool takes over; range edges at zero crossings with Snap to
  Zero Crossing; the combined selection tools, and a lane less than two rows high selecting events only; the pointer
  per tool, at events' edges and in a combined lane's upper half, also when a tool is chosen while the mouse rests;
  the pointer with tool modifiers held, also when Alt goes down and up while the mouse rests, with both Alt keys and
  with Cmd and Ctrl, and with the modifiers changed through `settings.set`; a drag's pointer over the ruler, and the pointer not following changes once the
  mouse has left the lanes; a right click selecting events only where a press selects them, and ending a range.
- `tests/uiapp_tests.cpp`: the toolbar's tool buttons, the keys 1 to 9, F9, F10 and Alt+Shift+1 in the app's project
  window, the object selection tool's, the line tool's and the play tool's mode menus on a second click; scrubbing an
  audio track with the mouse, and the dummy device playing it while the button is held; the split tool and the range
  tool driven with the mouse as MCP's UI tools drive it; the toolbox and the context menu on right clicks with and
  without modifiers, with the setting on and off, a tool chosen from each, a right click selecting the event for the context menu; the context menu's range commands
  disabled without a range and cutting with one, after a right click that leaves the event unselected; the
  notification after a key, not after a button or with the setting off, on RmlUi's frozen clock; the pointer as
  `ui_windows` reports it, also right after a tool is chosen in the toolbox. `tests/automationtracks_tests.cpp`: an
  automation track's pointer with the selection tools combined. `tests/keyeditor_ui_tests.cpp`: the key editor's toolbox by the setting and modifiers. The project window's golden image shows the
  toolbar.
- `tests/widgets_tests.cpp`: `modifiercontextmenu` with each modifier key, and notifications.
  `tests/svg_tests.cpp`: every pointer rasterizes at each scale with its hotspot inside it, and broken ones fall back
  to the OS's arrow, which RmlUi's system interface then reports as no pointer, as it does names the OS doesn't have.
