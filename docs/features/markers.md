# Markers

Marker tracks and markers, locating with them, and the markers in the UI (#91, reference manual pp. 155–156,
422–433).

- **Markers** (#91) sit on marker tracks: tracks without a channel that stay at the top level, at most 10 per project. One marker track is active (`activeMarkerTrack`): marker commands without a track use it, a new marker track becomes it, and removing it activates the topmost one. A position marker marks a position, a cycle marker a range (position and end). Positions are in the marker track's time base; position and cycle markers are numbered separately on each track from 1 (the first free number), and the numbers are what key commands like "locate to marker 3" use. `marker.add`, `marker.put` (position marker N at the cursor: moved there, or added with that number), `marker.set` (move, rename, renumber, move to another marker track), `marker.remove`, `marker.renumber` and `marker.activateTrack` are undo steps with `marker.added`, `marker.changed`, `marker.removed` and `marker.trackActivated` events; `marker.add` and `marker.put` without a marker track add one in the same step. `marker.list` and `marker.range` (the range between the markers around a position, which a range selection by markers spans) are queries.
- Locating with markers moves the transport, which is not in the document: `marker.locateNext`, `marker.locatePrevious`, `marker.locate` (marker N, or by id) and `marker.setLocators` (to a cycle marker) compute sample positions from the document and run `transport.locate` or `transport.set` inside the command (`CommandContext::call`), so clients see `transport.changed` and no undo step. Marker commands that default to the cursor or the locators read them with `transport.get` the same way.
- **Cycle Follows When Locating to Markers** (p. 428), the user setting `transport.cycleFollowsMarkers` (off by default): `marker.locate`, `marker.locateNext` and `marker.locatePrevious` also set the locators to the marker they locate to: a cycle marker's range (also when they reach its end), or from a position marker to the next marker position or cycle marker end after it on its track. After the last marker, the locators stay.

## Marker files

Markers are imported and exported with MIDI files and CSV files (#171, pp. 432–433):

- **MIDI files** ([midi-files.md](midi-files.md)) have the start of every position and cycle marker and their marker
  tracks: in a type 1 file the tempo track has the active marker track's markers, where other applications look for
  them, and each other marker track with markers a file track of its own after the others, named after it and with
  nothing but its markers. `midiFile.import` puts the tempo track's markers, and those of any track with other events,
  on the active marker track and makes each marker-only track a new marker track at the top, while the project has
  room for one (at most 10); otherwise its markers go on the active one too. So a file from another application has
  all its markers on one marker track, as in the reference. A type 0 file has every marker in its one track. The
  marker track that takes the tempo track's markers is active afterwards; its name and the cycle markers' ends and the
  marker ids are not in the file.
- **CSV files**: `marker.export` writes marker tracks (default all) as a header row with the columns Track
  Number, Track, Time Base, Type, ID, Position, End, Description and Color, then a row per marker, tracks in track
  order and markers in timeline order. Track Number numbers the tracks in the file from 1, so marker tracks with the
  same name stay apart; rows with the same number are one track, rows without one those with the same Track name.
  Positions and ends are beats (quarter notes) on musical tracks and seconds on linear ones, as the shortest decimals
  that read back exactly, so `marker.import` gives back every marker track with its name and time base and every
  marker with its type, id, position, end, description and color, and exporting them again gives the same file. The
  import makes each track of the file a new marker track at the top, keeps the active marker track (or activates the
  first new one) and is one undo step. It reads files from spreadsheets too: RFC 4180 quoting, CRLF or LF line ends
  and a UTF-8 byte order mark; columns are found by their header in any order and others are left out, and only
  Position is needed (without Track one marker track, without Time Base musical, without Type position markers,
  without ID the first free number). A row it can't read, an id that's taken, a row whose Track name or Time Base
  differs from the earlier rows of its track (also an empty one), or too many marker tracks change nothing, and the
  error names the row. **File > Export > Markers...** (`file.exportMarkers`) and **File > Import > Markers...**
  (`file.importMarkers`) choose the file in the OS's file dialog; an exported file without an extension gets `.csv`.
  The reference's track archives, which also hold marker tracks, come with track import and export.

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

- **Marker lines** across every track, behind the events and folder data: an `<event-lanes class="marker-lines">` with
  one lane as tall as any display, a line of no length per position marker and at the start and end of each cycle
  marker. Which markers: the user setting `ui.markerLines`, `active` (the active marker track's, the default), `all` or
  `off`, as the reference's Show Marker Lines menu and preferences (p. 425). Changes from MCP or Lua apply at once.
- **Marker events** on the marker tracks' lanes, over the events: an `<event-lanes class="marker-events">` with a lane
  per track, position markers as lines and cycle markers as spans, and a `.marker-label` per marker with its number,
  in brackets for cycle markers, and its name; labels of the active marker track's markers have `.active`. The themes
  color them; code places the labels. The user setting `ui.markerIds` (Show Marker IDs on Marker Track, on by
  default) leaves out the numbers when it is off.
- **Mouse** on a marker track's lane: a click selects the marker under the mouse (its label, a position marker's line
  within 4 dp, or the cycle marker it is in) and Ctrl-click adds or removes it, its label getting `.selected`, and the info line shows and edits the selected markers ([inspector.md](inspector.md), p. 430); selecting
  a marker deselects the events and selecting an event the markers;
  Alt-click adds a position marker there (but not on a cycle marker); a double click on a cycle marker sets the
  locators to it (`marker.setLocators`), with Alt it zooms to it (p. 424). The event
  display stops mouse downs, so RmlUi sends no `dblclick`: two clicks within half a second and 3 dp make one.
- The event display tells it about every change of the view (`EventDisplay::onViewChanged`); the marker elements are
  its own, in its event area, so the display's code doesn't know about markers.

## Markers window

`ui::MarkersWindow` fills `resources/ui/markers.rml`, which `app::AppUi::toggleMarkers` opens in a window of its own
(Mod+M opens and closes it, from any window), as the reference's Project > Markers (pp. 426–431):

- `#marker-track` picks the active marker track (Set Focus to Marker Track), `marker.activateTrack`.
- `#marker-list`, a table in a list view ([widgets.md](widgets.md)), has a row per marker of the active marker track:
  the locate column, whose arrow marks the marker at the project cursor or the last one before it (`.at-cursor`) and
  where a click locates to the marker; then the id number, the type, the position, the end and length (cycle markers)
  and the description. All but the type are text fields, which Enter or leaving the field changes (`marker.set`; a
  number that is taken, text that isn't a number or a time, or an end before the position shows the old value
  again). Times are in the transport's primary time format (`ui.timeFormats`), lengths as lengths ("0.2.0.0" for two
  beats in bars and beats); a new position moves a cycle marker as a whole, a new length moves its end. Rows select
  with clicks, Ctrl, Shift, the arrow keys, Page Up and Page Down, Home and End (p. 428); Enter locates to the
  selected marker.
- **Several markers at once** (p. 430): a description or a length typed into a row of a selection of several gives it
  to all the selected markers (the length to the cycle markers among them), in one undo step (`history.transaction`).
  The reference does this with checkbox attributes, which markers here don't have.
- **Columns** (p. 429): a click on a header sorts by its column, a second one the other way round, the position always
  second (`.sorted` and `.descending` on the header); dragging a header moves its column, dragging the line between
  two headers resizes the column on its left. The locate column stays first. Their order, widths and sorting are the
  user setting `ui.markerColumns`, whose changes from MCP or Lua apply at once.
- **Auto-Scroll with Project Cursor** (`#marker-auto-scroll`, p. 427), the user setting `ui.markersAutoScroll` (on by
  default): when the arrow moves to another marker, the list scrolls as little as it takes to show its row.
- **Marker settings** (`#marker-settings`, p. 428): Show Marker IDs on Marker Track (`ui.markerIds`), Cycle Follows
  When Locating to Markers (`transport.cycleFollowsMarkers`) and Sync Selection (`ui.markersSyncSelection`, on by
  default), with which the markers window and the project window's marker events select the same markers
  (`app::AppUi` gives each one's selection to the other), except while recording. When it starts syncing (the window opens, recording stops, the setting is turned on) the window takes the project window's selection.
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
removing markers and picking the marker track; editing positions, ends and lengths in the primary time format and
refusing what isn't valid; sorting by a column with the position second, moving and resizing columns and the
setting's changes from MCP; Auto-Scroll on and off, Page Up and Page Down; changing the descriptions and lengths of
several selected markers in one undo step; the marker settings: marker ids on the marker track, cycle follows and
sync selection both ways, paused while recording; and the marker key commands locating, setting and recalling
markers. `tests/marker_tests.cpp` covers `transport.cycleFollowsMarkers` for the three locate commands. Marker files:
`tests/marker_tests.cpp` has the CSV round trip byte for byte with undo, also of marker tracks with the same name,
a spreadsheet's file and files that change nothing, among them rows of one Track Number with different names;
`tests/midifile_tests.cpp` the marker tracks in type 1 and type 0 files, the same file exported again and the limit
of 10 marker tracks; `tests/markers_ui_tests.cpp` the File menu's items through the file dialog.

Not yet: dragging markers and resizing cycle markers on the marker track, Ctrl-drag for a cycle marker and Shift to
rename at once (with parts editing, #85); range selections by markers (the range tool, #86; `marker.range` gives the
range); hiding columns; the track list's controls
for marker tracks (add markers, activate, the Locate, Cycle and Zoom menus, #81); marker lines in the key editor;
locking marker tracks; importing markers from wave files.
