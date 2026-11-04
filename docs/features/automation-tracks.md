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
  Controller 0 to 127 (MIDI channel 0, from 0 to 1; pitch bend's static value is 0.5). Each comes with `group`
  (`Channel`, the instrument's or plugin's name, or `MIDI`) and `lane`, the id of the track's lane for it, or 0.
  Tracks without a channel have none.

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
- **Showing and hiding** (p. 880): Show/Hide Automation in a track's lower left corner, or the track list's context
  menu (Show/Hide Automation for the selected tracks, Show All Used Automation, Hide All Automation;
  `track.toggleAutomationSelected`, `track.showUsedAutomation`, `track.hideAllAutomation`). Showing a track's
  automation shows its lanes with points, or else its first lane, or else adds a lane for its first parameter (volume
  on tracks with audio). Append Automation Track shows the next parameter in `automationLane.targets`' order that no
  automation track shows, adding its lane if the track has none. New lanes don't read until a tool draws on them.
- **Parameter selector** (p. 880): the parameter's name opens a menu with a search field, which narrows the track's
  parameters to those whose name has the text in any case; Remove Parameter (`automationLane.remove`, p. 878),
  Remove Unused Parameters (the track's lanes without points, p. 880), and on a plugin's lane Show Only Automation
  Tracks of the plugin. Picking a parameter shows its lane in that automation track, adding it if the track has
  none; a lane without points that the track showed is removed in the same undo step.
- **Curves** (p. 870): ramps for continuous parameters, steps for on/off and stepped ones, as the engine plays them.
  A lane's values fill its height from its parameter's minimum to its maximum; volume from -60 dB to the project's
  maximum, like the mixer's fader, with silence at the bottom. A lane that doesn't read is grayed out (`.off`).
- **Static value line** (p. 870): a dashed line at the parameter's static value (`automationLane.targets`' `value`)
  while the lane has no points or doesn't read.

## Tools

A click with a tool turns reading on (p. 873), in the same undo step as the points. Snapping applies where a click
places a point and where a moved point lands ([snap.md](snap.md)).

| Tool | Click | Drag |
|---|---|---|
| Object Selection | On a point: selects it (Ctrl or Shift: adds or removes it); elsewhere adds a point | A selected point moves the selection in time and value (Ctrl: across or up and down only); from an empty spot a rectangle selects the points in it (Ctrl or Shift: adds them) |
| Draw | Adds a point | Draws a curve: the points drawn, reduced to those that stray more than 2 dp from the line between their neighbours (p. 873), replace the points in its range |
| Line | | Replaces the points in its range with a line, a parabola (fast at first when drawn rightwards), or a sine, triangle or square wave between the start's and the end's value with periods of the grid as zoom adapts it (p. 874); not on step curves |
| Erase | Removes the point | |

- **Automation event editor** (p. 876): with two or more points of one ramp curve selected, a box around them. Its
  top border's middle scales them vertically from the box's bottom, its top corners tilt the left or right part, the
  rest of its top moves them up or down, and its bottom border stretches them in time from the box's left.
- **Delete** (`edit.delete`, Backspace) removes the selected points (p. 878). Selecting events deselects the points,
  and the other way round. The selection is the display's (`EventDisplay::selectedPoints()`), also in the display's
  attribute `automation-selection` as `lane:point` pairs.

## Not yet

Write automation (#122), automation track heights of their own, the automation panel and its settings (Reduction
Level, Reveal Parameter on Write), bézier handles and quick vertical scaling of one segment (pp. 874, 877), the
editor's compress, relative and absolute centre scaling, Range Selection on automation tracks with its added points,
copying automation to other tracks (pp. 878–879), Alt with the object selection tool to draw, Shift to set the line
tool's period, the most recent parameters at the top of the selector, Open Plugin Editor, the gaps of virgin
territory and terminators ([virgin-territory.md](virgin-territory.md)), and automation of the channel strip's sections
(#658).

## Checks

- `tests/parameter_tests.cpp`: `automationLane.targets` on audio, MIDI, instrument and marker tracks, with lanes.
- `tests/automationtracks_tests.cpp` drives the app's project window with `ui::Automation`: showing a track's
  automation adds its first lane, level with its row, with the static value line; Append Automation Track; the
  parameter selector with its search, picking a parameter in one undo step, Remove Parameter and Remove Unused
  Parameters; hiding, Show All Used Automation, Hide All Automation and the selected tracks' command; the draw tool's
  click and curve, the line tool's line, square, sine, triangle and parabola, nothing on a step curve, the erase tool,
  and reading off with the static value line; selecting points with clicks, Ctrl and a rectangle, the event editor's
  vertical scaling and moving, moving points by a drag, Delete, and selecting events deselecting points.
