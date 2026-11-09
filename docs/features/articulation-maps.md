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
| `track.set articulationMap` | The map of a MIDI or instrument track, 0 for none | p. 1286 |

They emit `articulationMap.added`, `articulationMap.changed` and `articulationMap.removed` with the map (its id for
removed).

- **Articulations in parts** (pp. 1287-1291). A direction is a controller event of the type `articulation`
  (`controller.add`, `controller.set`) with the direction's name; with `reset` it ends the directions of its group
  instead (p. 1289). Directions are part of the part's controller events, so they move, copy, split, glue and undo
  with them; they play nothing themselves and MIDI files and DAWproject leave them out. Attributes are the notes'
  `articulations` (`note.add`, `note.set`), by name. Names that aren't the map's articulations play nothing (p. 1291).
- **Playback** (`src/engine/document/rendersnapshot.cpp`). Each note of a track with a map plays a sound slot
  (`midi::soundSlots`): of each group, its attribute or else the direction in effect at its start; the slot with
  exactly those articulations, else the slot that has those of the more important groups, then the fewest others,
  then the first: with no match the default slot (pp. 1262, 1278). Where the slot changes from one note to the next in
  a part (and at its first note), the previous slot's off events and the new slot's on events play: controller and
  program changes where the note starts, key switches for one sample just before it. Where playback starts or jumps,
  the controller and program changes are chased like any others, and with note chasing (`midi.chaseNotes`) the last
  key switch of each pitch before the position plays again there for a sample, in their order, before the notes. The
  slot's channel, transpose, velocity and length apply to the note before the track's MIDI modifiers.
- **Remote triggers** (pp. 1263, 1268, 1282). In what a recording brings to a track with a map (`recording.add`), the
  notes whose pitch is a slot's trigger (Key Switch), or the program changes whose program is one (Program Change),
  aren't recorded; they select their slot instead: a direction for each group that has directions, the slot's own or
  a reset, where they come, and the slot's attributes on the notes that start while it is selected. Without latch
  the default slot comes back where the key is released.
- **Articulation map files** are JSON: the map as `articulationMap.set` takes it, with `"format":
  "gnubase.articulationMap"`. Track presets and track import bring a track's map along, or use the project's map
  that is the same.

## In the UI

- **Articulation Map Setup** (MIDI > Articulation Map Setup..., the UI command `midi.articulationMapSetup`, and the
  inspector's Setup... button; `resources/ui/articulation-maps.rml`, pp. 1264-1283). The project's maps on the left:
  Add Map, Duplicate Map, Remove Map, Import Map and Export Map (the OS's file dialogs through
  `gnubase.ui.importArticulationMap` and `gnubase.ui.exportArticulationMap`), and Assign to Track for the selected
  MIDI or instrument track. On the right the selected map: its name, Remote Trigger (Key Switch or Program Change)
  and latch; its groups; its articulations with their type and group; its sound slots with their articulations
  (names separated by commas) and remote trigger; the output mapping (status, data 1, data 2, on or off events) and
  MIDI modifiers (channel 1-16 or empty for the notes' own, length and velocity in percent, transpose) of the slot
  picked with its radio button. Each change goes to `articulationMap.set` at once, text fields with Enter or when they
  lose the focus; a change the engine refuses shows its message and the field as it was.
- **Inspector** ([inspector.md](inspector.md), p. 1263): MIDI and instrument tracks' Articulation Map section chooses
  the track's map, or No Map (`track.set articulationMap`), and opens the setup.
- **Empty lists from Lua.** Lua has no empty list apart from an empty table, which becomes a JSON object: where a
  command's parameters take a list, the Lua functions make it an empty list (`src/scripting/runtime.cpp`), so that the
  setup can take the last articulation of a slot away.

## Not yet

The Articulations controller lanes with their sound slots lane, and the info line's and list editor's
Articulations fields (pp. 1287-1291): directions and attributes are entered through the engine API for now;
add-on slots, the MIDI modifiers Attack Compensation, Separation and the pitch and velocity ranges; playback
techniques, aliases and fallbacks, symbols, articulation combinations beyond one per group; remote triggers on live
MIDI input (they select slots in recordings only); muted articulations; Sound Slot event colors; Transpose All and
Reassign All Remote Triggers; Import Key Switches from instrument presets; importing the reference's own files and
other applications' maps.

## Checks

- `tests/articulationmaps_ui_tests.cpp` drives the app with `ui::Automation`: the inspector's section opens the setup,
  which adds a map, renames it, adds an articulation and a slot with it, a remote trigger, an output mapping and a
  transpose, shows a refused change's message, assigns the map to the track; the inspector takes it off again, and the
  MIDI menu's setup removes it. `tests/scripting_tests.cpp` checks empty Lua tables as lists.
- `tests/articulationmaps_tests.cpp`: maps added, copied, changed and removed with their errors and undo, a track's
  map; directions and attributes; playback through the slots' output events and MIDI modifiers, the closest slot and
  resets; export and import round trip with errors; maps, the tracks' maps and articulations saved and loaded; remote
  triggers in recordings, key switches with and without latch and program changes.
