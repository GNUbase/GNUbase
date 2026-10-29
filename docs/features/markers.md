# Markers

Marker tracks and markers, and locating with them (#91).

- **Markers** (#91) sit on marker tracks: tracks without a channel that stay at the top level, at most 10 per project. One marker track is active (`activeMarkerTrack`): marker commands without a track use it, a new marker track becomes it, and removing it activates the topmost one. A position marker marks a position, a cycle marker a range (position and end). Positions are in the marker track's time base; position and cycle markers are numbered separately on each track from 1 (the first free number), and the numbers are what key commands like "locate to marker 3" use. `marker.add`, `marker.set` (move, rename, renumber, move to another marker track), `marker.remove`, `marker.renumber` and `marker.activateTrack` are undo steps with `marker.added`, `marker.changed`, `marker.removed` and `marker.trackActivated` events; `marker.add` without a marker track adds one in the same step. `marker.list` and `marker.range` (the range between the markers around a position, which a range selection by markers spans) are queries.
- Locating with markers moves the transport, which is not in the document: `marker.locateNext`, `marker.locatePrevious`, `marker.locate` (marker N, or by id) and `marker.setLocators` (to a cycle marker) compute sample positions from the document and run `transport.locate` or `transport.set` inside the command (`CommandContext::call`), so clients see `transport.changed` and no undo step. Marker commands that default to the cursor or the locators read them with `transport.get` the same way.

## DAWproject

- DAWproject has one list of markers in the arrangement and no ranges: it gets the markers of every marker track in timeline order, cycle markers at their start; read from another application, they become position markers on one marker track.
