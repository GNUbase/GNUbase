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
  new list) and `arranger.removeChain` (not the last one) are undo steps. `arranger.changed` announces every change with
  what `arranger.get` returns.
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
  and `arranger.trigger` (an event from the events list, which then loops until another one is chosen) take over at
  once while stopped or with the jump mode `none`, through a locate. While playing with another jump mode the
  transport keeps them waiting until the next jump point of the event playing: every 4 or 2 bars from its start (the
  first bar line at or after it), the next bar line, the next beat, or its end; at its end at the latest. Outside an
  event they take over at once. A step or event that waits survives a chain edit: the new plan finds it again by the
  event it plays, and drops it only when the chain lost that step.
- `transport.get` and `transport.changed` have `arranger`: the step and repeat playing and the arranger event playing,
  the step -1 and the event 0 both outside the chain and while the plan doesn't play (arranger mode off or an empty
  chain). Every new plan gets a generation number; the transport publishes its arranger state after every block, in
  one sequence lock with the generation its step and section indices walk, and a new step or repeat makes the engine
  announce `transport.changed`.
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
Range copies keep the order in which audio events stack, so overlapping events play as before.

## UI

- **Add Track** has Arranger, once per project.
- **The arranger editor** (`resources/ui/arranger-editor.rml`, the UI command `window.arranger`, in a window of its own)
  is driven by its own Lua script through the engine API: the toolbar switches arranger mode and picks the active chain
  and the jump mode, with Create New Chain, Duplicate Current Chain, Remove Current Chain and Flatten Chain, and the
  chain step buttons, which are disabled while they can't work (arranger mode off or an empty chain; First and Last
  Repeat also need a step of the chain playing). The Current Arranger Chain list has a row per step, with its locate
  arrow (a button that plays from the step's event, shown on every row, p. 410), its repeats and its mode, and a button
  that removes it; the Arranger Events list has a row per event in timeline order, whose arrow triggers it and whose
  Append button adds it to the chain. The row of the step or event playing is `.current`, highlighted by the theme.

## Checks

`tests/arranger_tests.cpp`: the transport along a plan for block sizes 1 to 1024 (steps, repeats, chain end, pause after
repeats, repeat forever with a step that waits for the next beat, a triggered event that loops, arranger mode against
the cycle, chain edits during the pre-roll, while the chain plays back into the project and without a block between
them - two back to back and 129 of them -, a stop and a cycle jump ending the pre-roll, a new plan finding the step
playing and a step waiting again by their events, a locate from a repeated step into another step's own first repeat,
a generation that wraps the packed field, and the generation of the plan the transport publishes with its state); the
commands with undo, settings and the arranger track's removal; saving and loading, with a file's repeats capped to 1
to 999; the chain playback render, sample by sample against the audio of each section, for two block sizes; real time
against offline, also with a chain edit while playing; choosing steps and triggering events while stopped and playing,
also across a chain edit that moves the chosen step; the plan a cue walks for the published generation, including
when neither kept plan is it; a cue that falls between two plan generations; `transport.play` ending a repeat forever
loop, keeping a triggered event and starting from the left locator with the start mode cycleStart, and `transport.get`
reporting no step while the plan doesn't play; flattening, played back linearly against the chain's audio, with undo.
`tests/lua/ui/arranger.lua` opens the arranger editor and drives it, with its step buttons disabled outside arranger
mode and a locate arrow on every row.

Not yet: arranger events on the arranger track's lane in the event display and drawing them with the draw tool,
the track list's arranger controls and the inspector's arranger section, renaming chains in the editor, drag and drop
in the chain list, the other flatten options (Checked and All Chains, New Project, Make Real Event Copies, Don't Split
Events, Rename Arranger Events), key commands for triggering events, and arranging to an external timecode.
