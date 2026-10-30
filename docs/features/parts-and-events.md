# Parts and events

Editing audio events and MIDI parts in the project window (#85, reference manual pp. 245–277): selecting, moving,
copying and shared copies, resizing, splitting, gluing, muting, locking, deleting, duplicating and repeating, nudging
and filling the locator range. The engine's commands are in `src/engine/document/eventedits.cpp`, the event display's
gestures and edit commands in `src/ui/eventediting.cpp` ([event-display.md](event-display.md)).

## Engine commands

The `event.*` commands take audio events and MIDI parts alike, by id (`events`), as the reference treats both as
events. Each is one undo step and changes nothing if it fails.

| Command | What it does | Reference |
|---|---|---|
| `event.move` | Moves by `delta` project samples or by `ticks`, and onto `track` (of the events' type) | pp. 256–259 |
| `event.copy` | Copies, moved like `event.move`; `shared` makes shared copies of MIDI parts | p. 270 |
| `event.duplicate` | `count` copies one after the other from the end of the last event; `shared` as above | pp. 270–272 |
| `event.fillLoop` | Copies on the grid of the events' length from `start` to `end` (default the locators), the last cut | p. 272 |
| `event.resize` | Moves the `edge` (start or end) by `delta` or `ticks`; `mode` normal or moveContents | pp. 264–268 |
| `event.split` | Splits at `positions` (project samples): the events, or every unlocked event a position cuts | pp. 268–269 |
| `event.glue` | Glues events of one track into the first | p. 269 |
| `event.set` | Mutes or unmutes, locks or unlocks | pp. 276–277 |
| `event.remove` | Deletes | p. 255 |
| `event.toRealCopy` | Turns shared copies into real ones: a part's own notes, an audio event's own clip | p. 272 |
| `event.get` | Where events are, in project samples and ticks, their name, mute, lock and shared group | p. 72 |

- **Positions.** Distances are project samples (`delta`), like the locators, or ticks (`ticks`), which are exact for
  events on musical tracks: a part moved by a bar in ticks starts on the bar. Blocks of events (duplicate, fill loop)
  are measured in ticks when every event is on a musical track, otherwise in samples. Nothing moves before the
  project start: a move or copy there fails, a resize stops at it.
- **Resizing** stops at the clip's start and end and keeps events at least a sample frame (a tick for parts) long.
  Normal sizing reveals or hides content: a part's start moved later moves its notes and controllers back, so they keep
  their time. Sizing moves contents moves the content with the edge: an audio event's offset, a part's notes. A part's
  notes and controllers that end up before its start are removed (they aren't kept hidden; parts have no offset yet).
  The reference's sizing applies time stretch waits for time stretching in the project window.
- **Splitting** an audio event gives the piece before the split its fade in, the piece after it its fade out; an event
  that crossfaded from it now crossfades from the piece it overlaps. A MIDI part's notes stay in the part they start in,
  sticking out of it, or with `splitNotes` (the reference's Split MIDI Events preference) are cut in two.
- **Gluing** audio events works where one continues the other in the same clip, as after a split, and makes one event;
  the reference makes an audio part of other events, which waits for audio parts. MIDI parts become one part from the
  first start to the last end, with every note and controller.
- **Muting.** Muted audio events and MIDI parts don't play (the render snapshot leaves muted parts out). In DAWproject a
  muted part, like a muted event, is a disabled clip.
- **Locks.** A locked event can't be moved, resized, split, glued or removed through the `event.*` commands; it can
  still be copied, muted and unlocked. Without `events`, `event.split` leaves locked events alone. The object commands
  (`audioEvent.set`, `midiPart.set`) stay low-level and don't check locks. The reference's lock attributes (position,
  length, other) come with the preferences.
- **Copies.** A copy is unlocked and has no crossfade; copies of audio events play the same clip.

## Shared copies

Shared copies of a MIDI part (p. 272) have the same `shared` group id and the same notes and controllers, with the
same ids. After every command (`ProjectModel::addEdit`, also inside transactions), the first part of a group whose
notes or controllers changed gives them to the others (`syncSharedParts`), so any command that edits notes, `note.set`
as much as a MIDI function or quantizing, edits all copies, as one undo step. A part whose contents change for its own
sake, by a resize that moves its notes, a split or a glue, first leaves its group with new note ids; so does
`event.toRealCopy`. A group's last part is no longer shared. The position, length, name, mute and lock stay each
part's own. Shared copies are saved in `gnubase.json`; `project.xml` gets them as ordinary clips.

## The event display

The display keeps a selection (`EventDisplay::selection`, ascending ids, also in the root's attribute `selection`
for MCP's `ui_dom`); it isn't part of the project or of undo. Selected events show again over the others in
`event-lanes.selected-events`, muted ones in `event-lanes.muted-events`, which the themes color (lit and greyed out).

The object selection tool's gestures (#86 brings the other tools):

| Gesture | Does | Reference |
|---|---|---|
| Click | Selects the event, or nothing on an empty spot | p. 253 |
| Ctrl (Cmd) or Shift and a click | Adds the event to the selection or takes it out | |
| Drag | Moves the selection, and onto the track it is dragged to when it is on one track | p. 256 |
| Alt and a drag; with Shift too | Copies the selection; shared copies of MIDI parts | pp. 270–271 |
| Drag within 6 dp of an event's left or right edge | Resizes the selection: normal sizing, or sizing moves contents (`setSizingMovesContents`) | p. 265 |
| Alt and a click | Splits the event there | p. 268 |
| Double click on an empty MIDI or instrument lane | Adds a one-bar part at the bar, or one between the locators when the click is between them | p. 251 |

The edit commands are in `actions()`, for the key command system to bind (#57): `edit.delete`, `edit.mute`,
`edit.unmute`, `edit.lock`, `edit.unlock`, `edit.splitAtCursor` and `edit.splitLoop` (the selection, or every event
there), `edit.glue`, `edit.duplicate`, `edit.fillLoop`, `edit.toRealCopy`, `edit.moveStartToCursor`,
`edit.moveEndToCursor`, the nudges `edit.nudgeLeft`, `edit.nudgeRight`, `edit.nudgeStartLeft`, `edit.nudgeStartRight`,
`edit.nudgeEndLeft` and `edit.nudgeEndRight` (a quarter note until the snap grid, #87, sets the step), and the select
submenu `edit.selectAll`, `edit.selectNone`, `edit.invertSelection`, `edit.selectInLoop`,
`edit.selectFromStartToCursor`, `edit.selectFromCursorToEnd` and `edit.selectUnderCursor` (on every track until the
track list, #81, selects tracks). `repeat(count, shared)` is the Repeat Events dialog's action. Copies, duplicates
and new parts become the selection.

Not yet: the info line (#89, which `event.get` feeds), a selection rectangle, the arrow keys' selection, the drag
delay, the Repeat Events dialog, default keys for the edit commands, a lock and a shared copy icon on events, and
moving a selection on several tracks to other tracks.

## Checks

- `tests/eventedit_tests.cpp`: every command on audio events and MIDI parts, in samples and ticks, undo, locks and
  the errors; real and shared copies, edits reaching every shared copy and undone together, real copies and the last
  copy; resizing both edges in both modes and its limits; splitting at the cursor and the locators, with and without
  splitting notes; gluing; filling the locators; `event.get`.
- `tests/eventediting_tests.cpp` (fake window system, real engine): selecting with clicks and Ctrl, moving and copying
  with drags, shared copies with Alt and Shift, onto another track; resizing at the edges, splitting with Alt, adding
  parts with a double click at a bar and between the locators; every edit command on the selection, the select
  submenu, muted events shown greyed out and locked ones kept.
- `tests/lua/events.lua`: a muted part is silent and plays again after undo; duplicating, shared copies, splitting and
  gluing from Lua.
