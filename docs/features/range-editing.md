# Range editing

Editing selection ranges in the project window (#88, reference manual pp. 285-294): splitting and cropping at a
range, deleting and cutting it with or without closing the gap, copying and pasting it with or without making room,
inserting silence, and global copy. The engine's commands are in `src/engine/document/eventedits.cpp`, next to the
`event.*` commands ([parts-and-events.md](parts-and-events.md)); the range tool that selects ranges is in
[tools.md](tools.md).

## Engine commands

A range is `start` to `end` in project samples, by default the locators, on `tracks` (ids, by default every track).
Each command is one undo step and changes nothing if it fails; `range.copy` changes nothing in the project at all.

| Command | What it does | Reference |
|---|---|---|
| `range.split` | Splits the events and parts at the range's edges (at its start for a range of no length) | p. 293 |
| `range.crop` | Cuts the events and parts that cross an edge to the range; those inside or outside stay | pp. 293-294 |
| `range.delete` | Deletes what is in the range, and the part of the cycle markers it lies in; `time` (Delete Time) closes the gap | pp. 292, 425 |
| `range.insertSilence` | Moves what starts at the range's start or later on by the range's length | pp. 292-293 |
| `range.copy` | Copies the range to the range clipboard; without a range everything between the locators (Global Copy) | p. 292 |
| `range.cut` | `range.copy`, then `range.delete`; `time` (Cut Time) closes the gap | pp. 292, 425 |
| `range.paste` | Pastes the clipboard at `position` (default the cursor); `time` (Paste Time) first makes room | p. 292 |
| `range.move` | Moves what is in the range to `position` on the same tracks, without the clipboard; `copy` leaves it and pastes a copy | pp. 291, 432 |

- **Cuts at the edges.** Events and parts that cross an edge are split there first, as `event.split` splits them, so
  only what lies in the range is affected (p. 291). A MIDI part's notes across an edge stay in the part they start in,
  or with `splitNotes` (the reference's Split MIDI Events preference, p. 287) are cut in two. Locked events stay
  whole, aren't deleted and don't move. Events and parts are cut where they show, in samples: an edge in the sample a
  part on a musical track starts or ends in, which in ticks can lie up to half a sample from it, doesn't cut it.
- **Time.** Closing a gap and making room move what follows by the range's length: in ticks on musical tracks, so that
  it moves exactly by beats, in samples on linear ones.
- **Automation.** The automation lanes of the range's tracks are edited with their events, and the curve outside the
  range stays as it was: points are added at the edges with the curve's values there (p. 288 does this for controller
  curves). Deleting the range removes its points and holds the value the curve arrives at the start with up to the
  end; Delete Time joins that value and the one after the range at the start. Inserting silence holds the value at the
  start over the silence. A copy keeps the points in the range with the curve's values at both edges, and pasting it
  replaces the curve where it lands, on the destination track's lane of the same parameter; a track without such a
  lane gets none. A point shaped as a curve keeps its bend over what remains of its segment. Like events and parts,
  points are in the range where they show, in samples: on a musical track a point in the sample an edge lies in, before
  the edge's tick, is where the edge is, so that a point on the downbeat of a part that starts in the range's start
  sample goes with the part, and one on the downbeat of a part that starts in the end sample stays with that part, also
  when Delete Time moves it back.
- **The clipboard** is the engine's, so MCP, Lua and the UI share it; it isn't part of the project or of undo. It keeps
  copies of the events, parts and points, so cutting and deleting the originals doesn't change it. A new or opened
  project empties it, since it holds the old project's track and clip ids. `range.paste` goes
  onto the tracks the range was copied from, or with `track` onto that track and the ones below it in the project's
  order, one for each copied track; their types must take the copied events. Pasted events are unlocked real copies;
  a copied audio event whose clip was removed from the pool can't be pasted.
- **Cycle markers.** Without `time`, `range.delete` and `range.cut` take the range out of a cycle marker on a marker
  track in the range that it lies in (p. 425): shortened, or split in two ([markers.md](markers.md)). Other markers
  stay.
- **Moving** (`range.move`) cuts what lies in the range at its edges and moves it to `position`, in one undo step and
  leaving the clipboard as it is. What moves keeps its id, so an editor open on a moved part keeps it, a crossfade
  between two moved events stays, also one between an audio part's event and an event outside the part when both move,
  and a shared copy of a MIDI part that lies in the range whole stays one (a part cut at an edge is a real copy, as
  `range.split` makes it); a crossfade between a moved event and one that stays ends. A cut gives the piece after it the
  new id, as `event.split` does: of an event or part that starts before the range, the piece that moves is new and the
  one before the range keeps the id; of one that starts in the range and ends after it, the piece that moves keeps it
  and the one after the range is new, so an editor open on such a part follows the moved piece. The automation moves as
  `range.cut` and `range.paste` would move it. With `copy` the range stays and a copy is pasted at `position` as
  `range.paste` pastes, with new ids. Either way the audio that lands comes in front of the events there, every event in
  the order it was stacked, so that the events of an audio part keep their places among the others. Locked events stay
  where they are and aren't copied. The event display does it when the range is dragged by the marker track
  ([markers.md](markers.md), p. 432).
- **Where it lands.** On musical tracks what lies in the range moves by the gap between `start` and `position` in
  ticks, so that it keeps its distance from the range's start in beats. What starts where the range starts lands at
  `position` exactly, a piece cut there and an event that starts there alike: an event's start in ticks can lie up to
  half a sample from the range's start, and the gap, with both ends rounded to ticks, could otherwise put it a sample
  off `position` at some tempos. So when such an event lies on a musical track, everything on the range's musical tracks
  moves by the one shift in ticks that puts the first of them in ticks at `position`, on every track alike: what lay on
  one tick, on one track or on several, still does, and the others that start in the range's start sample keep their
  distance of less than a sample from it, so they can land in the sample after `position`. Where the gap already puts
  the first of them in `position`'s sample, it stays the shift: a part on a bar pasted where it was copied, or moved to
  the range's own start, keeps the bar's tick. The automation on musical tracks moves by the same shift as the events,
  so a point on a part's downbeat stays on it. `range.paste` lands the same way at its `position`, and so does what
  `arranger.flatten` pastes. Not yet: a `position` in ticks, so that a drag that snaps to a bar lands on the bar's own
  tick.
- **Global copy** copies every track between the locators: the events, parts and automation. Markers, the tempo and
  the signatures stay where they are.

## The event display

With a selection range ([tools.md](tools.md)) the edit commands work on it, and it stays after each edit (p. 287):

| Command | Keys | Does |
|---|---|---|
| `edit.cut`, `edit.copy` | Mod+X, Mod+C | Cuts or copies the range |
| `edit.paste` | Mod+V | Pastes at the range's start onto its first track and those below; without a range at the cursor onto the tracks it came from, selecting what it pasted |
| `edit.delete` | Delete, Backspace | Deletes the range (without one, the selected events) |
| `edit.cutTime`, `edit.deleteTime`, `edit.pasteTime` | Mod+Shift+X, Shift+Backspace, Mod+Shift+V | The same, closing the gap or making room |
| `edit.insertSilence` | Mod+Shift+E | Inserts silence |
| `edit.splitRange`, `edit.cropRange` | Shift+X, none | Splits or crops at the range |
| `edit.globalCopy` | none | Copies everything between the locators |

The main menu's Edit menu has Cut, Copy and Paste, and the Range submenu Global Copy, Cut Time, Delete Time, Paste
Time, Split, Crop and Insert Silence.

Not yet: copying and pasting selected events without a range, Paste at Origin and Paste Relative to Cursor, moving and
duplicating a range's contents by dragging it on other tracks than marker tracks, nudging its edges, Range Tool Splits Partly Selected Events, the info
line's range fields, the range in the editors (they come with their tickets), and the controller events at the edges of a range inside a part.

## Checks

- `tests/eventedit_tests.cpp`, the range tests: splitting at both edges and at a range of no length with a locked
  event; deleting with the gap kept and closed, on a linear audio track with pan automation and a musical MIDI track
  with controller automation, and undo; inserting silence; cropping, with and without splitting notes; copying (no
  undo step), pasting onto the same tracks with the automation replacing the curve, Cut Time and Paste Time, pasting
  onto other tracks and the errors for a wrong type, too few tracks and nothing copied; global copy and a pasted clip
  that is gone; moving and copying a range with `range.move`, in one undo step, without the clipboard and leaving a
  locked event, moving the automation as `range.cut` and `range.paste` move it, keeping the ids of what moves, the
  crossfade between two moved events and a shared copy, bringing the moved audio to the front in its order, also an
  event stacked between two of an audio part's and two audio parts whose events interleave, undo and redo, and ending
  the crossfade between a moved event and one that stays, either way round; the pan and modulation points after a
  move, also onto the range's own later or earlier half; which piece of an event cut at either edge keeps its id; a
  crossfade from an audio part's event to an event outside it, kept when both move and ended when either stays; a
  shared copy cut at an edge becoming a real copy while the copies in the range stay shared; and at tempos whose bars
  lie between samples, two parts on consecutive bars, a part on the second bar on another MIDI track, an audio event
  and an audio part on musical tracks moved, copied and pasted by a range from the first part's start sample: the
  first part, the audio event and the audio part with its event landing on the position's sample with the part's first
  note and the modulation point on its downbeat, the second part a bar after the first in ticks, the other track's part
  on the second part's tick, and no piece left behind; at such tempos two parts on two tracks less than a sample apart
  in ticks in the range's start sample and the piece of a third track's part cut there, the first of them in ticks
  landing at the position and the others as many ticks after it as before, moved, copied and pasted; parts on two
  bars cut and pasted where they were and moved to the range's own start, keeping their bars' ticks; and
  `range.delete` up to the sample the next part starts in, with and without Delete Time, and `range.crop` to that
  part's own samples leaving it whole with the note and the modulation point on its downbeat.
- `tests/eventediting_tests.cpp` (fake window system, real engine): the commands exist; split, Delete Time, cut and
  paste on a selection range, which stays; paste at the cursor without a range, selecting what it pasted.
