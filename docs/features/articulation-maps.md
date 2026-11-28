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
| `addOn` | An add-on slot (#1113): its output events go besides the slot of the other groups; not the default slot; its MIDI modifiers do nothing | pp. 1272, 1281 |
| `attack` | Attack Compensation: ms 0-1000 the notes start earlier | p. 1271 |
| `separation` | Ms 0-1000 the output events go before the notes; 0: the key switches one sample before, the others with the note | p. 1271 |
| `minPitch`, `maxPitch` | The range the notes' pitch is kept in, after `transpose` | p. 1272 |
| `minVelocity`, `maxVelocity` | The range (1-127) the notes' velocity is kept in, after `velocity` | p. 1272 |

| Command | What it does | Reference |
|---|---|---|
| `articulationMap.add` | Add Map (New Map with Group 1) or Duplicate Map (`copy`), with the settings given; Import Key Switches with `keySwitches`, a track (below) | pp. 1267, 1280, 1284 |
| `articulationMap.set` | Changes a map; `articulations` and `slots` replace the whole lists | pp. 1268-1279 |
| `articulationMap.remove` | Removes a map; the tracks that used it have none | p. 1267 |
| `articulationMap.export` | Saves a map as an articulation map file (`.articulationmap`) | p. 1292 |
| `articulationMap.import` | Loads an articulation map file into the project as a new map | p. 1283 |
| `articulationMap.soundSlots` | The sound slot each note of a part plays (`midi::soundSlots`), the sound slots lane's, for MCP and Lua clients | p. 1287 |
| `articulationMap.remoteTriggers` | The remote triggers at once (#1113): Transpose All (`transpose` by `semitones`, with `allMaps` of every map, one undo step), Reassign All (`reassign` from `start` on the `keys` `all`, `white` or `black`, `skipFirst`: the first slot keeps its trigger, which no other slot gets) and Remove All (`remove`); `semitones` is -127 to 127 | pp. 1273-1275 |
| `track.set articulationMap` | The map of a MIDI or instrument track, 0 for none | p. 1286 |

They emit `articulationMap.added`, `articulationMap.changed` and `articulationMap.removed` with the map (its id for
removed).

- **Import Key Switches** (`articulationMap.add` `keySwitches`, p. 1284). The instrument of a MIDI or instrument track
  (a MIDI track's: the instrument track's it plays) reports the key switches of its preset through the plugin
  wrapper's `KeySwitches` extension ([plugin-api.md](../plugin-api.md#gnubase-extensions)): a name and a key each.
  Each becomes a direction of the first group and a sound slot with its name and that articulation, the key as its
  remote trigger and a note on the key at velocity 100 as its output mapping; a name that comes twice is one
  direction. An instrument that reports none, or a track without one, is refused. Of the built-in instruments, only
  the test synth has key switches, Sustain (C-2) and Staccato (C#-2), for the tests; they play like other notes.

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
- **Muted articulations** (#1684, p. 1289). A direction with `muted` (`controller.add`, `controller.set`; only
  articulation events have it) and the attributes in a note's `mutedArticulations` (`note.add`, `note.set`: only
  attributes the note has, each once, and an attribute that goes is no longer muted; a loaded project's too, #1762)
  are muted: they select no sound slot, so they play no output events and change no note, as if they weren't there. A
  muted direction ends no direction before it, and Merge MIDI in Loop doesn't take it as in effect, though it copies it,
  muted. Renaming an attribute renames its mute too. `project.get` lists them only where they are muted, and the project
  saves them.
- **Playback** (`src/engine/document/rendersnapshot.cpp`). Each note of a track with a map plays a sound slot
  (`midi::soundSlots`): of each group, its attribute or else the direction in effect at its start; the slot with
  exactly those articulations, else the slot that has those of the more important groups, then the fewest others,
  then the first: with no match the default slot (pp. 1262, 1278). Where a note's slot differs from the last one its
  track sent, across the track's parts in the order the notes play in, earlier by their slots' Attack Compensation
  (and at the track's first note), the off events
  of that slot and the new slot's on events play (#1491): controller and program changes where the note starts, key
  switches for one sample just before it, and at one sample before the notes that start there, whatever their pitch (at
  the project's start a key switch can't come a sample before), in the order the track sends them, so that the
  instrument ends in the slot the track sent last where notes of several slots start together (#1521). Notes that don't
  play (their probability, a muted drum sound) select no slot and send no output events. Where playback starts or jumps,
  the controller and program changes are chased like any others, and with note chasing (`midi.chaseNotes`) the last key
  switch of each pitch before the position plays again there for a sample, in their order, before the notes. The
  controller and program changes are the track's, not the part's of the note that sent them (`g_soundSlotPart`): with
  the chase limited to the parts under the position (`midi.chaseBeyondParts` off) they are chased all the same, also
  between parts, so a part that continues the slot of the part before it has that slot after a jump (#1521). Key
  switches are notes: without note chasing, and for a MIDI track heard again after it was soloed out or disabled, which
  chases no notes, a part that continues the slot of the part before it doesn't send its key switch again, as no note of
  it sends a slot the track has sent. The slot's channel, transpose, velocity and length apply to the note before the
  track's MIDI modifiers. Merge MIDI in Loop (`midi.merge`) starts the new part with the directions of each group in
  effect where the range starts, and where a part starts later in the range it adds a reset of each group of that part's
  track's map in which a direction is in effect there in the new part, as playback starts each part with the default
  slot, so that its notes play the same slots. Where merge makes more than one new part (#1240), each of them gets these
  directions and resets; as playback sends the output events across a track's parts, the new parts send the key switches
  the source parts did, also where they lie over each other across the range. A new track for the merged part gets the
  map of the first merged track that has one. The directions of several tracks with maps share the new part's one lane,
  so they can't all keep their slots: a direction of one track's part also selects the slots of the other tracks' notes
  after it, and the reset where another track's part starts ends it, even where its own part still plays.
- **Add-on slots and the other MIDI modifiers** (#1113, pp. 1271-1272, 1281). A note plays the best slot that isn't an
  add-on, as above, and besides it each add-on slot whose articulations are all in effect for it
  (`midi::addOnSlots`): where the add-ons differ from those of the note before, the off events of those that end and
  the on events of those that start go after the slot's own, on the channel of the note, the off events on the channel
  the on events went to; where the notes go to another channel (a slot's `channel`), an add-on that stays goes off
  there and on again on the new channel (#1667). On a track without a MIDI channel, where the notes keep their own
  channels, the notes of each of their own channels are a line of their own, with the slot and add-ons last sent for
  them (#1744): where notes on several channels sound together, a chord from MPE input or two hands on two channels,
  each channel gets the on events of its notes' slot and add-ons, and none the off events because a note on another
  channel plays another slot. Only a slot's own channel moves a line to another channel, with the off events of its slot
  and add-ons on the one it leaves (#1718). Each channel the notes play on keeps what went out there last: where lines
  meet on one through slots with a channel of their own, a note whose slot or add-ons differ from those sent there for
  another line sends its own again, after the off events of the others. An add-on slot's own MIDI modifiers do nothing:
  the notes play with those of their slot, and the setup shows none for it. The sound slots lane and
  `articulationMap.soundSlots` show the slot, not the add-ons. Attack Compensation starts a note earlier by its slot's
  `attack`, with its end, its expression and its slot's output events; its slot's `separation` sends the output events,
  key switches, controller and program changes, that much before that (with 0 the key switches one sample before, the
  others with the note), but after the start of the note before, so that note keeps its own slot (#1667). The pitch and
  velocity ranges keep the note's pitch and velocity in them after the slot's transpose and velocity, also for the notes
  played into the track. A remote trigger of an add-on slot in a recording adds the directions of its own groups only,
  and their resets where its key is released without latch, in the groups no later trigger took; its attributes go to
  the notes that start while it is selected, until a trigger of another slot with an articulation of its groups, an
  add-on slot or not, which takes its place (#1667). Slots that aren't add-ons leave the groups alone in which an add-on
  slot is selected and they have no articulation of their own; in the others they write their own direction (#1669).
  Without latch its key isn't one that comes before a held key's release. Each group's triggers are listed once, by
  position, so that the work grows with the triggers, not their square (#1669). Played into the track, it sends its on
  events on the channel the notes play on (the selected slot's), and without latch its off events where the key is
  released, on the channel its on events went to, and leaves the selected slot as it is; its trigger, and that of a slot
  that isn't an add-on with an articulation of its groups, sends the off events of an add-on slot of its groups that is
  on, also with latch (#1667). An add-on slot that is on follows the notes to another channel: where another slot with a
  channel of its own is selected, the default slot comes back on another, the MIDI track is set to another channel (back
  to Any: the selected slot's channel, its trigger's, or with none selected the default slot's own or the channel of the
  add-on's trigger), or its key is pressed again on another channel, its off events go to the channel its on events went
  to and its on events to the new one (#1718). Unlike playback, the input keeps one selected slot and its add-ons on one
  channel: on a track without a MIDI channel, notes played on another channel than the selected slot's get no output
  events on theirs (#1744). Without latch, the default slot coming back where a key is released ends no add-on slot: one
  whose key is still held keeps its groups, in the track and in a recording, where the default slot's directions leave
  those groups alone, also where it has an articulation of them (#1718).
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
  inspector's Setup... button; `resources/ui/articulation-maps.rml`, pp. 1264-1283). The project's maps on the left: Add
  Map, Duplicate Map, Remove Map, Import Map and Export Map (the OS's file dialogs through
  `gnubase.ui.importArticulationMap` and `gnubase.ui.exportArticulationMap`; Export Map offers the map's name as the
  file name, with the characters file names can't have replaced, `engine::safeFileName`), and Assign to Track for the
  selected MIDI or instrument track. With that track, Import Key Switches beside Add Map, checked, makes Add Map import
  the track's key switches (p. 1285, the reference's Add Map dialog has it). On the right the selected map: its name,
  Remote Trigger (Key Switch or Program Change) and latch; its groups; its articulations with their type and group; its
  sound slots with their articulations (names separated by commas) and remote trigger; the output mapping (status, data
  1, data 2, on or off events) and MIDI modifiers (channel 1-16 or empty for the notes' own, length and velocity in
  percent, transpose) of the slot picked with its radio button, which an add-on slot doesn't have (`.add-on-modifiers`
  says so instead). Each change goes to `articulationMap.set` at once, text fields with Enter or when they lose the
  focus; a change the engine refuses shows its message and the field as it was, and so does an import or export that
  fails. While it is open it follows `articulationMap.added`, `changed` and `removed` and `project.reset`, so it shows
  the maps after an import, an undo or a change through MCP or Lua, and selects a map that comes; it unsubscribes its
  handlers when it closes (`gnubase.off`), as the profile manager and the workspaces organizer do. Like them it opens
  once (`AppUi::openOnce`): opening it again, from another window's inspector or the UI command through MCP, brings the
  open one to the front, so that one set of handlers follows the engine. Opened again for another track, it is that
  track's setup: it selects the track's map, if it has one, and Assign to Track assigns to it (the setup's `track`
  attribute and its `trackchange` event). Opened again without a MIDI or instrument track selected, it has no track, as
  when it opens without one, and hides Assign to Track. Opened again for the same track, it keeps the map it shows. A
  new or loaded project (`project.reset`) drops its track too, since object ids are per project: Assign to Track would
  otherwise assign to whatever track has that id in the new project. The maps list has a search field (by name) and Show
  only Maps used in this Project, and marks the maps the project's tracks use with P (p. 1267), which follow
  `track.added`, `track.changed` and `track.removed` and Assign to Track (#1667). Where they leave the selected map out,
  the first map listed is selected, or none with none listed. The sound slot actions (p. 1273, #1113) act on the slot
  picked with its radio button: Add Sound Slot and Add Sound Slot (Add-on) (marked Add-on in its row) add a slot with
  the articulations checked in the articulation combinations, named after them; Duplicate (without the trigger), Move
  Up, Move Down, As Default, Cut, Copy and Paste (after the picked slot) send the slots to `articulationMap.set`, and
  pick the slot they move or add once the engine took the change (an add-on slot isn't the default: it stays picked);
  Copy and Cut keep a copy of the slot, down to its output mapping, which later changes of the slot leave as it was
  (#1667); Remote Trigger's Transpose Up and Down change the picked slot's trigger by a semitone, and Transpose All (by
  semitones or octaves, optionally of all maps in the project), Reassign All (from a start key, all, white or black
  keys, Skip First Slot) and Remove All run `articulationMap.remoteTriggers`. The articulation combinations (pp.
  1275-1276) list a check box for each articulation of the map by group, the picked slot's checked, written again when a
  group or an articulation changes or the picked slot's articulations are typed (#1667); Apply to Slot gives the picked
  slot the checked ones, and the search shows only the articulations whose names have one of its terms, separated by
  `OR` or `|`. The MIDI modifiers have Attack Compensation, Separation and the pitch and velocity ranges too.
- **Inspector** ([inspector.md](inspector.md), p. 1263): MIDI and instrument tracks' Articulation Map section chooses
  the track's map, or No Map (`track.set articulationMap`), and opens the setup.
- **Articulations lane** (`ui::ControllerLanes`, pp. 1287-1289): the lane type `articulations` of the key, drum and
  in-place editors' controller lanes (`editor.set lanes`, the Create Controller Lane menu's Articulations). The map is
  the active part's track's, else the first shown part's track's; the lane shows and edits only the parts whose track
  has that map (#1498). Its first row is the sound slots lane: each note's slot (`.sound-slot[part][note]`, its name as
  text and title) from its start to its end, as `midi::soundSlots` chooses it from the map and the part as `project.get`
  lists them (`articulationMap.soundSlots` gives MCP and Lua clients the same), in the slot's color where it has one
  (the only color code sets there, user data like the track colors), with the state class `.light` or `.dark` for a
  light or dark slot color, on which the themes write the slot's name in a dark or a light color. The lane keeps its
  elements while their RML is the same, so code also takes away a width an element no longer has; a slot that loses its
  color loses its class `.light` or `.dark` with it, so its elements are new and have none. A row per articulation of
  the map follows (`.articulation-row[articulation-row]`, its name beside it in the lane's header): directions
  (`.articulation-item.direction[part][controller]`) last until the next direction of their group or the part's end, a
  reset (`.reset`) is as wide as the style makes it (p. 1290); attributes (`.articulation-item.attribute[part][note]`)
  are on their notes, as long as them. Directions and attributes whose names aren't the map's don't show (p. 1291).
  Muted ones have the state class `.muted`, which the themes show in gray (#1684, p. 1289), also selected or outside
  the part editing mode (#1799); a muted direction ends no direction before it, which lasts on through it as it plays,
  while it keeps its own span (#1762, #1799). The key editor's mute tool mutes the direction or attribute clicked
  (`controller.set` `muted`, `note.set` `mutedArticulations`), or unmutes it, one undo step each; the drum editor's
  toolbar has it too. With the draw tool a click in a direction's row adds it on the quantize grid (Ctrl: off it) to the
  part there (`controller.add`), but not where the row's direction already starts, in an attribute's row to the notes
  that sound there and started last, in place of their attribute of the same group (`note.set`); a click on a direction
  or attribute removes it, as the erase tool's does. The lane menu's Select All Events on Lane selects the lane's
  directions.
- **Groups and the sound slots lane** (#1580, pp. 1287-1288; the view's `collapsedGroups` and `soundSlotsLane`,
  `editor.set`). A collapsed group is one row (`.articulation-row.collapsed[articulation-group]`) where its first
  articulation's row would be, the group's name beside it (`.articulation-name.collapsed`), with the directions and
  attributes of all its articulations, each with its name. A click on an articulation's name collapses its group, on a
  collapsed group's name expands it. Collapse All Groups, Expand All Groups and Show/Hide Sound Slots Lane are buttons
  on the lane, under the articulations' names (`.articulation-control[lane-action]`, pp. 1287-1288, #1658). Hidden, the
  sound slots lane's row stays empty beside the lane's name, so the articulations' names stay beside their rows. The
  draw tool adds nothing in a collapsed row, which has no articulation of its own; it and the erase tool remove the
  directions and attributes there.
- **Selecting and deleting** (#1580, pp. 1289-1290). With the object selection tool a click selects the direction
  (`controllerSelection`) or the attribute pressed with its note (`selection`, the entry's `articulations` naming the
  attributes selected of it, #1688), and nothing else; Shift or Ctrl (Cmd) adds or removes it, a click beside them
  deselects. A note stays selected while one of its attributes is; a note selected elsewhere, an entry without
  `articulations`, has all its attributes selected (#1658), and Shift on one of them removes it from them, so that the
  note stays selected with the others (#1688). As the attribute selection is the editor view's, `editor.get` shows it,
  Lua and MCP select single attributes with `editor.set` (only the note's own, not none, #1762), and a new selection of
  the note elsewhere selects all its attributes again; the notes that stay selected as the note display's selection
  changes, as with Shift and a click on another note, keep theirs (#1762). An attribute that goes leaves the entry, and
  a note whose attributes selected all went, as after Delete, is no longer selected. A press on the lane gives it the
  keyboard focus (on the lanes' root, as the lane's elements can go with the next render), which its state class
  `.focused` shows, a frame the themes draw over the whole lane (`.controller-lane-frame`, taking no room, so that the
  area lines up with the other lanes'), as around the docking zone with the focus (p. 100, #1658, #1688), until the root
  loses the focus: Delete or Backspace then removes its selected directions and attributes (`controller.remove`,
  `note.set`, one undo step), not the notes, as the reference does (p. 1289). With the object selection tool an arrow in
  a direction's upper left corner (`.articulation-menu-arrow`, shown while the mouse is over the direction or it is
  selected; beside a reset, which is narrower than it, so that a press on the reset selects it, #1688) opens its event
  pop-up menu (`#controller-articulation-menu`, p. 1290, #1658), as a right click on it does; Reset, checked on a reset,
  makes it a reset or a direction again (`controller.set` `reset`). A direction of a part that isn't editable has
  neither.
- **Articulations field** (p. 1290): the key and drum editors' info line shows the first selected note's attributes,
  names separated by commas; typed, they replace every selected note's, and typed empty, they go. A warning sign
  (`svg.icon.warning`, which the themes color for every warning sign, the field's state class `.unknown`) shows where
  one of them isn't an attribute of the note's track's map (#1580, p. 1291), as `ui::unknownArticulations` finds. The
  button beside the field (`button.articulation-menu`) opens its pop-up menu of the map's attributes
  (`#key-articulations-menu`, the first selected note's checked, p. 1290): one chosen goes to every selected note of a
  track with the first selected note's map in place of its group's attribute, or, if the first selected note has it,
  goes from them (`ui::toggleAttribute`); the notes of tracks with another map or none keep theirs (#1658). The info
  line and the list editor join and split the names with `ui::joinNames` and `ui::splitNames`
  (`ui/articulationnames.h`).
- **List editor** (p. 1291, [list-editor.md](list-editor.md)): directions are events of the type Articulation with the
  direction's name in the Attributes column, without a channel; a note's Attributes column has its attributes. Both can
  be typed there. A warning sign follows the column where a direction or one of a note's attributes isn't the track's
  map's (#1580, p. 1291). A click on the column opens its pop-up menu (`#list-articulations-menu`) under it, a right
  click at the mouse (#1658); a click on another row's cell opens it for that row, a second click on the same cell
  closes it, also after the rows rendered anew meanwhile (#1799), and a cell whose text is edited (`.editing`) doesn't
  open it (#1688): for a note the map's attributes, one chosen toggling on the note in place of its group's, for a
  direction the map's directions, one chosen renaming it. Its checked items stay checked while the list editor
  renders.
- **Empty lists from Lua.** Lua has no empty list apart from an empty table, which becomes a JSON object: where a
  command's parameters take a list, the Lua functions make it an empty list (`src/scripting/runtime.cpp`), so that the
  setup can take the last articulation of a slot away.

## Not yet

The mute tool on notes and on controller events other than directions, which have no mute yet (#679); the
project window's info line for the in-place editor's notes; playback techniques, aliases and fallbacks,
symbols, categories and Add Custom Articulation, articulation combinations beyond one per group; a slot's length for
the notes played into a track, which aren't known to end yet; the list editor's Sound Slot event colors (the key, drum
and in-place editors have them, [colors.md](colors.md)); the slot actions Rename and Reset Color (the slots'
names and colors are typed or set through `articulationMap.set`), the keyboard preview and assigning triggers from a
MIDI keyboard; the sound slot actions as a menu and Transpose All and Reassign All as dialogs of their own (they are
rows of the setup); changing several maps' remote trigger mode and latch at once; Import Key Switches from the
track's running instrument and from plugins, which waits for instrument tracks that host plugins (#73): the
`KeySwitches` extension already reaches plugins in host processes and VST3's `IKeyswitchController` (#1988), but the
import asks a new instance of the track's built-in instrument; the Add Map dialog with the map's name; importing the
reference's own files and other applications' maps.

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
  in-place editor. Its test case for #1113 checks the maps list's P mark, Show only Maps used in this Project and
  search, an add-on slot added from a checked combination, the combinations' search with OR, Attack Compensation and
  the maximum velocity typed, Move Up and Down, Copy, Paste and Cut, Transpose Up, Reassign All, Transpose All and
  Remove All. The one after it (#1667): the P marks after `track.set` and Assign to Track, a search that leaves the
  selected map out, the combinations after an articulation's rename and a slot's articulations typed, Apply to Slot,
  As Default refused for an add-on slot, Duplicate, As Default, the clipboard after an output mapping is added,
  Transpose Down, Reassign All on white keys with Skip First Slot and Transpose All by an octave of all maps. The ones
  after it (#1580): groups collapsed and expanded with the lane's buttons, which the other lanes don't have, and their
  names, an attribute erased in a collapsed row, the sound slots lane hidden and shown; directions and attributes' notes
  selected with the object selection tool and Shift, Delete and Backspace removing the selected directions and
  attributes but not the notes, and Reset from a direction's pop-up menu, from its arrow and a right click, checked on a
  reset, and no arrows for the other tools; the warning sign in the info line and the list editor; the info line's and
  the list editor's pop-up menus, the list editor's opening on a click and keeping its checks while it renders. Two more
  (#1658): an attribute deleted while the note's other one stays, Shift adding and removing an attribute of a selected
  note, all attributes of a note selected elsewhere, and the lane's `.focused` while it has the keyboard focus;
  the info line's pop-up menu changing only the selected notes of tracks with the first one's map, also of a track
  with another map. The last ones (#1684, #1688): the mute tool muting and unmuting a direction and an attribute,
  `.muted`, and the slots the notes play then; the attribute selection in `editor.get` and `editor.set`, a new
  selection of the note selecting all its attributes, Shift removing one of all, Delete with the lane's frame; a press
  on a reset selecting it and its arrow beside it; the list editor's menu opening for another row's cell and not while
  a cell is edited, and closing on a second click on its cell after the rows rendered anew (#1799); a direction lasting
  on the lane through a muted one, which keeps its own span, and the note display's selection changes keeping the lane's
  attributes selected of the notes that stay (#1762). One more (#1114): Add Map with Import Key Switches checked
  imports the test synth's key switches, unchecked it adds an empty map; without a track the option is hidden.
  `tests/scripting_tests.cpp` checks empty Lua tables as lists and `gnubase.off`.
- `tests/articulationmaps_tests.cpp`: maps added, copied, changed and removed with their errors and undo, a track's map;
  directions and attributes; playback through the slots' output events and MIDI modifiers, the closest slot and resets;
  export and import round trip with errors; maps, the tracks' maps and articulations saved and loaded; remote triggers
  in recordings, key switches with and without latch and program changes; renamed and removed articulations in the
  parts, shared copies on other tracks and other track versions; a key switch at the project's start before a lower
  note; key switches at one sample in the order the track sends them, from two parts over each other (#1521); the sends
  before the modifiers getting the key switches; no output events for notes that don't play (their probability, a muted
  drum sound); a slot's off and on events where it differs from the last one the track sent, across parts; Merge MIDI in
  Loop with the directions in effect, the directions in every new part where it makes several, keeping the key switches
  of three parts, the middle one with a part of its own, and over several parts onto a new track with the map, with
  resets only where a direction of their group is in effect, on one track and in the lane two tracks share (a reset
  where the second track's part starts while the first's direction is in effect, none once that direction was reset);
  muted directions and attributes selecting no slot, refused on other events and on attributes the note doesn't
  have, and going with their attribute; saved and loaded, renamed with the map and not in effect in Merge MIDI in Loop
  (#1684); the selection's `articulations` (#1688), `editor.set` refusing attributes the note doesn't have and an empty
  list, and a note leaving the selection once its selected attributes are gone (#1762); a loaded project's mutes, only
  the note's own, each once, and Merge MIDI in Loop copying a muted direction inside the range, muted (#1762, #1799);
  the slots a map and a part as `project.get` lists them choose (the sound slots lane's) and
  `articulationMap.soundSlots`, and the `articulations` lane type, which
  `controller.line` refuses; track presets and track import with the map; remote triggers played into a track
  (`midi.inject`, `midi.capture`), the default slot after another map or other slots but not after a slot's new name or
  color, and a held key's note-off once it is a trigger. `tests/midipatches_tests.cpp` plays them into a MIDI track set
  to a channel: key switches with latch, the selected slot going out again when the track's channel changes, the default
  slot coming back on the track's channel where a trigger key is released without latch, program change triggers, and
  an add-on slot following the track to another channel, back to Any and to a slot's own channel (#1718).
  Its last test cases (#1113): add-on slots' output events besides the slot of the other groups, and the
  default slot refused as one; Attack Compensation, Separation and the pitch and velocity ranges, with a range refused;
  slots changing in the order the notes play with Attack Compensation;
  `articulationMap.remoteTriggers` transposing one map and all with one undo step, refusing triggers out of range and
  `semitones` past -127 to 127, reassigning on white and black keys with Skip First Slot, which doesn't give the first
  slot's trigger again, and past 127, program changes, and removing them; Separation for the controller and program
  changes and after the start of the note before; add-on slots on the notes' channel and following them to another;
  an add-on slot's trigger played into a track (`midi.inject`); remote triggers in a recording also check one recorded
  with latch before another slot's trigger and without latch while another slot's key is held, with its attribute on
  the notes, a slot with an articulation of their groups taking their place, a second add-on slot of their group
  replacing them, add-on slots triggered by program changes, and the default slot coming back without latch leaving
  the groups of an add-on slot whose key is still held alone; add-on slots played into a track with latch, ended by
  another of their group and by a slot with an articulation of it, on the selected slot's channel, pressed again on
  another channel, following the notes to the default slot's channel where it comes back, and pressed again after a
  setup edit put another one on into its group (#1744); a slot following the notes to their own channels in playback,
  as its add-ons do (#1718), and a chord across two channels keeping both channels' slots, and notes of two own channels
  whose slots share a channel of their own, alternating slots, sending their slot and add-on there again each time
  (#1744). Import Key Switches (#1114): the directions and slots of the test synth's key switches, one undo step, a MIDI
  track's instrument, the settings given changing the imported map, and the refusals (an instrument without key
  switches, no track, a MIDI track that plays none). `tests/vst3_adapter_tests.cpp` checks the key switches that the
  `KeySwitches` extension gives for the adapter test plugin's `IKeyswitchController`, which follow the program it
  loaded and leave out a key outside 0-127, in the test process and through a plugin host process after a program
  change, and no extension for a plugin without one (#1988).
  `tests/midi_tests.cpp` and `tests/sequencing_tests.cpp` check the chased key switches, and
  `tests/sequencing_tests.cpp` a jump with the chase limited to parts into a part that continues the slot of the part
  before it (#1521), and a MIDI track heard again there after it was disabled (#1600); `tests/midi_tests.cpp` the
  sound slots' chase index with the chase limited to parts: between parts and after them, a part's later event of the
  slot's kind and the slot's at one position, and a slot's event at the position, playing and stopped (#1600);
  `tests/rtcheck_tests.cpp` plays remote triggers through the RT checker.
- `tests/articulationnames_tests.cpp` (`[unit]`): names joined and split at commas, a track's map and the names it
  doesn't have, the pop-up menus' items and an attribute toggled in place of its group's (`ui/articulationnames.h`).
