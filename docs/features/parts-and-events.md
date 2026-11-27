# Parts and events

Editing audio events and MIDI parts in the project window (#85, reference manual pp. 245–277): selecting, moving,
copying and shared copies, resizing, splitting, gluing, muting, locking, deleting, duplicating and repeating, nudging
and filling the locator range. The engine's commands are in `src/engine/document/eventedits.cpp`, the event display's
gestures and edit commands in `src/ui/eventediting.cpp` ([event-display.md](event-display.md)).

## Engine commands

The `event.*` commands take audio events, audio parts and MIDI parts alike, by id (`events`), as the reference treats
them all as events; [audio-parts.md](audio-parts.md) has what they do to audio parts. Each is one undo step and changes
nothing if it fails.

| Command | What it does | Reference |
|---|---|---|
| `event.move` | Moves by `delta` project samples or by `ticks`, and onto `track` (of the events' type) | pp. 256–259 |
| `event.copy` | Copies, moved like `event.move`; `shared` makes shared copies of parts | p. 270 |
| `event.duplicate` | `count` copies one after the other from the end of the last event; `shared` as above | pp. 270–272 |
| `event.fillLoop` | Copies on the grid of the events' length from `start` to `end` (default the locators), the last cut | p. 272 |
| `event.resize` | Moves the `edge` (start or end) by `delta` or `ticks`; `mode` normal or moveContents | pp. 264–268 |
| `event.previewResize` | What `event.resize` with the same parameters would make of the events, changing nothing | p. 265 |
| `event.split` | Splits at `positions` (project samples): the events, or every unlocked event a position cuts | pp. 268–269 |
| `event.glue` | Glues events of one track into the first | p. 269 |
| `event.set` | Mutes or unmutes, locks or unlocks | pp. 276–277 |
| `event.remove` | Deletes | p. 255 |
| `event.toRealCopy` | Turns shared copies into real ones: a part's own notes, an audio event's own clip, an audio part out of its group with its events' own clips | p. 272 |
| `event.get` | Where events are, in project samples and ticks, their name, mute, lock and shared group | p. 72 |

- **Ticks.** Events, parts and notes on musical tracks have their positions and lengths in ticks, 1,720,320 to the
  quarter note (`time::g_ticksPerQuarter`, so that every common note value is a whole number of them): a part from
  the second beat, a quarter note long, is `midiPart.add` with `position` and `length` 1720320, and 3840 ticks are
  about a millisecond at 120 BPM (#1855). On linear tracks they are in samples.
- **Positions.** Distances are project samples (`delta`), like the locators, or ticks (`ticks`), which are exact for
  events on musical tracks: a part moved by a bar in ticks starts on the bar. Blocks of events (duplicate, fill loop)
  are measured in ticks when every event is on a musical track, otherwise in samples; a block with no length in its
  unit (a frame of a 192 kHz clip in a 48 kHz project) is refused, since its copies would lie on top of it. Nothing
  moves before the project start: a move or copy there fails, a resize stops at it.
- **The longest project.** Positions and lengths end at the longest project's end, `longestPosition` in
  `src/engine/document/document.h`: `time::g_maxPosition` (2^48) samples, or in ticks as long as that lasts at the
  project's tempo (`longestProjectTicks`). `audioEvent.add` and `.set`, `audioPart.add`, `midiPart.add` and `.set`,
  `note.add`, `.set` and `.move`, automation points, markers and the lengths of `media.add` refuse more, and so does a
  move or copy that would take an event's start past it, or its end as far again; a cycle marker can't move its end
  past it either (#686). A resize keeps a length within it. Both loaders, the native project's and the DAWproject
  import's, cut event, part and note positions and lengths and media lengths there (`cutAtLongest`). A position plus
  a length then can't overflow, and an event of a clip at a far lower sample rate than the project's ends there when
  it plays (#1906).
- **Resizing** stops at the clip's start and end and keeps events at least a sample frame (a tick for parts) long.
  Normal sizing reveals or hides content: a part's start moved later moves its notes and controllers back, so they keep
  their time. Sizing moves contents moves the content with the edge: an audio event's offset, a part's notes. A part's
  notes and controllers that end up before its start are removed (they aren't kept hidden; parts have no offset yet).
  The reference's sizing applies time stretch waits for time stretching in the project window.
- **Splitting** an audio event gives the piece before the split its fade in, the piece after it its fade out; an event
  that crossfaded from it now crossfades from the piece it overlaps. A MIDI part's notes stay in the part they start in,
  sticking out of it, or with `splitNotes` (the reference's Split MIDI Events preference) are cut in two. Events and
  parts are split where they show, in samples: a position in the sample a part or event on a musical track starts or
  ends in, whose start or end in ticks can lie up to half a sample from it, doesn't split it, so no piece shorter than
  a sample is cut off with the note on its downbeat. The range commands, Delete Time, Insert Silence and the comp tool
  cut the same way ([range-editing.md](range-editing.md)).
- **Gluing** audio events where one continues the other in the same clip, as after a split, makes one event; other
  audio events and audio parts become one audio part ([audio-parts.md](audio-parts.md)). MIDI parts become one part
  from the first start to the last end, with every note and controller.
- **Muting.** Muted audio events and MIDI parts don't play (the render snapshot leaves muted parts out). In DAWproject a
  muted part, like a muted event, is a disabled clip.
- **Locks.** A locked event can't be moved, resized, split, glued or removed through the `event.*` commands, and
  `event.set` doesn't put it on another lane unless the same call unlocks it; it can still be copied, muted and
  unlocked. Without `events`, `event.split` leaves locked events alone. The object commands
  (`audioEvent.set`, `midiPart.set`) stay low-level and don't check locks. The reference's lock attributes (position,
  length, other) come with the preferences.
- **Copies.** A copy is unlocked and has no crossfade; copies of audio events play the same clip.

## Shared copies

Shared copies of a MIDI part (p. 272) have the same `shared` group id and the same notes and controllers, with the
same ids. After every command (`ProjectModel::addEdit`, and after each command of a `history.transaction`), the first
part of a group whose notes or controllers changed gives them to the others (`syncSharedParts`), so any command that
edits notes, `note.set` as much as a MIDI function or quantizing, edits all copies, as one undo step. A transaction
that edits two copies of a group keeps both edits, and a later command in it sees what an earlier one did to another
copy. For that, a transaction copies only the shared parts before each of its commands (`sharedPartsOf`), not every
note of every part: a part that becomes shared in a command gives no edit to its new group anyway. A part whose
contents change for its own sake, by a resize that moves its notes, a split or a glue, first leaves its group with new
note ids; so does `event.toRealCopy`. A group's last part is no longer shared. The position, length, name, mute and
lock stay each part's own. Shared copies are saved in `gnubase.json`; `project.xml` gets them as ordinary clips.

Shared copies of an audio part (#660) share its events, placed from the part's start. Since an audio part's events are
audio events of their own, with ids of their own, a copy can't hold the same ones: when the events of one copy of a
group change, by any command that edits audio events, the other copies' events change in place to match, at the same
places from their own starts, and those the edit added or removed are added or removed there too (`audioEvent.changed`,
`audioEvent.added`, `audioEvent.removed`). Each copy's events keep their ids, so a `history.transaction` can edit
events of several copies, and what is each copy's own: its place in the track's stack, so `event.toFront` on one copy
moves only that one, and its crossfades with events outside the part. Events are matched by what they share, as they
were before the command; `sharedPartsOf` copies the shared audio parts' events for that, and `syncSharedParts` indexes
the events by part once per command, not once per shared part. A copy leaves its group where
its events change against its edges for its own sake: when sizing its start without moving its contents or its end
while moving them, when it is split or glued, and with `event.toRealCopy`. A range paste and a track preset make real
copies, in every version of a preset's track; `track.duplicate` keeps the copied parts in their groups. A stashed track version keeps a group whose copies
all go into it, as it keeps MIDI parts' ([track-versions.md](track-versions.md)); a copy whose group has copies
elsewhere leaves it.

## The event display

The display keeps a selection (`EventDisplay::selection`, ascending ids, also in the root's attribute `selection`
for MCP's `ui_dom`); it isn't part of the project or of undo. Selected events show again over the others in
`event-lanes.selected-events`, muted ones in `event-lanes.muted-events`, which the themes color (lit and greyed out).
Locked events show a lock icon (p. 276) and shared copies of parts a link icon (p. 272) in their upper right
corner (#384): a `.part-mark.locked` and a `.part-mark.shared`, attribute `part` the event's id, in the event's
`.part-marks-of` in the display's `.part-marks`, next to the mark of events excluded from global transpose
([transpose.md](transpose.md)).

The object selection tool's gestures ([tools.md](tools.md) has the other tools, the selection rectangle and the tool
modifiers, which change the keys below):

| Gesture | Does | Reference |
|---|---|---|
| Click | Selects the event, or nothing on an empty spot | p. 253 |
| Ctrl (Cmd) or Shift and a click | Adds the event to the selection or takes it out, also where the keys are a drag's tool modifier (Copy, Shared Copy, Disable Snapping at an edge): a press with them released without a drag. Such a press leaves the selection as it is until the drag starts, and the release works out the click from the selection then, without events that went meanwhile | |
| Drag | Moves the selection, also onto other tracks; snapped ([snap.md](snap.md)) | p. 256 |
| Alt and a drag; with Shift too | Copies the selection; shared copies of parts | pp. 270–271 |
| Ctrl (Cmd) while dragging | Keeps a move or copy across or up and down, whichever the mouse went further | pp. 256, 271 |
| Left, Right | Selects the event before or after the selected one on its track | p. 253 |
| Up, Down | Selects the track above or below, and its first event; an empty track keeps the selected events | pp. 218, 253 |
| Drag within 6 dp of an event's left or right edge | Resizes the selection: normal sizing, or sizing moves contents (`setSizingMovesContents`); Escape cancels the drag | p. 265 |
| Drag of the repeat handle to the right; with Shift | Repeats the selection: real copies, shared ones with Shift (see below) | p. 271 |
| Alt and a drag of the last selected event's lower right corner to the right; with Shift too | The same: real copies, shared ones with Alt and Shift | p. 271 |
| Alt and a click | Splits the event there | p. 268 |
| Double click on an empty MIDI or instrument lane | Adds a one-bar part at the bar, or one between the locators when the click is between them | p. 251 |
| Double click on an empty audio lane between the locators | Adds an audio part from one locator to the other | p. 250 |
| Alt and a drag or click on an empty lane | Draws a part, as the draw tool does ([tools.md](tools.md)) | p. 250 |
| Double click below the tracks | Adds a MIDI track ([add-track.md](add-track.md)) | p. 198 |

Moving or copying a selection past the project start limits its shared offset after snapping: the earliest event
lands at zero, even when another event was pressed. Musical moves keep their tick distances; linear moves keep their
sample distances. The drag preview uses that same offset, also across tempo changes and onto other tracks. Left
nudges and moves to the cursor stop there too. Nudges keep the grid step's unit for each event, including mixed
musical and linear selections across tempo changes. Typed start positions in the info line and list editor reject
times before the project start. Direct `event.move` and `event.copy` requests with impossible positions still fail.

The edit commands are in `actions()`, for the key command system to bind (#57): `edit.delete`, `edit.mute`,
`edit.unmute`, `edit.lock`, `edit.unlock`, `edit.splitAtCursor` and `edit.splitLoop` (the selection, or every event
there), `edit.glue`, `edit.duplicate`, `edit.repeat`, `edit.fillLoop`, `edit.toRealCopy`, `edit.moveStartToCursor`,
`edit.moveEndToCursor`, the nudges `edit.nudgeLeft`, `edit.nudgeRight`, `edit.nudgeStartLeft`, `edit.nudgeStartRight`,
`edit.nudgeEndLeft` and `edit.nudgeEndRight` (a step of the snap grid, [snap.md](snap.md)), and the select
submenu `edit.selectAll`, `edit.selectNone`, `edit.invertSelection`, `edit.selectInLoop`,
`edit.selectFromStartToCursor`, `edit.selectFromCursorToEnd` and `edit.selectUnderCursor` (on every track, not yet only
on the track list's selected tracks). `edit.repeat` opens the Repeat Events dialog (`resources/ui/repeat-events.rml`,
reference manual pp. 271–272): a count from 1 to 1000 and Shared Copies, which OK passes to `repeat(count, shared)`.
The count is edited as the dialog opens, so Mod+K, a number and Enter repeat the selection.
Events also repeat by dragging (#2009, p. 271), on lanes at least two rows high: 80 dp, twice the default track
height, as GNUbase sets track heights in dp (`trackLayout.set`), not in rows. The repeat handle is an event's right
border (its 6 dp edge) from a quarter of the lane's height down to the middle, between the fade handle at the top and
the lower corner; over it the pointer is the pointing hand (`pointer` attribute `repeat`). A drag of it to the right
repeats the selection, or the pressed event if it isn't selected, and with Shift held at the press makes shared copies.
With Alt held, and Shift too for shared copies, the lower right corner of the last selected event (the lower half of
its 6 dp edge) repeats instead of resizing or splitting. The copies follow the drag back to back after the selection,
as many as the snapped distance covers in steps of the selection's length (from its first start to its last end),
rounded to the nearest; the selected events show them while the button is held, and the release makes them with
`repeat(count, shared)`, the dialog's `event.duplicate`, one undo step. Elsewhere the border still resizes, as before.
Copies, duplicates and new parts become the selection. `app::AppUi` binds the reference's keys for them in the project
window ([key-commands.md](key-commands.md)): Delete and Backspace delete, Mod+D duplicates, Mod+K repeats, Alt+X splits
at the cursor, Mod and the arrows nudge, and so on.

- **Drag delay** (p. 256). A press on events moves them only once it is held for 200 ms, so that a click doesn't
  move them by accident; a drag released sooner does nothing. The reference's Drag Delay setting comes with the
  preferences. `ui::Automation::drag`, and so MCP's `ui_drag` and the UI tests, holds the button long enough: it
  moves RmlUi's clock on by a second between the press and the first step.
- **Restricted drags.** With Ctrl (Cmd) held while dragging, a move or copy goes across, keeping its tracks, or up
  and down, keeping its time, whichever way the mouse went further. Ctrl held at the press counts too: a click,
  also one that moved a little before the drag delay, still adds the event to the selection or takes it out. With a
  copy's tool modifier and Ctrl (Alt and Ctrl), the press acts as with the tool modifier alone and the copy is
  restricted. Ctrl doesn't restrict when the keys held are a copy's tool modifier themselves ([tools.md](tools.md)). The reference's tool modifier for it comes with the
  preferences; until then it is always Ctrl (Cmd).
- **Onto other tracks.** Every selected event goes as many tracks up or down as the mouse, onto the lanes shown
  (hidden tracks don't count). It changes tracks only when every event lands on a track of its type (audio events
  on audio tracks, MIDI parts on MIDI and instrument tracks); otherwise it only moves in time. A selection on
  several tracks moves or copies with an `event.move` or `event.copy` per track in one `history.transaction`, one
  undo step.
- **Resizing shows the new size while the edge is dragged** (#732). Every step of the drag asks the engine
  `event.previewResize` with the parameters the release would send, one question on its way at a time (the latest
  one is asked when the answer is in), and the display draws the project with the answer's events in place of the
  selected ones: their boxes, waveforms (the part of the clip the new size plays), fades and volume lines, exactly
  as the release leaves them, at the clip's limits and the snap included. The release sends one `event.resize`, one
  undo step; the preview stays until the project shows the resize. The preview is asked again when the modifier keys
  change (Disable Snapping) or the project changes during the drag, e.g. by another client. A release after the
  pressed event went sends nothing. Escape cancels the drag: the events show their size again and the release sends
  nothing. The project window doesn't draw notes in parts or an info tip during
  drags yet, so a part's preview is its box.
- **The arrow keys** (pp. 218, 253): Left and Right (`edit.selectPrevious`, `edit.selectNext`) select the event
  before the first selected one or after the last on its track; at the first or last event, or with no event
  selected, they do nothing. Up and Down always step the track selection (`track.selectPrevious`,
  `track.selectNext`, [track-list.md](track-list.md)); while an event is selected, they also select the first event
  on the newly selected track. On a track without events the selected events stay selected, so the next Up or Down
  goes on selecting events. With the user setting `ui.upDownSelectsTracksOnly` they only select tracks.

Not yet: the info line (#89, which `event.get` feeds), and scrolling an event that the arrow keys select into view.

## Checks

- `tests/eventedit_tests.cpp`: every command on audio events and MIDI parts, in samples and ticks, undo, locks and
  the errors; real and shared copies, edits reaching every shared copy and undone together, real copies and the last
  copy; transactions in which parts become or stop being shared, edit parts that aren't shared, and undo and redo as
  one step; only shared parts copied for the comparison; resizing both edges in both modes and its limits; splitting
  at the cursor and the locators, with and without splitting notes; gluing; filling the locators; `event.get`;
  `event.previewResize` agreeing with `event.resize` at the clip's limits, in both modes, for parts and audio parts,
  and changing, announcing and recording nothing.
- `tests/document_tests.cpp`: positions and lengths at and past the longest project, refused by the commands and cut
  by loading, a native project and a DAWproject from elsewhere, resizes kept within it, markers refused past it on
  linear and musical marker tracks (#686), and the playlist of a clip at a far lower rate than the project's (#1906).
- `tests/eventediting_tests.cpp` (fake window system, real engine): selecting with clicks and Ctrl, moving and copying
  with drags, shared copies with Alt and Shift, onto another track; drags restricted with Ctrl, across and down, moving
  and copying, and Ctrl and a click still deselecting; Ctrl and a click adding to the selection and taking out at an
  edge (Disable Snapping) and with Ctrl as Copy, and a Ctrl drag then still copying; with Shift as Copy, the selection
  unchanged while the button is held; with Ctrl as Copy, a Ctrl press moved past the drag threshold and released before
  the drag delay as a click, one released after the drag delay without a move since, copying only the pressed event, not
  the selection from before the press, and no event that went while the button was held selected by the release; the
  drag delay (a drag released at once is a click, one held past it shows and moves); a selection on two tracks moved two
  tracks down and copied back in one undo step each, and only in time when a track's type doesn't fit; the arrow keys'
  commands along a track and at its ends; resizing at the edges, splitting with Alt, adding parts with a double click at
  a bar and between the locators; the edit commands on the selection (delete, mute, lock, nudging, duplicating, moving
  to the cursor, splitting at the cursor and the locators, gluing, filling the locators with the last copy cut,
  converting a shared copy to a real one), the select submenu, muted events shown greyed out and locked ones kept; the
  lock and shared icons inside the event's upper right corner, side by side, following `event.set`, a shared
  `event.copy`, `event.toRealCopy` and undo; repeating by dragging: the handle's pointer, a drag over two lengths
  showing and making two real copies in one undo step, with Shift shared ones, less than half a length none, Alt at the
  last selected event's lower right corner, and a lane lower than two rows resizing there instead; a
  drag of an audio event's end and start showing, at every step, the box, waveform, fades and volume line the release
  leaves: both edges in both sizing modes, snapped and with Disable Snapping pressed and released mid-drag, while
  another client moves the event; an audio event and a MIDI part selected together, and a second drag pressed before the
  display has the first one's project; a locked event staying as it is; Escape cancelling it. With the engine held,
  a second release before the first one's project reaches the display, and the project that lands a release coming in
  during the next drag with another client's move in it, which asks that drag's preview again.
- `tests/uiapp_tests.cpp` presses the default keys in the app's project window: select all, duplicate, mute, nudge
  and Backspace; Left and Right selecting parts, Up and Down stepping the tracks and selecting their first parts, also across a track
  without parts, and only the track with no part selected; and Mod+K,
  whose Repeat Events dialog makes three shared copies, and real copies from just a number typed and Enter; its count
  is edited as the dialog shows, before the next update, and again when Tab or Shift+Tab moves to it. It also adds
  MIDI and instrument parts while the start screen is open and after it closed, which show at their time and length (#1855).
- The project-start checks cover move and copy previews and release at 1024 by 600, single and multiple selections,
  snapping on and off, musical and linear tracks, other tracks, and one-step undo. The fake-window checks cover left
  nudges, moving to the cursor, a musical preview across a tempo change, and musical nudges of mixed audio/MIDI
  selections across a tempo change.
- `tests/lua/events.lua`: a muted part is silent and plays again after undo; duplicating, shared copies, splitting and
  gluing from Lua.
