# Lanes and comping

Showing a track's takes on lanes, bringing takes to the front and comping them (#99, reference manual pp. 225–228).
It builds on cycle recording ([cycle-recording.md](cycle-recording.md)), which puts each lap's take on a lane of its
own, and works the same on takes and overlapping events put on lanes by hand (`event.set` with `lane`).

## Engine

- **The stack.** Where audio events overlap on a track, one plays: the one in front. Each audio event has a place in
  its track's stack, `front` in `project.get` and the events' announcements: 0 orders it by its id, so the event
  added last is in front, as before; a number brings it before every event added until then. The render snapshot
  orders a track's events by it (`frontOf` in `document.h`), and the playlist's front layer follows
  (`makePlaylist`). A split leaves the piece after it where the event was in the stack (its `front` is the event's
  place), so splitting a take behind another doesn't bring half of it to the front; a copy or a pasted event comes in
  front, as added last. Events copied, duplicated or pasted together, and the events of a copied audio part, keep
  the order they were stacked in among them, and so do takes that `event.toFront` or `event.comp` bring to the front
  together. The project file keeps `front` in GNUbase's entry; ids loaded go on after the highest one.
- **`event.toFront`** brings takes to the front, one undo step "To Front": an audio event gets a new place in front
  of every other, an audio part's events too, in the order they are stacked in. MIDI parts all play where they
  overlap (p. 226), so a MIDI take comes to the front unmuted, and the MIDI parts on its track's other lanes that it
  overlaps are muted, as a stacked recording leaves all but the last lap muted.
- **`event.comp`** is the comp tool's drag on a lane (p. 228): it splits every unlocked audio event, audio part and
  MIDI part on the track at the range's start and end (project samples), as `range.split` does, then brings what
  lies on the lane between them to the front, as `event.toFront` does; one undo step "Comp". A lane with nothing in
  the range is refused, so nothing is split.

The acceptance test records nothing: it puts two takes of a ramp file on lanes 0 and 1, comps lane 0 from 300 to
600 samples, renders through the dummy device and finds every sample of the first take in the range and of the
second around it; undo brings back the second take everywhere, redo the comp (`tests/comping_tests.cpp`, which also
checks `event.toFront` on audio and MIDI takes, splits, the project file and undo, the stack order of takes copied,
pasted or brought to the front together, and `lane` on the commands that add events and parts).

## Event display and track list

- **Show Lanes.** `EventDisplay::showLanes` (the track list's context menu: Show Lanes, the UI command
  `track.toggleLanesSelected`) shows an audio, instrument or MIDI track's lanes: a display lane per lane from 0 to the
  highest one its events and parts are on, below the track's own lane and above its automation tracks, as high as
  the track. Each shows the events and parts on that lane; `laneTakes()` names the lane each display lane shows (-1
  for a track's own and automation lanes), and the lanes have the track's id in `laneTracks()`. A part drawn with the
  draw tool or a file dropped on a lane goes on that lane (`lane` of `midiPart.add`, `audioPart.add`,
  `audioEvent.add` and `audioFile.import`). Which tracks show
  their lanes is view state, like folding. The track list's row reaches over its lanes, with its automation rows
  below them.
- **The comp result.** A track's own lane shows its audio events where none in front of them covers them, with
  their waveforms there, so it shows what plays; clicks there find the event in front. A muted event plays nothing
  and covers no event that plays, as in `makePlaylist`: it shows, muted, where no other covers it. `EventWaveforms`
  keeps an event's waves apart by their order, as an event can show on its track's lane and on its own.
- **The comp tool** (`comp`, the toolbar's and the toolbox's Comp, `tool.comp` without a key, after the range
  selection tool, as in the reference's toolbar): a click on a take brings it to the front (`event.toFront`), Shift
  and a click selects it, and a drag on a lane of a track whose lanes show comps the range dragged, its edges
  snapped (`event.comp`), showing it as `.tool-rectangle` while dragging. Its pointer is the OS's arrow.

Tests: `tests/eventediting_tests.cpp` shows a track's lanes, checks that the track's lane shows only the take in
front and that a muted take covers nothing, brings a take to the front with a click, comps a range with a drag and
draws parts on lanes; `tests/fileimport_tests.cpp` drops a file on a lane.

## Not yet

The reference's Show Lanes button among the track controls, and the lanes' own headers with their solo buttons
(p. 227); reordering and sizing lanes; clicking a take twice to toggle; the comp tool on the main track (moving,
resizing all takes with the same edges, adjusting and gluing splits, auditioning with Ctrl, p. 228) and merging
adjacent takes of matching material in a comp; To Front and Set to Region in the event context menu and the stacked
event's handle (p. 226), which needs regions in the pool; Clean Up Lanes, Delete Overlaps, Bounce Selection and
Create Tracks from Lanes (p. 227); the comp tool in the audio part editor; the Treat Muted Audio Events like Deleted
preference.
