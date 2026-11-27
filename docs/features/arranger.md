# Arranger track

The arranger track, its events and chains, arranger mode with its jump mode, flattening, and the arranger editor (#167,
reference manual pp. 164, 404-413).

## Engine

- **The arranger track** (`TrackType::Arranger`, `"arranger"`) has no channel, stays at the top level and is there at
  most once. Its events and chains are `Project::arranger`; removing the track removes them. Adding it gives it its
  first chain, "Chain 1", so a project with an arranger track always has a chain.
- **Arranger events** mark sections of the project: a name, a position and an end in the track's time base, at least
  one sample long. They can overlap and don't depend on events or parts. `arranger.add` (default: the locators; adds
  the arranger track if there is none), `arranger.set` (move, keeping the length unless `end` is given; rename) and
  `arranger.remove` (also removes their steps from every chain) are undo steps. New events are named in alphabetical
  order, the first free name of A to Z, AA, AB and so on (p. 405). Switching the track's time base and changing the
  sample rate keep them where they are, as for markers.
- **Chains** are lists of steps: an event, its repeats (1 to 999, and a project file's repeats are capped to that range
  when it loads) and a repeat mode, `normal`, `repeatForever` or `pauseAfterRepeats` (p. 407). `arranger.addChain`
  (Create New Chain, with `duplicate` Duplicate Current Chain; it becomes the active one), `arranger.setChain` (rename,
  or the whole list of steps: appending, moving, copying and removing steps and changing repeats and modes all send the
  new list; `removeSteps` removes steps by index, and `moveSteps` with `to` moves steps, with `copy` copies them, and
  `addEvents` inserts steps of arranger events before the step at `to` as the list was, as drag and drop does) and
  `arranger.removeChain` (not the last one) are undo steps. `arranger.changed` announces every change with what
  `arranger.get` returns.
- **Settings**: `arranger.setMode` switches arranger mode (`mode`), picks the active chain (`chain`) and the jump mode
  (`jumpMode`): saved with the project, not undo steps, like virgin territory.

## Playback

- The render snapshot carries an `ArrangerPlan` (`src/engine/audio/arranger.h`): every arranger event as a section in
  samples, the active chain's steps, arranger mode and the jump mode, and for each section the points where a jump that
  waits may happen. The audio engine hands it to `Transport::advance` with each block, and to every cue.
- **Arranger mode** with a chain that has steps replaces the cycle: the transport ends a segment at the exact sample
  where the step's event ends and goes on at the start of its next repeat or of the next step's event; a next event
  that starts where the last one ends plays on without a jump. After the last step playback goes on linearly from the
  end of its event. A step that pauses stops playback at the next step's start (the rest of the block is silent), and
  playback goes on from there. `arrangerEnter` decides where playback that starts or is located is in the chain: in the
  step playing if its event holds the position, else in the first step whose event holds it, at its own first repeat -
  the repeat playing belongs to the step, not to the event - else outside the chain, where it plays linearly. Playback
  enters at its start without the pre-roll, so a chain edit during the pre-roll keeps the chain playing. A new plan
  finds the step playing again by the event it plays, not by an index that a chain edit moved: the step keeps its
  repeat as far as the step found allows, a chain that plays one event in two steps goes on at the one playing, and a
  step that is gone leaves the position to decide. `transport.play` starts where the transport would (the cursor, or
  the left locator with the start mode cycleStart), and a start outside every step's event gives way to the chain:
  playback starts at its first step. A triggered event that loops where playback starts is kept. While playing,
  `transport.play` ends a Repeat Forever loop, going on with the next step or past the chain if the looping step is
  the last one (p. 407).
- **Jump mode** (pp. 411-412): `arranger.goToStep` (the chain list, Previous and Next Chain Step, First and Last Repeat)
  and `arranger.trigger` (an event from the events list, by its id or with `number` the Nth in timeline order, which
  then loops until another one is chosen) take over at once while stopped or with the jump mode `none`, through a
  locate. While playing with another jump mode the transport keeps them waiting until the next jump point of the event
  playing: every 4 or 2 bars from its start (the first bar line at or after it), the next bar line, the next beat, or
  its end; at its end at the latest. Outside an event they take over at once. A step or event that waits survives a
  chain edit: the new plan finds it again by the event it plays, and drops it only when the chain lost that step.
- `transport.get` and `transport.changed` have `arranger`: the step and repeat playing and the arranger event playing,
  the step -1 and the event 0 both outside the chain and while the plan doesn't play (arranger mode off or an empty
  chain). Every new plan gets a generation number, by which the transport tells a new plan from the one it played,
  not by its address, which a new plan may get once the old one is freed (#1479); it publishes its arranger state
  after every block, in one sequence lock with the generation its step and section indices walk, and a new step or
  repeat makes the engine announce `transport.changed`.
- **Cueing**: `Playback::points` and `Playback::ahead` walk the same path as the transport, through cycle jumps or
  arranger turns (`arrangerNext`), so the voices wait where each jump lands and real-time playback renders the same as
  offline. Sections need to be longer than a few cue intervals (5 ms each) for that; every musical section is. A cue
  walks the plan of the generation the transport's published state walks, not the newest one, so a chain edit the
  audio thread hasn't played yet doesn't cue with the old plan's indices. The cue state keeps the newest plan and the
  one before it, the previous one while the published state still walks it; the command thread rotates the two at a
  rebuild, so a plan is freed there and never on the audio thread. A cue whose generation is neither of the two skips
  the pass instead of walking a plan the indices don't mean - the next pass catches up - while an exact cue that the
  command thread makes grounds the state in the newest plan at its position instead of skipping.

## Flatten

`arranger.flatten` (Flatten Chain, pp. 407-411) makes the project play the chain linearly, in one undo step: from the
start of the chain's first event on, every track's events, parts and automation are replaced by the chain's sections,
each step's event as often as it repeats, one after the other, copied as range copies do ([range-editing.md]
(range-editing.md)). What lies before stays, and so do locked events. Without `keepTrack` (Keep Arranger Track) the
arranger track goes; with it, it has an event per section where it now is and one chain that plays them in order.
Range copies keep the order in which audio events stack, so overlapping events play as before. The parts an event's
repeats bring, from its second section on, are shared copies of those its first section brought, MIDI and audio parts
alike ([parts-and-events.md](parts-and-events.md)); with `realCopies` (Make Real Event Copies, p. 408) each is a part of
its own.
MIDI notes that cross a section's edges are cut there, and so are those that cross the chain's start, as range splits
with `splitNotes` cut them; with `dontSplit` (Don't Split Events, p. 408) a section has only the notes that start and
end in it, and what lies before keeps its notes whole. With `keepTrack` and `renameEvents` (Rename Arranger Events)
each section's event has a number after its name, how often its event came so far: A 1, B 1, A 2.

## UI

- **Add Track** has Arranger, once per project.
- **The track list's arranger controls** (p. 164, [track-list.md](track-list.md)): `arrangerMode` (Activate Arranger
  Mode), Switch Time Base, `arrangerChain` (Select Active Arranger Chain, `arranger.setMode`), `arrangerEditor` (Open
  Arranger Editor, which opens the arranger editor or brings it to the front, `AppUi::openArranger`) and the displays
  `arrangerItem` (Current Item, the name of the arranger event playing) and `arrangerRepeat` (Current Repeat, the
  repeat playing of the step's repeats, such as 2/4), which follow `transport.changed`'s `arranger` and are empty
  outside the chain; they only show, so a double click doesn't edit them. They also read `transport.get` on
  `arranger.changed`, as turning arranger mode off announces no `transport.changed`. The arranger track's controls area
  is 160 dp wide by default, which shows them on two lines, Current Item and Current Repeat narrow beside each other
  and Open Arranger Editor.
- **The inspector's Arranger Track section** (pp. 405, 409-411, [inspector.md](inspector.md)), for the arranger track:
  Open Arranger Editor (`.inspector-arranger-editor`) beside its header, which doesn't close the section; the Current
  Arranger Chain list, a row per step of the active chain with its arrow (`arranger.goToStep`), its event's name, its
  repeats, a `textdisplay` that a double click types (capped to 1 to 999; text that isn't a number keeps them), and its
  repeat mode, a drop-down list, both sent as the chain's new steps (`arranger.setChain`); a right click on a step
  opens `#inspector-arranger-step-menu`, whose Remove Touched removes that step (`removeSteps`) and is disabled beside
  the steps; the Arranger Events list, a row per event with its arrow (`arranger.trigger`) and name; and Jump Mode
  (`arranger.setMode` `jumpMode`). The arrows are disabled while they can't work (arranger mode off or an empty chain),
  and the row of the step or event playing is `.current`. Like the track list's controls it follows
  `arranger.changed` and `transport.changed`, and reads `transport.get` on `arranger.changed`. A list is built again
  only when what it shows changed, so a name being typed stays.
- **Selecting steps in a chain list** (p. 410): in the inspector's and the arranger editor's Current Arranger Chain
  list a click on a step, not on its arrow, buttons or repeat mode, selects it (`.selected`, which the themes tint
  with their accent), with Shift the steps from the one clicked last, with Ctrl (Cmd) adds or removes it. Remove Selected on the
  list's context menu (`.arranger-remove-selected` in the inspector's, `#chain-step-menu .remove-selected` in the
  editor's) removes them (`removeSteps`), disabled in the inspector without a selection. The steps are selected by
  index, so a chain edit selects none. The editor selects on the press, as a button's click builds its list again; a
  press on a step selected keeps the others, and its release, a click, selects it alone.
- **Drag and drop in a chain list** (pp. 409-410): in both Current Arranger Chain lists a step dragged onto another
  step's upper half goes before it, onto its lower half after it, and beside the steps after the last one
  (`moveSteps`); with Alt held at the release it is copied (`copy`), and if it is selected, all selected steps go,
  in their order. An event dragged from the Arranger Events list goes in so (`addEvents`). The press is on a step's or
  event's row, not on its buttons or repeat mode, and a release on the step pressed or outside the chain list does
  nothing. The editor's script works out a row's top from the offset parents, as RmlUi's Lua elements have no absolute
  offset. Events dragged from the project window don't drop there yet.
- **Renaming arranger events in a chain list** (p. 405): in the inspector's and the arranger editor's Current Arranger
  Chain list a step's name is a disabled `textdisplay` that Alt and a double click open, and a plain double click
  doesn't; Enter renames the step's event (`arranger.set`), an empty name keeps the old one. The editor doesn't build
  its chain list again while a name is being typed.
- **The arranger track's lane** (`ui::GlobalEventLane` with `LaneKind::Arranger`, `src/ui/transposetrack.h`, shared with
  the transpose and chord tracks' lanes, #1393, pp. 404-405): an `.arranger-lane-event` per arranger event in an
  `.arranger-lane-row` over the lane, as wide as its section, with its name. The class names are `arranger-lane-*`, not
  `arranger-*`, because `.arranger-row` is a row of the arranger editor's lists. The draw tool draws an event from the
  press to the release (`arranger.add`, named in alphabetical order), also over other arranger events, as they may
  overlap (p. 404), and shows it as an `.arranger-lane-event.drawing` while the button is down, which the themes draw as
  an outline, unlike the events the engine has. A click without a drag draws one from where it snaps, or the grid line
  at or before it if that is after it, up to the bar line after the point clicked, so that it holds that point also when
  its start snapped back over a bar line to another event's edge. With the other tools a drag moves an event, keeping
  its length (`arranger.set`), and the event follows the mouse while the button is down; a double click opens its name
  in the `.arranger-lane-field` over it, as wide as the event and at least wide enough for a name, where Enter renames
  it (an empty name keeps the old one); the erase tool removes the event clicked (`arranger.remove`). A click selects an
  event (`.selected`, which the themes fill with the selection's color), with Ctrl (Cmd) adds or removes it, and a press
  beside the events selects none; a drag of an event's edge, an `.arranger-lane-edge` (4 dp wide, with the resize
  cursor), resizes it (`arranger.set` with `position` and `end`), snapped as a move is, the edge not past the other one.
  A drag of one of several selected events moves them all by as much in the track's time base, none before the project
  start, in one undo step (`history.transaction` of `arranger.set`), and they all follow the mouse; Delete removes the
  selected events (`arranger.remove`, #2076). Arranger events selected deselect the events, markers, time signature
  events, automation points and the selection range, and each of those selected deselects the arranger events, so that
  Delete deletes what was selected last.
  Drawing and moving snap as the event display's drags do ([snap.md](snap.md), `EventDisplay::snapped`), with the other
  arranger events' edges as magnets too, so with Snap on and the snap type Events, as p. 404 asks, a new event starts
  and ends at other arranger events', parts' and markers' edges. What the drag shows stays until the lane shows the
  event as the engine has it. When the engine refuses an edit, such as a drawn event shorter than a sample, the
  display's `#edit-refused` says why ([event-display.md](event-display.md)).
- **Renaming arranger events on the info line** (p. 405, [inspector.md](inspector.md)): while the arranger events
  selected on the lane changed last, the info line has the class `arranger` and shows the first of them in timeline
  order as it shows a cycle marker: its name (labelled Name), start, end and length, which `arranger.set` edits for
  every selected one in one undo step, relative to the first one's value or with Ctrl+Enter the same for all; a new
  start keeps an event's length; an empty name keeps their names. Selecting an event or a marker deselects the
  arranger events.
- **Key commands** (p. 411): the key commands dialog lists Trigger Arranger Event 1 to 9 in the arranger area,
  `arranger.trigger` with `number` 1 to 9, without default keys ([key-commands.md](key-commands.md)), so the user
  gives them keys.
- **The arranger editor** (`resources/ui/arranger-editor.rml`, the UI command `window.arranger`, in a window of its own)
  is driven by its own Lua script through the engine API: the toolbar switches arranger mode and picks the active chain
  and the jump mode, with the active chain's name in `#chain-name` (Rename Arranger Chain, p. 406: Enter renames it,
  an empty name keeps the old one; its drop-down lists are filled through the select's own API, as setting their
  `inner_rml` would add the options to those they have), Create New Chain, Duplicate Current Chain, Remove Current Chain and Flatten Chain, and the
  chain step buttons, which are disabled while they can't work (arranger mode off or an empty chain; First and Last
  Repeat also need a step of the chain playing). The toolbar wraps onto more rows when the window is narrow.
  Flatten Options (Flatten with Options & Preferences, pp. 408, 410) shows the flatten options in place of the lists:
  Keep Arranger Track, Rename Arranger Events (only with Keep Arranger Track), Make Real Event Copies and Don't Split
  Events (`keepTrack`, `renameEvents`, `realCopies`, `dontSplit`), with Flatten and Go Back. Flatten Chain flattens
  with them too; Go Back and closing the editor keep them while GNUbase runs. The Current Arranger Chain list has a row per step, with its locate
  arrow (a button that plays from the step's event, shown on every row, p. 410), its repeats and its mode, and a button
  that removes it; the Arranger Events list has a row per event in timeline order, whose arrow triggers it and whose
  Append button adds it to the chain. The row of the step or event playing is `.current`, highlighted by the theme.

## Checks

`tests/arranger_tests.cpp`: the transport along a plan for block sizes 1 to 1024 (steps, repeats, chain end, pause after
repeats, repeat forever with a step that waits for the next beat, a triggered event that loops, arranger mode against
the cycle, chain edits during the pre-roll, while the chain plays back into the project and without a block between
them - two back to back and 129 of them -, a stop and a cycle jump ending the pre-roll, a new plan finding the step
playing and a step waiting again by their events, a locate from a repeated step into another step's own first repeat,
a generation that wraps the packed field, a new plan at the old one's address, and the generation of the plan the
transport publishes with its state); the commands with undo, settings and the arranger track's removal;
`arranger.setChain` moving, copying and inserting steps; saving and
loading, with a file's repeats capped to 1 to 999; the chain playback render, sample by sample against the audio of
each section, for two block sizes; real time against offline, also with a chain edit while playing; choosing steps and
triggering events while stopped and playing, also across a chain edit that moves the chosen step; the plan a cue walks
for the published generation, including when neither kept plan is it; a cue that falls between two plan generations;
triggering events by their number in timeline order;
`transport.play` ending a repeat forever loop, keeping a triggered event and starting from the left locator with the
start mode cycleStart, and `transport.get` reporting no step while the plan doesn't play; flattening, played back
linearly against the chain's audio, with undo, and its notes cut at the sections' edges, without `dontSplit` and with
it, with `renameEvents` numbering the events, and an event's repeats as shared copies of its parts or with
`realCopies` real ones.
`tests/lua/ui/arranger.lua` opens the arranger editor and drives it, with its step buttons disabled outside arranger
mode, a locate arrow on every row, renaming the active chain, and renaming a step's event with Alt and a double click,
across a refresh while it is typed, selecting steps and removing the selected ones, a chain edit selecting none, and
moving, copying and inserting steps by drag and drop, a click on one of the steps selected selecting it alone, and
flattening with the flatten options, which Go Back keeps.
`tests/arranger_ui_tests.cpp`: the arranger track's lane showing an event as wide as
its section with its name, the draw tool drawing one over the stretch dragged and, with a click, from the bar line at or
before it to the next one, both snapped to bars; with the snap type Events, drawing from and to other arranger events'
edges, the event drawn showing while the button is down, a click just after a bar line whose start snaps back to an edge
before it drawing up to the next bar line, and a drag that starts on an event drawing over it; a drag moving an event by
bars, the event following the mouse until the release, a double click renaming it in a field as wide as the event, an
empty name keeping the old one, and the erase tool removing it; a click selecting an event, Ctrl adding and removing
one, a press beside them selecting none, and drags of an end and of a start edge resizing events by bars;
`#edit-refused` saying why the engine refused a draw within one bar; the track list's arranger controls switching
arranger mode and the active chain and showing the event and repeat playing; Open Arranger Editor in the track list and
on the inspector's section opening the editor and leaving it open; the inspector's section listing the steps and events,
its arrows disabled outside arranger mode, playing a step and triggering an event with their rows `.current`, Jump Mode
both ways, its header closing and opening it, and renaming a step's event with Alt and a double click, not with a plain
one, across a refresh that changes nothing the lists show; typing a step's repeats, capped to 999 and kept for text that
isn't a number, picking its repeat mode and removing it with Remove Touched; the info line showing the arranger events
selected on the lane, renaming, moving and resizing them, the first or both, and a part selected deselecting them;
selecting steps with clicks, Shift and Ctrl, the arrow leaving the selection, and Remove Selected, disabled without a
selection; arranger events selected deselecting a part, a marker, a time signature event and the range, a range and a
time signature event selected deselecting them, a drag of one of two selected moving both in one undo step, and Delete
removing them; the inspector's chain list moving steps dragged before or after a step, copying them with Alt, moving the
selected ones together and inserting an event dragged from the Arranger Events list, a drag back onto the step pressed
or out of the list doing nothing.
`tests/theme_tests.cpp`: every theme fills a selected arranger event.

Both real-time comparisons in `tests/arranger_tests.cpp` retry runs that report playback dropouts, with at most five
attempts and a fresh song and audio file per attempt. Each retry emits a warning. The chain-edit comparison makes its
edit in the first half of B, judged by the transport's position rather than the device's, so that the audio thread
adopts it while B plays; an attempt that misses that stretch, or is still making the edit when it ends, for example
because a busy machine held up the test, is retried like one with dropouts. It compares only a capture with the edit
made in time and without dropouts; exhausting the attempts fails the test (#1893, #1900).

Not yet: dropping arranger events from the project window into a chain list, the other flatten options (Checked and
All Chains, New Project, Make Real Event Copies) and a dialog for them in the editor, and arranging to an external
timecode.
