# Range editing

Editing selection ranges in the project window (#88, reference manual pp. 285-294): splitting and cropping at a
range, deleting and cutting it with or without closing the gap, copying and pasting it with or without making room,
inserting silence, and global copy. The engine's commands are in `src/engine/document/eventedits.cpp`, next to the
`event.*` commands ([parts-and-events.md](parts-and-events.md)); the range tool that selects ranges is in
[tools.md](tools.md).

## Engine commands

A range is `start` to `end` in project samples, by default the locators, on `tracks` (ids, by default every track). A
folder track among them brings the tracks inside it, nested ones too, as its folded lane shows their events as data
blocks and editing them edits what they stand for (p. 251). An `end` past 2^48 samples, the largest project position,
means up to it, so that a client can send the largest integer for "to the end". Each command is one undo step and
changes nothing if it fails; `range.copy` changes nothing in the project at all.

| Command | What it does | Reference |
|---|---|---|
| `range.split` | Splits the events and parts at the range's edges (at its start for a range of no length) | p. 293 |
| `range.crop` | Cuts the events and parts that cross an edge to the range; those inside or outside stay | pp. 293-294 |
| `range.delete` | Deletes what is in the range, markers too; `time` (Delete Time) closes the gap | pp. 292, 425 |
| `range.insertSilence` | Moves what lies after the range's start, cut there, on by the range's length | pp. 292-293 |
| `range.copy` | Copies the range to the range clipboard; without a range everything between the locators (Global Copy); with `events` those events and parts whole | p. 292 |
| `range.cut` | `range.copy`, then `range.delete`; `time` (Cut Time) closes the gap; with `events` it copies and removes those | pp. 292, 425 |
| `range.paste` | Pastes the clipboard at `position` (default the cursor) or `positionTicks`, with `origin` (Paste at Origin) at the copied range's start, or with `relative` (Paste Relative to Cursor) as far from the cursor as it was when copied; `time` (Paste Time) first makes room; `selectedTrack` and `snap` paste events as Paste does | pp. 270, 292 |
| `range.move` | Moves what is in the range to `position` or `positionTicks`, on the same tracks or from `track` on, without the clipboard; `copy` leaves it and pastes a copy | pp. 291, 432 |

- **Cuts at the edges.** Events and parts that cross an edge are split there first, as `event.split` splits them, so
  only what lies in the range is affected (p. 291). A MIDI part's notes across an edge stay in the part they start in,
  or with `splitNotes` (the reference's Split MIDI Events preference, p. 287) are cut in two. Locked events stay whole,
  aren't deleted and don't move; `range.cut` doesn't copy them either, so that pasting the cut doesn't add them again.
  Events and parts are cut where they show, in samples: an edge in the sample a part on a musical track starts or ends
  in, which in ticks can lie up to half a sample from it, doesn't cut it. Once cut, a piece's middle decides where it
  lies: it is in the range when its middle lies at or after the start and before the end, and it comes after an edge, so
  that Delete Time, Cut Time, Insert Silence and Paste Time move it, when its middle lies at or after the edge (`after`
  in `eventedits.cpp`). Not its start: a cut in a clip at another sample rate than the project's lands up to half a clip
  frame off, and an event that starts less than that before an edge isn't cut there at all. So an unsplit event that
  starts a sample before the range's start and ends in it lies in the range, and one that starts a sample before its end
  lies after it. Later range features, such as dragging a range's contents or ranges in the editors, use the same rule.
- **Time.** Closing a gap and making room move what follows by the range's length: in ticks on musical tracks, so that
  it moves exactly by beats, in samples on linear ones.
- **Automation.** The automation lanes of the range's tracks are edited with their events, and the curve outside the
  range stays as it was: points are added at the edges with the curve's values there (p. 288 does this for controller
  curves). Deleting the range removes its points and holds the value the curve arrives at the start with up to the end;
  Delete Time joins that value and the one after the range at the start. Inserting silence holds the value at the start
  over the silence. A copy keeps the points in the range with the curve's values at both edges, and pasting it replaces
  the curve where it lands, on the destination track's lane of the same parameter; a track without such a lane gets
  none. A lane without points played the parameter's static value, and still does around the paste: terminators start
  gaps up to it and after it (p. 881), unless virgin territory makes them gaps anyway. A point shaped as a curve keeps
  its bend over what remains of its segment. Like events and parts, points are in the range where they show, in samples:
  on a musical track a point in the sample an edge lies in, before the edge's tick, is where the edge is, so that a
  point on the downbeat of a part that starts in the range's start sample goes with the part, and one on the downbeat of
  a part that starts in the end sample stays with that part, also when Delete Time moves it back; inserting silence
  there moves it later with the part. A terminator stays where its tick is: an edge on it would fill the gap it starts.
  Delete Time closes the gap by the range's length in ticks, as the events after it move, so the points after the range
  stay with their events. With the end edge on a point in its sample, that length puts the point, and the part on its
  downbeat, up to half a sample before the start tick, half a sample at the end's tempo: at a slower tempo at the start
  that can be more than a sample there. A point that stays before the range but lies after where the curve after it goes
  on, a terminator just before the start tick or a point in the sample before the range's, moves onto that joint, ahead
  of the curve that goes on there, so that no gap swallows what comes after the range and nothing passes what stays.
  Events don't move onto the joint: one that lands there can pass an event that stays just before the range in ticks.
  From sample 0, tick 0, or with a faster tempo at the end from a sample just after it, the length would put such a
  part, its point or an event of an audio part cut in that sample before tick 0: there everything after the range,
  events, points and transpose events, moves back by less, by the ticks that put the first of it on tick 0, and keeps
  its distances in ticks.
- **MIDI controllers.** A range edit that cuts a MIDI part starts the piece after each cut with the value each
  controller curve has there, so that the curves outside the range stay as they were (p. 288): for each control change
  number and channel, pitch bend, channel pressure and poly pressure pitch, the value of the curve's last event before
  the cut, unless the piece has an event of that curve at its start. What lies after the range then plays as it did
  after a delete, Delete Time, Insert Silence or a move, and a copy or crop of the range starts with the values the
  curves had at its start. Program changes, SysEx and articulations aren't curves and aren't repeated; `event.split` adds nothing.
- **The clipboard** is the engine's, so MCP, Lua and the UI share it; it isn't part of the project or of undo. It keeps
  copies of the events, parts and points, so cutting and deleting the originals doesn't change it. A new or opened
  project empties it, since it holds the old project's track and clip ids. `range.copy` and `range.cut` announce what it
  holds as `rangeClipboard.changed`; a `range.cut` in a `history.transaction` that fails leaves it as it was, as it
  leaves the project. `range.paste` goes onto the tracks the range was copied from, or with `track` onto that track and
  the ones below it in the project's order that aren't hidden, as the event display shows them, one for each copied
  track. A folded folder's lane counts once there, as the display shows it: the tracks inside it don't count, and the
  copied tracks of a folded folder's lane go into the tracks of the folded folder's lane they land on, in their order
  (#1978). The folded folders are the track layout's (`trackLayout.set`), not part of undo, so where a paste lands
  follows the view. Their types must take the copied events. Its linear positions are samples at the project's sample
  rate when it was copied: after `project.set` changes the rate, `range.paste` refuses it until the rate is back, and so
  does `automation.paste` its own clipboard. Pasted events are unlocked real copies; a copied audio event whose clip was
  removed from the pool can't be pasted.
- **Events without a range** (#1910). With `events` (ids) instead of `start`, `end` and `tracks`, `range.copy` and
  `range.cut` take those audio events and parts and MIDI parts whole, as a range from the first one's start to the last
  one's end on their tracks would, but without the other events there and without automation. `range.cut` removes them
  as `event.remove` does, and refuses locked ones. The clipboard also keeps where the project cursor was, so that
  `range.paste` with `relative` (Paste Relative to Cursor, p. 270) pastes at the cursor plus the distance the copied
  range's start had from it; a paste that would start before the project start is refused. Paste (p. 270) goes onto
  the selected track: `range.paste` with `selectedTrack` pastes from it on as with `track`, but a copied track that
  finds no track there, or one of a type that can't take what was copied from it, goes onto the track it came from.
  With `snap` a clipboard of events lands so that the snap point of its first audio event (not one in a part), or that
  event's start, lies at the position; a range's copy pastes at its start as without it.
- **Markers.** Without `time`, `range.delete` and `range.cut` take the range out of the markers on the marker tracks
  in the range as out of events (p. 425, #1190): the markers in it go, and a cycle marker across an edge is shortened,
  or split in two, the piece after the range keeping its id and number ([markers.md](markers.md)).
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
  where they are and aren't copied. With `track` it goes onto other tracks: the range's top track onto `track` and each
  of the others as many tracks further on in the project's order of tracks that aren't hidden, so that the range keeps
  its shape, as `range.paste` skips hidden tracks and counts a folded folder's lane once; the tracks there must take the
  events (audio onto audio tracks, MIDI parts onto MIDI and instrument tracks), and the automation lands on their lanes
  of the same parameter. The event display does it when the range is dragged, and Duplicate does it with `copy` at the
  range's end (pp. 291, 432).
- **Where it lands.** On musical tracks what lies in the range moves by the gap between `start` and `position` in
  ticks, so that it keeps its distance from the range's start in beats. What starts where the range starts lands at
  `position` exactly, a piece cut there and an event that starts there alike: an event's start in ticks can lie up to
  half a sample from the range's start, and the gap, with both ends rounded to ticks, could otherwise put it a sample
  off `position` at some tempos. So when such an event lies on a musical track, everything on the range's musical tracks
  moves by the one shift in ticks that puts the first of them in ticks at `position`, on every track alike: what lay on
  one tick, on one track or on several, still does, and the others that start in the range's start sample keep their
  distance of less than a sample from it, so they can land in the sample after `position`. Where the gap already puts
  the first of them in `position`'s sample, it stays the shift: a part on a bar pasted where it was copied, or moved
  to the range's own start, keeps the bar's tick. Not at position 0, the project's start, which is tick 0 exactly:
  there the first lands on tick 0. The automation on musical tracks moves by the same shift as the events, so a point
  on a part's downbeat stays on it, and nothing lands before tick 0: where the range's automation starts before the
  first event, at a point in the start sample or at the start's tick, at position 0 the events land as without it and
  what of the automation would land before tick 0 lands on tick 0, in its order, so that a part on a bar lands on
  tick 0 and the curve goes on from there as the last of those points shapes it, the one on the part's downbeat if it
  has one. No event lands before tick 0 either, where one lies before the first that starts in the range's start sample:
  pasted at another tempo than it was copied at, where it no longer lies in that sample, or an event of an audio part
  cut in the sample it starts in, which stays whole in the piece after the cut, starting before it. Then the earliest of
  them lands on tick 0. `range.paste` lands the same way at its `position`, and so does what `arranger.flatten` pastes.
- **At a tick.** Instead of `position` in samples, `range.move` and `range.paste` take `positionTicks`, a tick of the
  tempo map (#1785): the first in ticks of what starts in the range's start sample on a musical track lands on that
  tick exactly, and without such an event what lies on musical tracks moves by the ticks from the range's start tick
  to it. Linear tracks go to the sample the tick lies in. A bar's tick can lie up to half a sample from the bar's
  sample, so a part on a bar moved to another bar's sample lands on the tick nearest to it, or keeps its distance in
  ticks where that lands in the sample; at the other bar's tick it lands on that tick. Nothing lands before tick 0
  here either.
- **Global copy** copies every track between the locators: the events, parts and automation. Markers, the tempo and
  the signatures stay where they are.

## The event display

With a selection range ([tools.md](tools.md)) the edit commands work on it, and it stays after each edit (p. 287):

| Command | Keys | Does |
|---|---|---|
| `edit.cut`, `edit.copy` | Mod+X, Mod+C | Cuts or copies the range; without one the selected events (`events`) |
| `edit.paste` | Mod+V | Pastes at the range's start onto its first track and those below; without a range at the cursor onto the first selected track (`selectedTrack`, `snap`), selecting what it pasted |
| `edit.delete` | Delete, Backspace | Deletes the range (without one, the selected events) |
| `edit.cutTime`, `edit.deleteTime`, `edit.pasteTime` | Mod+Shift+X, Shift+Backspace, Mod+Shift+V | The same, closing the gap or making room |
| `edit.pasteAtOrigin` | Alt+V | Pastes where it was copied from, onto the tracks it came from (p. 270); without a range selecting what it pasted |
| `edit.pasteRelative` | Shift+V | Pastes as far from the cursor as it was when copied, onto the first selected track (`selectedTrack`, p. 270); without a range selecting what it pasted |
| `edit.insertSilence` | Mod+Shift+E | Inserts silence |
| `edit.splitRange`, `edit.cropRange` | Shift+X, none | Splits or crops at the range |
| `edit.globalCopy` | none | Copies everything between the locators |
| `edit.duplicate` | Mod+D | Copies the range's contents to its end (`range.move` with `copy`), where the range goes, so that the next one follows (p. 291) |
| `edit.nudgeLeft`, `edit.nudgeRight` | Mod+Left, Mod+Right | Move the range by a grid step, its contents stay (p. 290) |
| `edit.nudgeStartLeft`, `edit.nudgeStartRight`, `edit.nudgeEndLeft`, `edit.nudgeEndRight` | Alt+Left, Alt+Shift+Right, Alt+Shift+Left, Alt+Right | Move its start or end by a grid step, the start never past the end (p. 289) |
| `edit.nudgeTopUp`, `edit.nudgeTopDown`, `edit.nudgeBottomUp`, `edit.nudgeBottomDown` | Alt+Up, Alt+Shift+Down, Alt+Shift+Up, Alt+Down | Take the track above or below it in, or its top or bottom track out, as long as one stays (p. 289) |

The grid step is the nudges' for events ([parts-and-events.md](parts-and-events.md)), at the edge that moves, and
nothing moves before the project start. With the range tool, a press in the range on one of its tracks, without Shift,
drags its contents (p. 291): the range follows the mouse across, its start snapped, and down or up by as many tracks as
the mouse, as far as there are tracks for all of its own, and the release moves what is in it there (`range.move`, with
`track` when the tracks changed, and at `positionTicks` when its start snapped to a musical grid line, so that a part on
a bar lands on another bar's own tick; with Grid Relative a range that starts in a line's own sample snaps to the other
line, as the distance in ticks of less than a sample is the rounding of that sample, not one to keep (#2005), and one
that starts off the grid at its distance from it), or with
the tool modifier Copy (`dragDrop.copy`, Alt by default) a copy of it; the range stays where it went, and a drag that
came back to the press leaves it where it was, one that came back across only its position. A press within 6 dp inside
one of its edges, without Shift, drags that edge instead (p. 288), with the pointers `resize` at the start and the end
and `resizeVertical` at the top and the bottom ([tools.md](tools.md)): the start or the end follows the mouse across,
snapped, and past the other edge becomes that one; the top or the bottom goes to the track under the mouse, taking the
tracks it passes in or out, not past the other edge. Only a range wider or higher than 18 dp has those edges. A click
there selects a range as anywhere. Shift and a drag in the range selects a new range, just the stretch dragged over, and
Shift and a drag outside it adds to the range (p. 288). A drag released outside the window puts the range back at the
next press.

The main menu's Edit menu has Cut, Copy and Paste, its Functions submenu Paste at Origin and Paste Relative to Cursor
(p. 270), and the Range submenu
Global Copy, Cut Time, Delete Time, Paste Time, Split, Crop and Insert Silence. Menus and keys disable the commands that
only edit the range while there is none, the top and bottom nudges too, and Paste, Paste Time and Paste at Origin until
something is copied (`rangeClipboard.changed`, since the project opened), Paste Relative to Cursor too. Without a
range, Cut and Copy take the selected events, and can't run while none is selected.

The info line shows the range, made with the range selection tool or, with the selection tools combined, the object
selection tool: Range Start, Range End, Range Length, Top Track and Bottom Track, which set it (pp. 287-288,
[inspector.md](inspector.md)).

Not yet (#1910): Range Tool Splits Partly Selected Events, and the range in the editors (they come with their
tickets).

## Checks

- `tests/eventedit_tests.cpp`, the range tests: splitting at both edges and at a range of no length with a locked
  event; deleting with the gap kept and closed, on a linear audio track with pan automation and a musical MIDI track
  with controller automation, and undo; inserting silence; cropping, with and without splitting notes; copying (no
  undo step), pasting onto the same tracks with the automation replacing the curve, Cut Time and Paste Time, pasting
  onto other tracks and the errors for a wrong type, too few tracks and nothing copied; pasting with `origin` where the
  range was cut, with the cursor elsewhere, and refused with a position too; copying events whole without the others
  on their tracks or the automation, cutting one in one undo step, a locked one refused and events with a range
  refused; pasting with `relative` as far from the cursor as the copy was, refused before the project start and with
  a position or `origin`; global copy and a pasted clip that is gone;
  moving and copying a range with `range.move`, in one undo step, without the clipboard and leaving a locked event,
  moving the automation as `range.cut` and `range.paste` move it, keeping the ids of what moves, the crossfade between
  two moved events and a shared copy, bringing the moved audio to the front in its order, also an event stacked between
  two of an audio part's and two audio parts whose events interleave, undo and redo, and ending the crossfade between a
  moved event and one that stays, either way round; the pan and modulation points after a move, also onto the range's
  own later or earlier half; which piece of an event cut at either edge keeps its id; a crossfade from an audio part's
  event to an event outside it, kept when both move and ended when either stays; a shared copy cut at an edge becoming a
  real copy while the copies in the range stay shared; and at tempos whose bars lie between samples, two parts on
  consecutive bars, a part on the second bar on another MIDI track, an audio event and an audio part on musical tracks
  moved, copied and pasted by a range from the first part's start sample: the first part, the audio event and the audio
  part with its event landing on the position's sample with the part's first note and the modulation point on its
  downbeat, the second part a bar after the first in ticks, the other track's part on the second part's tick, and no
  piece left behind; at such tempos two parts on two tracks on the first and the last tick of the range's start sample,
  with modulation points on both ticks, and the piece of a third track's part cut there, the first of them in ticks
  landing at the position and the others and the points as many ticks after it as before, moved, copied and pasted, and
  the points in that sample gone from where they were after moving, cutting and deleting the range; parts on two bars
  cut and pasted where they were and moved to the range's own start, keeping their bars' ticks; `range.delete` up to the
  sample the next part starts in, with and without Delete Time, `range.insertSilence` from that sample and `range.crop`
  to that part's own samples, leaving it whole with the note and the modulation point on its downbeat; `range.delete`
  from the sample a terminator lies in, before the tick nearest to it, leaving the lane as it was; moving, copying and
  pasting to position 0 from the sample of a part on a bar, the part landing on tick 0, also with a modulation point in
  that sample before the part's tick or a lane whose copy starts at the range's start tick before it, those points
  landing on tick 0 too and the part's downbeat point last there; pasting to position 0 at a slower tempo than the
  copy's, the part landing on tick 0; moving to position 0 from, and Delete Time from sample 0 up to, the sample an
  audio part's event on a bar starts in, before the tick nearest to it, the event landing on tick 0 and the part's piece
  as many ticks after it as before; an audio part trimmed at its start with its take hidden a bar before it, Delete Time
  and moving it refused where they would put the take before the project start, and closing and landing by the whole
  length where they don't; `arranger.flatten` of a chain whose first event starts on a bar, the part on that bar keeping
  the bar's tick; and Delete Time with its end edge on a point before the end tick, the parts after the range and the
  whole lane compared: past a terminator a tick before the start tick, which moves onto the joint; from sample 0, with
  and without a lane and also as Cut Time, the part on the downbeat, the point on it and a transpose event landing on
  tick 0 and the next part on the next bar, and without the part a transpose event alone landing on tick 0 and a lane's
  point alone keeping the next part and the point on its downbeat together on the next bar; from sample 1 with a faster
  tempo at the end, the part and its point landing on tick 0 where the length would put them before it; and with a
  faster tempo at the end past a point just before the range's start sample, which moves onto the joint.
- `tests/eventedit_tests.cpp`, landing at a tick (#1785): `range.move`, its copy and `range.paste` at `positionTicks` of
  a bar, a part on another bar landing on that tick at every bar, also where the bar's sample puts it off the tick, and
  a linear part in that tick's sample, a part in a range that starts before it moved by the ticks from the range's start
  tick, and the errors for no position, both, a negative tick and one past the longest project; moving and pasting a
  musical track's range onto a linear track at position 0, its lane's points in the start sample before the part's tick
  or at the range's start tick landing on sample 0 with the part, the downbeat's point last there.
- `tests/eventedit_tests.cpp`, the range tests of the later rounds: `range.delete` and `range.copy` with a 44.1 kHz
  clip, taking the piece the cut at the end leaves; `range.paste` after `project.new`, refused; with a 22.05 kHz clip,
  an event at `end - 1` staying whole after `range.delete` and not copied, Delete Time moving it back by the range's
  length and Insert Silence at the range's end moving it on by the silence, and an event a sample before the start
  taken whole by `range.copy` and `range.delete`; `range.cut` leaving a locked event in place and out of the
  clipboard, which `range.copy` copies; pasting onto a lane without points, with terminators before and after the
  paste, none before it at position 0 and none with virgin territory; a `range.cut` in a failed `history.transaction`
  leaving the clipboard as it was; `range.crop` announcing the removal of the parts and events it takes away, not of
  the pieces after the end, also of an audio part with an event moved whole into such a piece; a range on a folder
  track editing the tracks inside it, and `range.paste` onto a track skipping a hidden one; `range.move` onto the two
  tracks below, past a hidden one, as `range.cut` and `range.paste` onto them would put it, with the automation and in
  one undo step, a copy there, and moves refused onto a track of the wrong type, past the last track and onto no track,
  changing nothing; `range.move` and `range.paste` of two tracks onto a folded folder's lane landing the second on
  the track after it, onto the folder's own track when it is unfolded, onto a track inside it, and a range on a folded
  folder going into the tracks of another folded folder or refused by a track; `range.paste` and
  `automation.paste` refused after a sample rate change and pasting again after its undo; an end of the largest integer
  deleting up to the end and a start past 2^48 refused.
- `tests/eventedit_tests.cpp`, the paste and controllers of #1910: `range.paste` with `selectedTrack` onto another
  audio track, back onto the event's own track from a MIDI track, and an audio event and a MIDI part split between the
  bottom track and the part's own track; with `snap` an event's snap point at the position, refused before the project
  start, and a range's copy at its start; a range cutting a MIDI part with control changes on two channels, pitch bend
  and a program change, its copy and the piece after it starting with the curves' values and no program change, the
  piece before it unchanged, and undo.
- `tests/eventediting_tests.cpp` (fake window system, real engine): the commands exist; without a range the range
  commands can't run, and Paste can't until a cut, nor after `project.new`; split, Delete Time, cut and paste on a
  selection range, which stays; paste at the cursor without a range, selecting what it pasted, and Paste at Origin where
  the range was cut, selecting it too; without a range, Cut and Copy of the selected part or audio event, enabled only
  with a selection, and Paste Relative to Cursor; Paste onto the first selected track with the event's snap point at
  the cursor, and onto the event's own track with the MIDI track selected; a drag in the range moving its contents on a MIDI track in one undo step with the
  range, snapped to a bar at a tempo whose bars lie between samples landing on the bar's tick, also with Grid Relative,
  and the range on the bar's sample, Alt and a drag copying them, a click in it selecting a range of no length, and
  Duplicate copying them to the range's end twice in a row; a drag down a lane and across, straight down and with Alt
  taking the part or a copy onto the track below, and one below the last lane stopping at the last track; a drag and a
  paste of two tracks past a folded folder's lane landing where the range shows them; the edges' pointers, and dragging
  the start and the end across, snapped and past the other edge, and the bottom and top down and up, taking tracks in
  and out and stopping at the other edge, while the project stays as it was; the nudges moving the range and its edges
  by a beat, not before the project start nor the start past the end, and the top and bottom nudges taking tracks in and
  out, never the last, while the project stays as it was.
- `tests/markertrack_ui_tests.cpp`: the same drag on the marker track, also released outside the window.
- `tests/uiapp_tests.cpp`: the context menu disables the range commands without a range, but not Cut and Copy with a
  selected part, and Paste until something is cut.
