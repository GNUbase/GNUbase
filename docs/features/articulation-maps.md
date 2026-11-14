# Articulation maps

Articulation maps (#175, reference manual pp. 1262-1292) say which articulations a track's notes have and how the
instrument plays them: each note plays the sound slot of its articulations, which sends the instrument the output
events that switch it to that sound and changes the note as the slot's MIDI modifiers say.

## In the engine

**Articulation maps** (`src/engine/midi/articulationmaps.h`) are part of the project (`Project::articulationMaps`):
`project.get` lists them as `articulationMaps`, they are saved in the project's GNUbase entry, and changing them is an
undo step. A new project has none.

| Map field | What it is | Reference |
|---|---|---|
| `name` | The map's name | p. 1267 |
| `groups` | The mutual exclusion groups' names, the first the most important; at least one | pp. 1278-1279, 1283 |
| `articulations` | `{name, type, group}`: a `direction` or an `attribute`, and its group's index | pp. 1262, 1279 |
| `slots` | The sound slots, the first the default slot (below) | pp. 1268-1272 |
| `remoteTrigger` | `keySwitch` or `programChange`: what selects slots from the MIDI input | p. 1268 |
| `latch` | A key switch's slot stays after the key's release; off: the default slot comes back | p. 1268 |

| Slot field | What it is | Reference |
|---|---|---|
| `name`, `color` | The slot's name and color (`#rrggbb` or empty) | p. 1269 |
| `articulations` | The articulations it plays, by name, at most one of each group | p. 1269 |
| `trigger` | The remote trigger: a key or a program, 0-127, or `null` | pp. 1269, 1282 |
| `outputs` | The output mapping: `{status, data1, data2, off}`, `status` `note` (a key switch), `controller` or `program`; `off` events go when the slot ends, the others when it starts | pp. 1270-1271 |
| `channel` | 0-15, the channel the notes play on, or `null` for their own | p. 1271 |
| `length`, `velocity` | Percent of the notes' length and velocity, 1-1000 | pp. 1271-1272 |
| `transpose` | Semitones | p. 1271 |

| Command | What it does | Reference |
|---|---|---|
| `articulationMap.add` | Add Map (New Map with Group 1) or Duplicate Map (`copy`), with the settings given | pp. 1267, 1280 |
| `articulationMap.set` | Changes a map; `articulations` and `slots` replace the whole lists | pp. 1268-1279 |
| `articulationMap.remove` | Removes a map; the tracks that used it have none | p. 1267 |
| `articulationMap.export` | Saves a map as an articulation map file (`.articulationmap`) | p. 1292 |
| `articulationMap.import` | Loads an articulation map file into the project as a new map | p. 1283 |
| `articulationMap.soundSlots` | The sound slot each note of a part plays (`midi::soundSlots`), the sound slots lane's, for MCP and Lua clients | p. 1287 |
| `track.set articulationMap` | The map of a MIDI or instrument track, 0 for none | p. 1286 |

They emit `articulationMap.added`, `articulationMap.changed` and `articulationMap.removed` with the map (its id for
removed).

- **Renaming and removing articulations** (`articulationMap.set`). An articulation renamed at its place in the list
  (a list as long as before, its old name gone, the new one not the map's before), as the setup renames them, is
  renamed in the parts of the tracks that use the map, their shared copies and other track versions: their
  directions and the notes' attributes. One that is left out is removed from them. Both are part of the map's undo
  step.

- **Articulations in parts** (pp. 1287-1291). A direction is a controller event of the type `articulation`
  (`controller.add`, `controller.set`) with the direction's name; with `reset` it ends the directions of its group
  instead (p. 1289). Directions are part of the part's controller events, so they move, copy, split, glue and undo
  with them; they play nothing themselves and MIDI files and DAWproject leave them out. Attributes are the notes'
  `articulations` (`note.add`, `note.set`), by name. Names that aren't the map's articulations play nothing (p. 1291).
- **Playback** (`src/engine/document/rendersnapshot.cpp`). Each note of a track with a map plays a sound slot
  (`midi::soundSlots`): of each group, its attribute or else the direction in effect at its start; the slot with
  exactly those articulations, else the slot that has those of the more important groups, then the fewest others,
  then the first: with no match the default slot (pp. 1262, 1278). Where a note's slot differs from the last one its
  track sent, across the track's parts in the order of the notes' starts (and at the track's first note), the off events
  of that slot and the new slot's on events play (#1491): controller and program changes where the note starts, key
  switches for one sample just before it, and at one sample before the notes that start there, whatever their pitch (at
  the project's start a key switch can't come a sample before). Notes that don't play (their probability, a muted drum
  sound) select no slot and send no output events. Where playback starts or jumps, the controller and program changes
  are chased like any others, and with note chasing (`midi.chaseNotes`) the last key switch of each pitch before the
  position plays again there for a sample, in their order, before the notes. The slot's channel, transpose, velocity and
  length apply to the note before the track's MIDI modifiers. Merge MIDI in Loop (`midi.merge`) starts the new part with
  the directions of each group in effect where the range starts, and where a part starts later in the range it adds a
  reset of each group of that part's track's map in which a direction is in effect there in the new part, as playback
  starts each part with the default slot, so that its notes play the same slots. Where merge makes more than one new
  part (#1240), each of them gets these directions and resets; as playback sends the output events across a track's
  parts, the new parts send the key switches the source parts did, also where they lie over each other across the range.
  A new track for the merged part gets the map of the first merged track that has one. The directions of several tracks
  with maps share the new part's one lane, so they can't all keep their slots: a direction of one track's part also
  selects the slots of the other tracks' notes after it, and the reset where another track's part starts ends it, even
  where its own part still plays.
- **Remote triggers** (pp. 1263, 1268, 1282). In what a recording brings to a track with a map (`recording.add`), the
  notes whose pitch is a slot's trigger (Key Switch), or the program changes whose program is one (Program Change),
  aren't recorded; they select their slot instead: a direction for each group that has directions, the slot's own or
  a reset, where they come, and the slot's attributes on the notes that start while it is selected. Without latch
  the default slot comes back where the key is released.
- **Remote triggers on the MIDI input** (pp. 1263, 1268; `MidiSequencer::triggerSlot`, on the audio thread). On a
  track with a map, a trigger key or program played into the track (its MIDI input, `midi.inject`) doesn't reach the
  instrument: it selects its slot, which sends the off events of the slot before and its own on events (a key switch
  as a note-on and its note-off), on the slot's channel, else the MIDI track's channel, else the trigger's. The notes
  played after it get the slot's channel, transpose and velocity before the track's MIDI modifiers; the default
  slot's until a trigger comes, and again once another map is assigned or what the map's slots play from the input
  changes: their triggers, output mappings, channels, transpose and velocity, not their names, colors, articulations
  or length. A trigger key's note-off still ends the note its note-on played before the key became a trigger (a map
  assigned, or the key made a trigger, while it was held). Without latch the default slot comes back where the key is
  released, on the same channel. A MIDI track set to another channel (`channel.set` `midiChannel`) sends the selected
  slot's off events on the channel its on events went to and its on events on the new one, unless the slot has a channel
  of its own, so the instrument there has them before the next note. What is recorded keeps the triggers for
  `recording.add`, which turns them into directions and attributes.
- **Articulation map files** are JSON: the map as `articulationMap.set` takes it, with `"format":
  "gnubase.articulationMap"`. Track presets and track import bring a track's map along, or use the project's map
  that is the same (`src/engine/document/trackpresets.cpp`).

## In the UI

- **Articulation Map Setup** (MIDI > Articulation Map Setup..., the UI command `midi.articulationMapSetup`, and the
  inspector's Setup... button; `resources/ui/articulation-maps.rml`, pp. 1264-1283). The project's maps on the left:
  Add Map, Duplicate Map, Remove Map, Import Map and Export Map (the OS's file dialogs through
  `gnubase.ui.importArticulationMap` and `gnubase.ui.exportArticulationMap`; Export Map offers the map's name as the
  file name, with the characters file names can't have replaced, `engine::safeFileName`), and Assign to Track for the
  selected MIDI or instrument track. On the right the selected map: its name, Remote Trigger (Key Switch or Program
  Change) and latch; its groups; its articulations with their type and group; its sound slots with their articulations
  (names separated by commas) and remote trigger; the output mapping (status, data 1, data 2, on or off events) and
  MIDI modifiers (channel 1-16 or empty for the notes' own, length and velocity in percent, transpose) of the slot
  picked with its radio button. Each change goes to `articulationMap.set` at once, text fields with Enter or when they
  lose the focus; a change the engine refuses shows its message and the field as it was, and so does an import or
  export that fails. While it is open it follows `articulationMap.added`, `changed` and `removed` and
  `project.reset`, so it shows the maps after an import, an undo or a change through MCP or Lua, and selects a map
  that comes; it unsubscribes its handlers when it closes (`gnubase.off`), as the profile manager and the workspaces
  organizer do. Like them it opens once (`AppUi::openOnce`): opening it again, from another window's inspector or
  the UI command through MCP, brings the open one to the front, so that one set of handlers follows the engine.
  Opened again for another track, it is that track's setup: it selects the track's map, if it has one, and Assign
  to Track assigns to it (the setup's `track` attribute and its `trackchange` event). Opened again without a MIDI or
  instrument track selected, it has no track, as when it opens without one, and hides Assign to Track. Opened again
  for the same track, it keeps the map it shows. A new or loaded project (`project.reset`) drops its track too, since
  object ids are per project: Assign to Track would otherwise assign to whatever track has that id in the new project.
- **Inspector** ([inspector.md](inspector.md), p. 1263): MIDI and instrument tracks' Articulation Map section chooses
  the track's map, or No Map (`track.set articulationMap`), and opens the setup.
- **Articulations lane** (`ui::ControllerLanes`, pp. 1287-1289): the lane type `articulations` of the key, drum and
  in-place editors' controller lanes (`editor.set lanes`, the Create Controller Lane menu's Articulations). The map is
  the active part's track's, else the first shown part's track's; the lane shows and edits only the parts whose track
  has that map (#1498). Its first row is the sound slots lane: each note's slot (`.sound-slot[part][note]`, its name
  as text and title) from its start to its end, as `midi::soundSlots` chooses it from the map and the part as
  `project.get` lists them (`articulationMap.soundSlots` gives MCP and Lua clients the same), in the slot's color
  where it has one (the only color code sets there, user data like the track colors), with the state class `.light`
  or `.dark` for a light or dark slot color, on which the themes write the slot's name in a dark or a light color.
  The lane keeps its elements while their RML is the same, so code also takes away a width an element no longer has;
  a slot that loses its color loses its class `.light` or `.dark` with it, so its elements are new and have none.
  A row per articulation of the map follows (`.articulation-row[articulation-row]`, its name beside it in the lane's
  header): directions
  (`.articulation-item.direction[part][controller]`) last until the next direction of their group or the part's end,
  a reset (`.reset`) is as wide as the style makes it (p. 1290); attributes (`.articulation-item.attribute[part][note]`)
  are on their notes, as long as them. Directions and attributes whose names aren't the map's don't show (p. 1291).
  With the draw tool a click in a direction's row adds it on the quantize grid (Ctrl: off it) to the part there
  (`controller.add`), but not where the row's direction already starts, in an attribute's row to the notes that
  sound there and started last, in place of their attribute of the same group (`note.set`); a click on a direction
  or attribute removes it, as the erase tool's does. The lane menu's Select All Events on Lane selects the lane's
  directions.
- **Articulations field** (p. 1290): the key and drum editors' info line shows the first selected note's attributes,
  names separated by commas; typed, they replace every selected note's, and typed empty, they go. The info line and
  the list editor join and split the names with `ui::joinNames` and `ui::splitNames` (`ui/articulationnames.h`).
- **List editor** (p. 1291, [list-editor.md](list-editor.md)): directions are events of the type Articulation with the
  direction's name in the Attributes column, without a channel; a note's Attributes column has its attributes. Both
  can be typed there.
- **Empty lists from Lua.** Lua has no empty list apart from an empty table, which becomes a JSON object: where a
  command's parameters take a list, the Lua functions make it an empty list (`src/scripting/runtime.cpp`), so that the
  setup can take the last articulation of a slot away.

## Not yet

The Articulations lane's Collapse All Groups, Expand All Groups and Show/Hide Sound Slots Lane, its object
selection tool, the event pop-up menu's Reset, keyboard focus to delete an attribute and the mute tool's muted
articulations (pp. 1287-1290); the info line's and the list editor's pop-up menus of the attributes, which are typed
as names for now, and the warning sign for attributes the map doesn't have (p. 1291); the project window's info line
for the in-place editor's notes; add-on slots, the MIDI modifiers Attack Compensation, Separation and the pitch and
velocity ranges; playback techniques, aliases and fallbacks, symbols, articulation combinations beyond one per group;
a slot's length for the notes played into a track, which aren't known to end yet; Sound Slot event colors; Transpose
All and Reassign All Remote Triggers; the articulation combinations dialog, and the maps list's search and "used in
this project" filter; Import Key Switches from instrument presets; importing the reference's own files and other
applications' maps.

## Checks

- `tests/articulationmaps_ui_tests.cpp` drives the app with `ui::Automation`: the inspector's section opens the setup,
  which adds a map, renames it, adds an articulation and a slot with it, a remote trigger, an output mapping and a
  transpose, shows a refused change's message, assigns the map to the track; the inspector takes it off again, and the
  MIDI menu's setup removes it. Its second test case shows changes from elsewhere (an add, a rename, an undo), exports
  and imports a map, which the setup selects, shows why an import and an export failed, and offers a map's name with a
  slash as a file name without it. Its third test case opens the setup, the profile manager and the workspaces organizer
  twice each and counts their Lua handlers (`gnubase.on`): one set while open, none after. Its fourth test case
  opens the setup for one track, another and the first again, and assigns the map it selected to the last. Its fifth
  opens it again for the same track, which keeps the map browsed to, then without a track, which hides Assign to
  Track, then for the track again. Its sixth opens it fresh without a track, which hides Assign to Track, then for
  the track and starts a new project, which drops the track, then for a track of the new project. Its seventh to
  tenth test cases check the Articulations lane: the sound slots each note plays in their color, a direction and an
  attribute drawn and removed with the draw and erase tools; a reset's own width, and the lane in the drum editor;
  the key editor's Articulations field setting and clearing the selected notes' attributes; the list editor's
  Articulation events without a channel, a direction renamed and notes' attributes typed and cleared. Its eleventh to
  fourteenth (#1498): a slot color's light or dark class, a cleared color and a width a kept element no longer has,
  and the articulations' names beside their rows; no second direction where the row's starts, and Select All Events
  on Lane selecting the directions; only the parts whose track has the lane's map shown and edited; the lane in the
  in-place editor.
  `tests/scripting_tests.cpp` checks empty Lua tables as lists and `gnubase.off`.
- `tests/articulationmaps_tests.cpp`: maps added, copied, changed and removed with their errors and undo, a track's map;
  directions and attributes; playback through the slots' output events and MIDI modifiers, the closest slot and resets;
  export and import round trip with errors; maps, the tracks' maps and articulations saved and loaded; remote triggers
  in recordings, key switches with and without latch and program changes; renamed and removed articulations in the
  parts, shared copies on other tracks and other track versions; a key switch at the project's start before a lower
  note; no output events for notes that don't play (their probability, a muted drum sound); a slot's off and on
  events where it differs from the last one the track sent, across parts; Merge MIDI in Loop with the directions in
  effect, the directions in every new part where it makes several, keeping the key switches of three parts, the middle
  one with a part of its own, and over several parts onto a new
  track with the map, with resets only where a direction of their group is in effect, on one track and in the lane two
  tracks share (a reset where the second track's part starts while the first's direction is in effect, none once that
  direction was reset); the slots a map and a part as `project.get` lists them choose (the sound slots lane's) and
  `articulationMap.soundSlots`, names joined and split at commas, and the `articulations` lane type, which
  `controller.line` refuses; track presets and track import with the map; remote
  triggers played into a track (`midi.inject`, `midi.capture`), the default slot after another map or other slots but
  not after a slot's new name or color, and a held key's note-off once it is a trigger. `tests/midipatches_tests.cpp`
  plays them into a MIDI track set to a channel: key switches with latch, the selected slot going out again when the
  track's channel changes, the default slot coming back on the track's channel where a trigger key is released without
  latch, and program change triggers. `tests/midi_tests.cpp` and `tests/sequencing_tests.cpp` check the chased key
  switches; `tests/rtcheck_tests.cpp` plays remote triggers through the RT checker.
