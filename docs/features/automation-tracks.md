# Automation tracks

The project window's automation tracks and their editing (#121, reference manual pp. 870, 872–880): lanes below a
track's own that show its automation lanes ([automation.md](automation.md)) as curves, the static value line, the
parameter selector, and the tools that draw, select, move, scale and erase points. The code is
`src/ui/automationtracks.cpp` (the event display's lanes and tools, `ui::EventDisplay`) and `src/ui/tracklist.cpp`
(the rows, `ui::TrackList`). Every edit goes to the engine as the `automationLane.*` and `automationPoint.*` commands
that MCP and Lua have too, one undo step each; which automation tracks show is view state, like folding, and not in
the project or its undo history.

## In the engine

- **`automationLane.targets`** `{track}` lists what a track's automation tracks can show, in the order they offer it:
  its channel's parameters as `parameter.list` returns them (volume, pan, the panner's sides, mute, the built-in
  instrument's and the plugins' parameters), then on MIDI and instrument tracks Pitch Bend, Channel Pressure and
  Controller 0 to 127 (MIDI channel 0, from 0 to 1; pitch bend's static value is 0.5; a MIDI track set to a MIDI
  channel plays them on that one, [mixer.md](mixer.md)). Each comes with `group`
  (`Channel`, the instrument's or plugin's name, or `MIDI`) and `lane`, the id of the track's lane for it, or 0.
  Tracks without a channel have none. Its result schema describes the items' fields. The event display asks for a
  track's targets again only when the track, its channel, the channel's plugins, the track's lanes (not their points)
  or the project's maximum volume change.

## Automation tracks

```
.event-area
  .automation-lanes                    over the event lanes
    .automation-lane[lane=<id>]        at its lane; .off while it doesn't read
      curve.static-value               the static value line, dashed
      curve.automation-curve           the curve with its points, selected ones larger
  .automation-editor                   around the selected points, .shown
#track-list listitem
  button.automation-toggle             Show/Hide Automation, .checked while shown
  .automation-rows
    .automation-row[lane=<id>]
      button.automation-parameter      opens the parameter selector #automation-parameter-menu
      button.toggle.automation-read    Read, .checked
      button.automation-append         Append Automation Track
```

- **Lanes.** An automation track is a lane of the event display of its own, 40 dp high (times vertical zoom), below
  its track's lane and those of the track's other automation tracks. `EventDisplay::laneAutomation()` names its
  automation lane per display lane (0 for a track's own), and `laneTracks()` its track. The track list's row reaches
  down over them; the track's height still drags at its own lane's lower edge.
- **Showing and hiding** (p. 880): Show/Hide Automation in a track's lower left corner, 16 dp square, left of the name
  and above the 4 dp at the row's lower edge that size the track (2 dp in the lowest rows, 20 dp), so that the
  selection frame and the separator to the next row stay whole (#1773), or the track list's context menu (Show/Hide
  Automation for the selected tracks, Show All Used Automation, Hide All Automation; `track.toggleAutomationSelected`,
  `track.showUsedAutomation`, `track.hideAllAutomation`; Show All Used Automation adds to the automation tracks that
  show, unused ones too). Showing a track's automation shows its lanes with points, or else its first lane, or else
  its first parameter (volume on tracks with audio) without a lane. Append Automation Track shows the next parameter
  in `automationLane.targets`' order that no automation track shows, with its lane or without one. Selecting a track
  with the keys scrolls it into view with its automation tracks, as far as they fit (`TrackList::reveal`). A new or
  loaded project (`project.reset`) starts with none open and no points selected, since its tracks, lanes and points get
  the old project's ids again.
- **Unadded lanes** (#1918): an automation track whose parameter has no lane on the track shows an unadded lane
  (`EventDisplay::unaddedLane`, shaped like `project.get`'s lanes, with read off and no points, its id above the
  project's), so that opening automation tracks changes neither the project nor its undo history. The first points a
  tool draws on it add the lane, which reads, in their undo step: the lane's `history.transaction` and then the
  points' with the same merge key. Pasting onto it adds the lane in the paste's own undo step, through
  `automation.paste`'s targets, only where the paste pastes ([copy-automation.md](copy-automation.md)). A lane that
  goes while its automation track shows, as when that step is undone, leaves the track showing its parameter unadded,
  and a lane that comes for an unadded lane's parameter, as on redo, takes its place. Read does nothing on an unadded
  lane, and copying selected automation tracks leaves them out.
- **Parameter selector** (p. 880): the parameter's name opens a menu with a search field, which narrows the track's
  parameters to those whose name (not its RML, where `&`, `<` and `>` are entities) has the text in any case; Remove
  Parameter (`automationLane.remove`, p. 878), Remove Unused Parameters (the track's lanes without points, p. 880), and
  on a plugin's lane Show Only Automation Tracks of the plugin. Picking a parameter shows its lane in that automation
  track, or the parameter unadded if the track has none; it is no undo step, and a lane the track showed stays in the
  project. Remove Parameter and Remove Unused Parameters close the automation tracks they take, unadded ones too.
- **Curves** (p. 870): ramps for continuous parameters, steps for on/off and stepped ones, as the engine plays them.
  A lane's values fill its height from its parameter's minimum to its maximum; volume from -60 dB to the project's
  maximum, like the mixer's fader, with silence at the bottom. A lane that doesn't read is grayed out (`.off`).
  Gaps (p. 881, [virgin-territory.md](virgin-territory.md)) have no line: from a terminator point up to the next
  point, and with virgin territory before the first point and after the last one (`ui::CurvePoint`'s `terminator`,
  `ui::Curve::setVirginTerritory`). The display asks for the project again on `automation.changed`, since virgin
  territory is no undo step.
- **Static value line** (p. 870): a dashed line at the parameter's static value (`automationLane.targets`' `value`)
  while the lane has no points or doesn't read.

## Tools

A click with a tool turns reading on (p. 873), in the same undo step as the points. Snapping applies where a click
places a point and where a moved point lands ([snap.md](snap.md)); Ctrl with Alt or Shift turns it off for a move
(p. 877).

| Tool | Click | Drag |
|---|---|---|
| Object Selection | On a point: selects it, on one of several selected points only it (Ctrl or Shift: adds or removes it); elsewhere adds a point and ends the selection (Ctrl or Shift: keeps it) | A selected point moves the selection in time and value, and a continuous selection replaces the points where it lands (pp. 877-878), none when the times stay, so a jump's other point does; with the keys held at the press, Ctrl: across or up and down only, Ctrl with Alt or Shift: unsnapped. From an empty spot a rectangle selects the points in it (Ctrl or Shift: adds them) |
| Draw | Adds a point | Draws a curve: the points drawn, reduced to those that stray more than 2 dp from the line between their neighbours (p. 873), replace the points in its range |
| Line | | Replaces the points in its range with a line, a parabola (fast at first when drawn rightwards), or a sine, triangle or square wave between the start's and the end's value with periods of the grid as zoom adapts it (p. 874); not on step curves |
| Erase | Removes the point | |

- **Automation event editor** (p. 876): with two or more points of one ramp curve selected, a box around them. Its
  top border's middle scales them vertically from the box's bottom, its top corners tilt the left or right part, the
  rest of its top moves them up or down, and its bottom border stretches them in time from the box's left.
- **Copying automation to other tracks** (pp. 878–879) selects automation tracks in the track list and copies
  their automation, or that of tracks, between the locators: [copy-automation.md](copy-automation.md).
- **Other tools.** The zoom and play tools work on automation tracks as on the track's own lane: the play tool
  auditions and scrubs an audio track from its automation tracks too. A press with the range selection, split, glue,
  mute, comp or time warp tool does nothing there.
- **Delete** (`edit.delete`, Backspace) removes the selected points (p. 878). Selecting events or a range deselects
  the points, and the other way round for events; points and selected markers or time signature events deselect each
  other too (`AppUi`, #2011), so that Delete deletes what was selected last. The selection is the display's
  (`EventDisplay::selectedPoints()`), also in the display's attribute `automation-selection` as `lane:point` pairs.

## Not yet

Write automation (#122), automation track heights of their own, the automation panel and its settings (Reduction Level,
Reveal Parameter on Write), bézier handles and quick vertical scaling of one segment (pp. 874, 877), the editor's
compress, relative and absolute centre scaling, Range Selection on automation tracks with its added points, Alt with the
object selection tool to draw, a selection with unselected points in it that can't move past other points (p. 878),
Shift to set the line tool's period, the most recent parameters at the top of the selector, Open Plugin Editor, and
automation of the channel strip's sections (#658).

## Checks

- `tests/parameter_tests.cpp`: `automationLane.targets` on audio, MIDI, instrument and marker tracks, with lanes.
- `tests/automationtracks_tests.cpp` drives the app's project window with `ui::Automation`: showing a track's automation
  shows its first parameter unadded, level with its row, with the static value line; Show/Hide Automation inside its
  track's own part of the row at every track height; Append Automation Track; the parameter selector with its search,
  picking a parameter without an undo step, Remove Parameter on an unadded lane and on a lane with points, whose
  automation track stays closed, and Remove Unused Parameters closing its automation track; opening and picking changing
  neither the project nor its undo history, and the first points adding their lane in their undo step, whose undo leaves
  the automation track unadded and whose redo puts the lane back; hiding, Show All Used Automation, Hide All Automation
  and the selected tracks' command; the draw tool's click and curve, the line tool's line, square, sine, triangle and
  parabola, nothing on a step curve, the erase tool, and reading off with the static value line; selecting points with
  clicks, Ctrl and a rectangle, the event editor's vertical scaling and moving, moving points by a drag, Delete, and
  selecting events deselecting points, points and markers or time signature events deselecting each other, also through
  the markers window's Sync Selection, with Delete deleting what was selected last; a Ctrl-drag of selected points in
  one direction, a Ctrl-click that deselects one, the editor's tilt left, tilt right and stretch; a move that snaps the
  pressed point, and Ctrl with Shift or Alt that turns snapping off; a moved selection that replaces the point where it
  lands, in its preview and in one undo step, a click on one of the selected points, a click on an empty spot that ends
  the selection, a Ctrl-restricted move released after Ctrl, and a range that deselects points; a vertical move of one
  of a jump's two points that keeps the other; the zoom tool on an automation track, the play tool auditioning and
  scrubbing its track there, Show All Used Automation keeping an unused automation track open, and selecting a track
  with the keys that reveals its automation track; Show Only Automation Tracks of a plugin; a new project without the
  old one's open automation tracks and selected points; the curve's terminators and virgin territory following
  `automation.set`.
- `tests/canvas_tests.cpp`: a curve's lines with a terminator in the middle and at the end, with virgin territory,
  and scrolled to start after a terminator.
