# Audio quantizing

Quantizing audio event starts, warp quantizing by time stretch and quantizing several audio tracks together
(#145, reference manual pp. 367-380). The grid, the groove and how far things move are the quantize settings of
[MIDI quantizing](quantize.md); the hitpoints are those of [hitpoints.md](hitpoints.md), the warp markers those of
[warp.md](warp.md).

## Audio event starts

`quantize.apply` and `quantize.reset` take audio events as well as MIDI notes: `events`, and with `audioParts` every
event of those audio parts (p. 369). An event moves so that its snap point, or without one its start, moves toward the
grid as a note's start does: strength, catch range, safe range, randomizing and grooves alike. The randomizing is
seeded by the position, not the event, so events at the same place move alike. Locked events stay.

As with notes, an event keeps where it was before it was first quantized and where quantizing put it
(`AudioEvent::quantizeOrigin`, saved in `gnubase.json`): quantizing again starts from the original unless it is soft,
and `quantize.reset` puts it back. An event moved by other means since is where it is, and its origin no longer counts.
`src/engine/document/audioquantize.cpp` has the functions; the commands stay in `midi/quantizecommands.cpp`.

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
| `quantize.slice` | Cuts every event `offset` ms before each position and gives each slice that starts there its snap point at the position | pp. 370, 378-379 |
| `quantize.createWarpMarkers` | Adds a warp marker at each position to every event's clip, changing nothing until it is quantized | pp. 371-372, 379-380 |
| `quantize.crossfade` | On each track: an event overlapping the next ends where it starts, an event after a gap starts where the one before ends, then each goes `length` ms into the next with a crossfade | p. 379 |

The positions are the hitpoints that count of the events' clips: those of the tracks with the highest priority
(`priorities`, 0 to 5 per track, default 1; 0 leaves a track out), then those of the others with none of a higher
priority, or of the same priority earlier, within `range` ms. Slicing all tracks at the same places with the slices
snapping there is what keeps them phase coherent: `quantize.apply` then moves the slices of every track at a position
by the same amount. Warp quantizing several tracks stretches each clip by itself, so it doesn't keep them phase
coherent (p. 371).

## In the UI

`src/app/appquantize.cpp` and `ui::QuantizePanel` (`src/ui/quantizepanel.h`), as for MIDI ([quantize.md](quantize.md)):

- **Edit > Quantize and Reset Quantize** (and the panel's Apply and Reset) take the project window's selected audio
  events and audio parts too, with its MIDI parts in one `quantize.apply` or `quantize.reset`. Quantize MIDI Event
  Ends and Lengths and Freeze MIDI Quantize stay MIDI only.
- **Warp Quantize On/Off**: `#quantize-warp` in the panel, next to the strength, and `#toolbar-quantize-warp` in the
  toolbar's quantize section, which shows it in windows at least 1100 dp wide only, where the toolbar has room for it.
  Both are `.checked` while `warpQuantize` is on and switch it with `quantize.set`.
- **Several audio tracks** (`#quantize-tracks`, below Apply): Range (`#quantize-range`) and Offset
  (`#quantize-offset`) in ms, Slice (`#quantize-slice`), or with warp quantizing on Create
  (`#quantize-create-markers`; the other one is `.hidden`), and Crossfade Length (`#quantize-crossfade-length`) with
  Crossfade (`#quantize-crossfade`). Each sends its engine command with the selected audio events and those of the
  selected audio parts, every track at priority 1.

## Not yet

Group editing of folder tracks (the commands take the events, of any tracks); the panel's Hitpoint Tracks list with
each track's priority (the engine takes `priorities`); Slice Rules' Reset (undo does it) and
cutting positions shown in the project window; Nudge Crossfade Left/Right and the crossfade editor of the Crossfades
section; quantizing a selection range of an event; Create Groove Quantize Preset from an audio event's hitpoints in
the Edit menu (`event.fromHitpoints` with `groove` makes it).

## Checks

`tests/audioquantize_tests.cpp`: event starts and snap points to the grid, strength, soft quantizing, reset, events
moved since, audio parts, locked events, the catch range, errors and the origin saved with the project; warp
quantizing a loop played on eighths with a quarter grid, rendered, with reset and existing markers; several tracks
sliced by priority and range, quantized and crossfaded, then rendered with the second track's audio inverted: the sum
is silence, so they stayed phase coherent; warp markers for several tracks of the same priority.
`tests/audioquantize_ui_tests.cpp` drives the app with `ui::Automation`: Edit > Quantize and Reset Quantize on a
selected audio event, Warp Quantize On/Off, and the panel's Slice, Apply, Crossfade and Create on two tracks.
