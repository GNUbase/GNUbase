# Markers

Marker tracks and markers, locating with them, and the markers in the UI (#91, reference manual pp. 155–156,
422–433).

- **Markers** (#91) sit on marker tracks: tracks without a channel that stay at the top level, at most 10 per project. One marker track is active (`activeMarkerTrack`): marker commands without a track use it, a new marker track becomes it, and removing it activates the topmost one. A position marker marks a position, a cycle marker a range (position and end). Positions are in the marker track's time base; position and cycle markers are numbered separately on each track from 1 (the first free number), and the numbers are what key commands like "locate to marker 3" use. `marker.add`, `marker.put` (position marker N at the cursor: moved there, or added with that number), `marker.set` (move, rename, renumber, move to another marker track), `marker.remove`, `marker.renumber` and `marker.activateTrack` are undo steps with `marker.added`, `marker.changed`, `marker.removed` and `marker.trackActivated` events; `marker.add` and `marker.put` without a marker track add one in the same step. `marker.list` and `marker.range` (the range between the markers around a position, which a range selection by markers spans) are queries.
- Locating with markers moves the transport, which is not in the document: `marker.locateNext`, `marker.locatePrevious`, `marker.locate` (marker N, or by id) and `marker.setLocators` (to a cycle marker) compute sample positions from the document and run `transport.locate` or `transport.set` inside the command (`CommandContext::call`), so clients see `transport.changed` and no undo step. Marker commands that default to the cursor or the locators read them with `transport.get` the same way.

## Key commands

The reference's defaults (`ui::defaultKeyMap`, [key-commands.md](key-commands.md)), everywhere:

| Keys | Command | Reference |
|---|---|---|
| Shift+N, Shift+B | `marker.locateNext`, `marker.locatePrevious` | Locate Next/Previous Marker |
| Shift+1 to Shift+9, Num3 to Num9 | `marker.locate` with the number | To Marker 1 to 9 |
| Mod+1 to Mod+9, Mod+Num3 to Mod+Num9 | `marker.put` with the number | Set Marker 1 to 9 |
| Shift+Num1 to Shift+Num9 | `marker.setLocators` with the number | Recall Cycle Marker 1 to 9 |
| Insert | `marker.add` | Insert Marker |
| Mod+M | the UI command `window.markers` | Open Markers |

## Event display

`ui::MarkerLines` (`src/ui/markers.h`) draws the markers into the project window's event display
([event-display.md](event-display.md)) and follows the project like it, reading `project.get` after every change:

- **Marker lines** across every track, behind the events: an `<event-lanes class="marker-lines">` with one lane as
  tall as any display, a line of no length per position marker and at the start and end of each cycle marker. Which
  markers: the user setting `ui.markerLines`, `active` (the active marker track's, the default), `all` or `off`, as the
  reference's Show Marker Lines menu and preferences (p. 425). Changes from MCP or Lua apply at once.
- **Marker events** on the marker tracks' lanes, over the events: an `<event-lanes class="marker-events">` with a lane
  per track, position markers as lines and cycle markers as spans, and a `.marker-label` per marker with its number,
  in brackets for cycle markers, and its name; labels of the active marker track's markers have `.active`. The themes
  color them; code places the labels.
- **Mouse** on a marker track's lane: Alt-click adds a position marker there (but not on a cycle marker); a double
  click on a cycle marker sets the locators to it (`marker.setLocators`), with Alt it zooms to it (p. 424). The event
  display stops mouse downs, so RmlUi sends no `dblclick`: two clicks within half a second and 3 dp make one.
- The event display tells it about every change of the view (`EventDisplay::onViewChanged`); the marker elements are
  its own, in its event area, so the display's code doesn't know about markers.

## Markers window

`ui::MarkersWindow` fills `resources/ui/markers.rml`, which `app::AppUi::toggleMarkers` opens in a window of its own
(Mod+M opens and closes it, from any window), as the reference's Project > Markers (pp. 426–431):

- `#marker-track` picks the active marker track (Set Focus to Marker Track), `marker.activateTrack`.
- `#marker-list`, a table in a list view ([widgets.md](widgets.md)), has a row per marker of the active marker track:
  the locate column, whose arrow marks the marker at the project cursor or the last one before it (`.at-cursor`) and
  where a click locates to the marker; the id number and the description in text fields, which Enter or leaving the
  field changes (`marker.set`; a number that is taken or isn't one shows the old one again); the type; position, end
  in bars and beats and length in seconds. Rows select with clicks, Ctrl, Shift and the arrow keys; Enter locates to
  the selected marker.
- Add Marker (`#add-position-marker`) adds a position marker at the cursor, Add Cycle Marker one at the locators,
  Remove the selected markers. The Functions menu moves the selected markers to the cursor in one undo step
  (`history.transaction` of `marker.set`s; cycle markers keep their length) and reassigns the position or cycle
  marker ids in timeline order (`marker.renumber`).

## DAWproject

- DAWproject has one list of markers in the arrangement and no ranges: it gets the markers of every marker track in timeline order, cycle markers at their start; read from another application, they become position markers on one marker track.

## Checks

`tests/marker_tests.cpp` covers the engine's commands, `marker.put` among them. `tests/markers_ui_tests.cpp`: on the
fake window system, marker lines for the active track, every track and none as the setting changes, behind the
events and on the display's timeline; marker events and labels on the marker tracks' lanes, following vertical zoom;
Alt-click adding a marker only on a marker track and off cycle markers, a double click setting the locators and Alt
zooming. In the app's UI (`test::UiApp`): Mod+M opening and closing the markers window, its rows, locating from the
locate column and with Enter, the arrow at the cursor, editing descriptions and ids, the functions, adding and
removing markers and picking the marker track; and the marker key commands locating, setting and recalling markers.

Not yet: dragging markers and resizing cycle markers on the marker track, Ctrl-drag for a cycle marker and Shift to
rename at once (with parts editing, #85); range selections by markers (the range tool, #86; `marker.range` gives the
range); editing positions in the markers window and on the info line; sorting and reordering its columns; the marker
settings (marker ids on the track, Cycle Follows When Locating to Markers, Sync Selection); the track list's controls
for marker tracks (add markers, activate, the Locate, Cycle and Zoom menus, #81); marker lines in the key editor;
locking marker tracks; importing markers from wave files.
