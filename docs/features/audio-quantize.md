# Audio quantizing

Quantizing audio event starts, warp quantizing by time stretch and quantizing several audio tracks together
(#145, reference manual pp. 367-380). The grid, the groove and how far things move are the quantize settings of
[MIDI quantizing](quantize.md); the hitpoints are those of [hitpoints.md](hitpoints.md), the warp markers those of
[warp.md](warp.md).

## Audio event starts

`quantize.apply` and `quantize.reset` take audio events as well as MIDI notes: `events`, and with `audioParts` every
event of those audio parts (p. 369). An event moves so that its snap point, or without one its start, moves toward the
grid as a note's start does: strength, catch range, safe range, randomizing and grooves alike. The randomizing is
seeded by the position, not the event, so events at the same place move alike. Locked events stay, also when
resetting.

As with notes, an event keeps where it was before it was first quantized and where quantizing and the quantize panel's
length changes left it (`AudioEvent::quantizeOrigin`, saved in `gnubase.json`): a position, an offset, a length and a
crossfade for each of the two states, as a note's origin keeps its timing. Quantizing again starts from the original
unless it is soft, and `quantize.reset` puts back all four, so it also undoes what the panel's Crossfade step cut and
stretched (pp. 367, 379). An event moved by other means since is where it is, and its origin no longer counts (p. 375);
an offset, a length or a crossfade changed by other means since stays, and counts as the original from then on, as a
note's length or velocity changed by other means does. Each of the four is judged by itself, so on a slice whose start
the Crossfade step stretched and whose end the user then trimmed by hand, resetting puts the stretched offset back but
keeps the hand-set length, and the slice's end moves in the clip by the stretch: {5960, 160, 16896} resets to {5960,
5960, 16896} (#1333).

Resetting puts back the crossfade the event had before quantizing, so the ones `quantize.crossfade` made go and one
made by hand stays: resetting returns to the original, unquantized state (p. 367), and a crossfade from before
quantizing is part of it. Which crossfade that was is our reading of the state, not something the reference manual
spells out.

Splitting a quantized event (`event.split`) gives each piece an origin of its own, so that resetting puts both back
where they were and they tile again: the piece after the split was as far from its original, in samples, as the split
is from where quantizing put the event, and its original offset and length are the recorded original's, cut where the
piece before it ends, not its current ones: with a start the Crossfade step stretched, those would reset it to the
wrong section of the clip and leave a gap before the next slice (#1333). The distance goes through samples because a
musical event's position is in ticks while its audio keeps its length in samples, so the tempo between the two counts.
A piece keeps no origin when its event was moved by other means since the panel left it.

The panel can also have made the event longer than its original: every slice the Crossfade step crossfades into the
next one is longer by the crossfade, and a slice whose start it stretched by the stretch, so a split can fall further
from the event's start than the original's length. The piece before it then keeps at most the whole original, and the
piece after it gets none of the original's length, even where it plays frames of the original, since the piece before
it takes all of them back: resetting removes it, in the same undo step, and lists it in `removed` (#1435). Reset
Quantize returns to the original, unquantized state and resets the panel's length changes (p. 367), and that state has
nothing where the piece is. Splitting such a piece again gives two pieces that resetting removes. A piece whose end was
trimmed by hand since has a length set by other means, which counts as its original as any such length does: resetting
keeps it, moves it back by the distance its event moved and leaves it that length, past the end of the original.

An event that crossfaded from the split event now crossfades from the piece after the split, and its origin's
crossfades follow, so that resetting still takes out one the panel made and keeps one made by hand, from the piece that
is then the neighbour before it. When the piece after the split has nothing of the original, the crossfade from before
quantizing stays with the piece before it, the whole original, and resetting removes the piece after the others are
back, so that crossfade comes back. Removing the piece ends the crossfade of a neighbour that isn't reset with it; its
origin then takes the ended crossfade as the panel's, so that resetting the neighbour later still brings back the one
from before quantizing.

Locked events stay as they are when resetting, a piece with nothing of the original too, and so do the warp markers
that resetting would put back for them: a locked event is protected from being moved and edited (p. 276), and
quantizing leaves it alone as well. Whether a part's lock protects its
events is #1076's question. `src/engine/document/audioquantize.cpp` has the functions; the commands stay in
`midi/quantizecommands.cpp`.

## Warp quantizing

With `warpQuantize` in the quantize settings (the Warp Quantize On/Off switch, p. 370), `quantize.apply` quantizes
the audio inside each event by time stretch instead of moving the event: the warp markers of its clip inside the event
move so that they play on the grid. A clip without markers there gets one at each of its hitpoints that count, where
it plays now. For each grid position the marker nearest to it goes toward it; the others keep their places relative to
the markers around them that move and the event's start (p. 370's 1/4 grid on sixteenths). An event that starts
inside its clip gets a marker at its start, so that it keeps playing the same audio there. Each marker's target is
found by bisection through `warpedOffset`, so musical mode and the definition grid are taken into account, between
targets that keep the sections around the marker within the real-time stretcher's range (4 times as long to a
quarter, [warp.md](warp.md)): a marker the grid would take further goes as near as it can.

The clip keeps its markers from before (`ClipWarp::quantizeOrigin`, saved with the warp): quantizing several events
of a clip starts from them once, and `quantize.reset` puts them back while the markers are still what quantizing made
of them; markers made from hitpoints then go again.

## Several audio tracks

The quantize panel's Slice Rules, Warp Marker Creation Rules and Crossfades sections (pp. 370-372, 378-380) are engine
commands on the events of several tracks. Each is one undo step.

| Command | What it does | Reference |
|---|---|---|
| `quantize.slice` | Cuts every event `offset` ms before each position, gives each slice that starts there its snap point at the position and clears the snap point of every other slice | pp. 370, 378-379 |
| `quantize.createWarpMarkers` | Adds a warp marker at each position to every event's clip, changing nothing until it is quantized | pp. 371-372, 379-380 |
| `quantize.crossfade` | On each track: an event overlapping the next ends where it starts, an event after a gap starts where the one before ends, then each goes `length` ms into the next with a crossfade. `quantize.reset` undoes all of it | pp. 367, 379 |
| `quantize.nudgeCrossfade` | Moves each crossfade into one of the events by `amount` ms (default 1, negative earlier): the end of the event before and the start of the event alike, so the crossfade keeps its length. `quantize.reset` undoes it too | p. 379 |

The positions are the hitpoints that count of the events' clips: those of the tracks with the highest priority
(`priorities`, 0 to 5 per track, default 1; 0 leaves a track out), then those of the others with none of a higher
priority, or of the same priority earlier, within `range` ms. `range` says when hitpoints on *different* tracks mark the
same beat (pp. 378, 380): two of one track, such as a flam, stay two positions, while two events of one track whose
hitpoints coincide give one position between them. Slicing all tracks at the same places with the slices snapping there
is what keeps them phase coherent: `quantize.apply` then moves the slices of every track at a position by the same
amount. A slice that starts at no position gets no snap point at all, so it snaps at its start like the same slice on
every other track, instead of keeping the one `event.split` copied from its event. Warp quantizing several tracks
stretches each clip by itself, so it doesn't keep them phase coherent (p. 371).

`quantize.crossfade` records what it changed in each event's origin, so that a later `quantize.reset` puts every slice
back to its position, offset, length and crossfade, and the slices tile again with no overlaps and no gaps left: Reset
Quantize resets the length changes made in the quantize panel too (p. 367), and crossfades are such changes (p. 379).
What the user changed by hand since is not: resetting only puts back a part of the event that still is where the panel
left it.

`quantize.nudgeCrossfade` moves the audio a crossfade plays, for example off an attack that the slices' Offset didn't
keep clear (p. 379): the event the crossfade comes from ends that much earlier or later, and the event it goes into
starts there, playing its clip from that much earlier or later. A crossfade moves only as far as both clips have audio
and it still plays, so a step that would take it past either clip's start or end leaves it where it is; locked events
stay. It records what it changed in the events' origins as `quantize.crossfade` does, so resetting still puts every
slice back.

## Groove quantize presets from hitpoints

`quantize.extractGroove` makes a groove preset of an audio event's hitpoints that count with `event` instead of a MIDI
part (Create Groove Quantize Preset, pp. 367, 373): a groove position at each, the groove as long as the event and
repeating from its start, named by default after its clip. It is the groove `event.fromHitpoints` with `groove` makes
the project's settings ([hitpoints.md](hitpoints.md)), kept as a preset, so it is on every Quantize Presets menu.

## In the UI

`src/app/appquantize.cpp` and `ui::QuantizePanel` (`src/ui/quantizepanel.h`), as for MIDI ([quantize.md](quantize.md)):

- **Edit > Quantize and Reset Quantize** (and the panel's Apply and Reset) take the project window's selected audio
  events and audio parts too, with its MIDI parts in one `quantize.apply` or `quantize.reset`. Quantize MIDI Event
  Ends and Lengths and Freeze MIDI Quantize stay MIDI only.
- **Warp Quantize On/Off**: `#quantize-warp` in the panel, next to the strength, and `#toolbar-quantize-warp` in the
  toolbar's quantize section, where it is the first thing to go when the toolbar has no room for all its sections
  (`fit="spare"`, [docking.md](docking.md)).
  Both are `.checked` while `warpQuantize` is on and switch it with `quantize.set`.
- **Several audio tracks** (`#quantize-tracks`, below Apply): Range (`#quantize-range`) and Offset
  (`#quantize-offset`) in ms, Slice (`#quantize-slice`), or with warp quantizing on Create
  (`#quantize-create-markers`; the other one is `.hidden`), and Crossfade Length (`#quantize-crossfade-length`) with
  Crossfade (`#quantize-crossfade`). Each sends its engine command with the selected audio events and those of the
  selected audio parts, every track at priority 1. Nudge Crossfade Left and Right (`#quantize-nudge-left`,
  `#quantize-nudge-right`, next to Crossfade) send `quantize.nudgeCrossfade` with -1 and 1 ms.
- **Edit > Advanced Quantize > Create Groove Quantize Preset** (`edit.createGrooveQuantize`) takes the first selected
  audio event when no MIDI part is selected, and makes its groove the project's settings, as for a MIDI part.

## Not yet

Group editing of folder tracks (the commands take the events, of any tracks); the panel's Hitpoint Tracks list with
each track's priority (the engine takes `priorities`); Slice Rules' Reset (undo does it) and
cutting positions shown in the project window; Open Crossfade Editor in the Crossfades section; quantizing a
selection range of an event; the quantize functions on the sample editor's toolbar.

## Checks

`tests/audioquantize_tests.cpp`: event starts and snap points to the grid, strength, soft quantizing, reset, events
moved since, audio parts, locked events, the catch range, errors and the origin saved with the project; splitting a
quantized event and resetting both pieces, on a linear and on a musical track whose tempo changes under it; warp
quantizing a loop played on eighths with a quarter grid, rendered, with reset and existing markers, and a musical clip
whose project runs at half its own tempo, and a locked event's markers staying; several tracks sliced by priority and
range, quantized and crossfaded, then rendered with the second track's audio inverted: the sum is silence, so they
stayed phase coherent; the range leaving a flam on one track two positions, and hitpoints of one track at the same place
giving one position; slicing clearing the snap points it doesn't set; soft and rough quantizing moving the slices of two
tracks alike; resetting after a crossfade that cut an overlap and one that closed a gap, putting every slice back to its
position, offset and length and removing the crossfades, also across a save and a load; splitting a slice whose start
the Crossfade step stretched and resetting both pieces, so that they tile the original slice again, splitting one inside
the stretch at its start and inside the part the step added to it, where resetting removes the piece after the split in
one undo step unless it is locked, the tail of a slice the step stretched at its start and made longer than its
original, and a crossfade into the next slice following such a split; a crossfade made by hand before quantizing coming
back from the piece before a split at the original's end, whose piece after it resetting removes: all at once, with undo
and redo, the neighbour after the others, across a save and a load, through an audio part, with the piece split again,
and with the piece's end trimmed by hand, which keeps it; resetting keeping a crossfade the event had before quantizing
while the one the Crossfade step made goes, also across a save and a load and from the piece after a split of the event
it comes from; resetting keeping an end trimmed and a crossfade made by hand after quantizing, and an end trimmed by
hand on a slice the step stretched; resetting leaving a locked event where quantizing left it; a project file whose
origins keep no offset or length yet; warp markers for several tracks of the same priority; Nudge Crossfade right,
left and back, with reset after it, not past a clip's start and not with a locked event; a groove preset from an audio
event's hitpoints that then quantizes another event, its name and its errors.
`tests/audioquantize_ui_tests.cpp` drives the app with `ui::Automation`: Edit > Quantize and Reset Quantize on a
selected audio event, Warp Quantize On/Off, the panel's Slice, Apply, Crossfade, Nudge Crossfade and Create on two
tracks, and Create Groove Quantize Preset from a selected audio event.
