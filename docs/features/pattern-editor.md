# Pattern editor

The pattern editor (#181, reference manual pp. 895-937): patterns of steps on step lanes, in drum mode a lane per drum
sound and in melodic mode a lane per pitch, which pattern events play over and over. It is one of the MIDI editors'
contents ([midi-editors.md](midi-editors.md)).

## In the engine

**Patterns** (`src/engine/midi/patterns.h`) are part of the project (`Project::patterns`): `project.get` lists them as
`patterns`, they are saved in the project's GNUbase entry, and changing them is an undo step. Each belongs to a MIDI or
instrument track, whose pattern management lists it.

| Pattern field | What it is | Reference |
|---|---|---|
| `name` | Pattern 1, Pattern 2 and so on | p. 899 |
| `mode` | `drum`, `mono` or `poly`: drum mode, or melodic mode with one step at a time or chords | pp. 896, 906, 913 |
| `length` | Number of Steps, 1-128 (16) | p. 898 |
| `resolution` | Step Resolution, a note value up to 1/64 with triplets and dotted (1/16) | p. 898 |
| `direction` | Pattern Play Direction: `forward`, `backward` or `alternating` | p. 898 |
| `swing` | Pattern Swing, -1 to 1: every second step moves by that many half steps | p. 899 |
| `lanes` | The step lanes, one per pitch, below | pp. 904, 921 |

| Lane field | What it is | Reference |
|---|---|---|
| `pitch`, `name` | Select Pitch, and the lane's name (drum mode) | pp. 904, 921 |
| `mute`, `solo` | The lane doesn't play; only soloed lanes play | p. 921 |
| `length`, `resolution`, `direction` | The lane's own Number of Steps, Step Resolution and Play Direction; 0, `null` and `forward` follow the pattern's. In mono mode they are the pattern's | pp. 904, 922 |
| `swing`, `offset` | Swing added to the pattern's, and the whole lane moved by -1 to 1 steps | p. 906 |
| `steps` | `{index, length, velocity, repeats, offset, probability, velocityVariance, gate}`: where it starts, the steps tied to it, and the parameter lane's values (Velocity 1-127, Repeats 1-10, Offset -0.5 to 0.5 steps, Probability, Velocity Variance, Gate) | pp. 927, 929 |

| Command | What it does | Reference |
|---|---|---|
| `pattern.add` | New Pattern of a track (drum mode with a kick, a snare and closed and open hi-hats, named after the track's drum map; on a drum track a lane per pad that plays, named after the pad, [drum-track.md](drum-track.md)), or Duplicate Pattern (`copy`); with `part` the part plays it | p. 899 |
| `pattern.set` | Changes a pattern; `lanes` replaces the whole list. A shorter `length` without `lanes` drops the steps that no longer fit on the lanes that follow it and ends their ties at the end | pp. 897-929 |
| `pattern.remove` | Removes a pattern and its pattern events | p. 899 |
| `pattern.convert` | Convert Pattern Event to MIDI Part | p. 937 |
| `pattern.export` | Save Preset: the track's patterns as a pattern preset file (`.patternpreset`, JSON) | p. 932 |
| `pattern.import` | Load Preset: a preset's patterns become new patterns of the track | p. 933 |
| `midiPart.add pattern`, `midiPart.set pattern` | Makes a MIDI part a pattern event of that pattern, 0 an ordinary part | pp. 934-935 |

They emit `pattern.added`, `pattern.changed` and `pattern.removed` with the pattern (its id for removed).

- **Mode switches** (p. 913): between drum and melodic mode the pattern is cleared; from poly to mono mode the lowest
  step at each position stays and the lanes' own settings go; mono to poly changes nothing. In mono mode a step can't
  overlap another, ties included.
- **Pattern events** are MIDI parts with a `pattern`: they play it from their start over and over instead of their
  own notes, each lane looping on its own length and resolution. Every change of the pattern is in its events at once.
  They move, copy, split, resize and undo as parts do (#1040): their `patternOffset` (`project.get`) is how many ticks
  of the pattern went before their start, so the pieces of a split, and a start sized later or earlier, continue the
  pattern instead of starting it again; another pattern, or none, starts from the part's start again (#1091). Glued to the pieces that continue it, a pattern event stays one; glued to
  anything else, the pattern events' steps become notes of the glued ordinary part, as they played.
- **Other tracks** (pp. 936-937): a pattern event copied or moved to another track, or on a duplicated track, plays a
  copy of its pattern on its own track, whose pattern management lists it (`midi::adoptPatterns` after every edit). A
  track without pattern events of its own gets copies of all the patterns of the event's track, one with pattern
  events only the event's pattern; a copy the track has already, made from that pattern and unchanged since, plays it
  again instead of another one (#1091). An equal pattern that isn't its copy doesn't count (#1202): equal patterns of
  the event's track each get a copy, and a track's own pattern doesn't play events copied there. Which pattern a copy
  was made from isn't saved, so in a loaded project the next copy is a new one. `midiPart.add` and `midiPart.set`
  return the part with the copy. Duplicate Track copies all of the track's patterns, those no event plays too, and the
  pattern events of all its versions play the copies (#1202). Removing a track removes its patterns.
- **MIDI functions** read what a pattern event plays (`midi::playedNotes`): Merge, Dissolve Part and Bounce make notes
  of its steps. The functions that change notes (`midi.transpose` and the others) leave pattern events, whose steps
  the pattern editor edits, as they are; the controller functions (`midi.deleteControllers`,
  `midi.deleteContinuousControllers`, `midi.thinOut`) change their controllers, which play (#1091).
- **Playback** (`midi::patternNotes`, `src/engine/document/rendersnapshot.cpp`): each step plays a note of its lane's
  pitch from its position, as long as it and its ties times the gate, its repeats as that many sub-steps; backward a
  step plays where its last tied step is forward. The sub-steps of a tie that wraps past the lane's end play at the
  start of the next cycle, in a piece after a split too. Swing moves every second step, the step's and the lane's offsets
  move it by parts of a step: a step that a negative offset moves before the event's start plays at its start when
  the pattern starts with the event, and a step of the repeat after the event's end that it moves before the end
  plays. Probability and velocity variance are the notes' (#128), with their own chance in every
  repeat of the pattern. The notes then play as a part's do: through the track's drum map, articulation map and MIDI
  modifiers.

## In the UI

The pattern editor is `ui::PatternEditor` (`src/ui/patterneditor.h`): `ui::MidiEditor` makes it while the view's
content is `pattern`, which a closed editor opens with when its first part is a pattern event. It shows the active
part's pattern.

```
.pattern-editor
  .toolbar.pattern-editor-toolbar
    #pattern-select                 Pattern Management: the pattern's name, menu #pattern-menu
    button.pattern-mode[pattern-mode=drum|mono|poly]     .checked for the pattern's
    button.pattern-tool[pattern-tool=select|draw]        Step Selection and Draw, .checked
    #pattern-length, #pattern-resolution, #pattern-direction   menus #pattern-length-menu, #pattern-resolution-menu,
                                    #pattern-direction-menu
    #pattern-double-length, #pattern-double-resolution, #pattern-tie, #pattern-split
    #pattern-add-lane, #pattern-remove-lane              drum mode
  .pattern-editor-body
    .pattern-lane[pitch]            a step lane; .selected for the parameter lane's
      .pattern-lane-controls        button.pattern-lane-mute, button.pattern-lane-solo (.checked), .pattern-lane-name
      .pattern-lane-steps           a .pattern-step[index] per step: .on where a step starts, .tied where its ties
                                    are, .selected, .beat every fourth
  .pattern-parameter-lane
    button.pattern-parameter[parameter=velocity|repeats|offset|probability|velocityVariance|gate]   .checked
    .pattern-bars                   a .pattern-bar-cell[index] per step of the selected lane, a .pattern-bar for each
                                    step, as high as its value
.pattern-editor-empty               instead, for a part without a pattern: #pattern-new-drum, #pattern-new-melodic
#pattern-menu                       menuitem[pattern-id] for the track's patterns, [checked] for the part's;
                                    menuitem[pattern-action=new|duplicate|remove|convert]
```

- **Step display.** Drum mode shows the lanes in their order, named after their sound; melodic mode a row per pitch
  from C2 to C4 and the lanes' pitches, and adding a step to a pitch without a lane adds its lane. A lane of its own
  length has that many cells.
- **Draw tool** (p. 923): a click adds a step, or removes the one clicked; a drag to the right adds (or removes) more.
  In mono mode a step replaces the one at its position on another lane (p. 913); on a tie, the tied steps move to the
  new pitch from the position clicked, all of them at the tie's start.
- **Step selection tool** (pp. 924-926): a click selects a step, with Shift more, with Ctrl (Cmd) one on or off; a
  click on an empty cell deselects; a double click adds or removes a step; Delete and Backspace delete the selected
  steps; Alt draws as the draw tool does.
- **Edit functions** (pp. 903, 910): Tie joins adjacent steps, Split cuts tied steps into single ones, of the selected
  steps or else of all; Double Length repeats the steps of the lanes that follow the pattern's length, Double
  Resolution doubles the steps' indexes and lengths of the lanes that follow its resolution, and the lengths of those
  of their own length, so that every lane plays as before: one that can't double keeps the resolution it had as its
  own, and lanes of their own length (Double Length) or resolution (Double Resolution) stay as they are; one of its
  own resolution that follows the pattern's length gets the length it had as its own.
- **Parameter lane** (pp. 928-929): a click or drag in it sets the chosen parameter of the selected lane's steps, as
  high as the mouse is.
- A gesture edits a copy of the pattern and changes only the steps' classes while it goes on, so that its mouseup
  reaches the elements it started on; it ends with one `pattern.set` in a `history.transaction`, one undo step. A
  project change that comes while it goes on (a script's, an MCP client's, an undo) shows when it ends, without dropping
  what it changed: its `pattern.set` sends the lanes it edited and the others as the engine has them then (#1091); if the engine refuses its `pattern.set`, the engine's pattern shows again. A project read before the last
  `pattern.set` was answered (a `project.get` answered before it, or the project the editor has with a newer view)
  doesn't have its lanes yet: the editor shows, and the next gesture sends, the lanes it sent instead of that
  project's (#1202). `ui::MidiEditor` tells it when it read the project (`EngineBinding::handed`).
- **Commands** (Edit menu, `app::AppUi`): `editor.openPattern` (Open Pattern Editor) opens the selected MIDI parts in
  the editor window's pattern editor; `pattern.loadPreset` and `pattern.savePreset` (Load and Save Pattern Preset...)
  load a preset into, and save the patterns of, the editor's active track through the OS's file dialogs.

`resources/styles/patterneditor.rcss` lays the lanes and the parameter lane out; the themes color the steps and bars
(`tools/skin/generate.py`).

## Not yet

Play with Project and patterns without pattern events; overlapping pattern events playing only the topmost (p. 935);
the mode-switching rules and the drum lanes matched from top to bottom for pattern events copied to a track whose
pattern events are in another mode (p. 937); the pattern events' menu in the event display, drawing them with the project window's draw tool and Drag Pattern to
Project; the Event Type of the Add Track dialog; the Inspector's generators (Euclidean, Randomization, Add Every Nth
Step, Shapes, Variation, Density, Voices) and Nudge; the scale, pitch range and chord lane sections of melodic mode;
step input and Acoustic Feedback; Legato; ties drawn by dragging a step's end and cut with scissors; moving and copying
steps by dragging, cut, copy, paste and duplicate; the step lane context menu (Copy Steps to Lane, Rename, Sort Lanes
by Pitch); Randomize Parameter and parameters beyond the six (automation, MIDI controllers); Auto-Scroll and the
playback cursor; the colors menu; confirmation dialogs for mode switches that lose steps; the reference's own pattern
bank files; the notes of pattern events in MIDI file export and in the DAWproject content that other applications
read, which have the parts' own notes.

## Checks

- `tests/patterns_tests.cpp`: new, melodic and duplicated patterns, a part becoming a pattern event, steps with their
  parameters and every check, undo, removing a pattern with its events; mode switches; rendered playback of steps,
  ties, repeats, gate, offsets, swing, the play directions, a lane's own length and resolution, mute, solo, longer and
  muted events; split, sized and glued pattern events continuing their pattern, glued to ordinary parts, negative
  offsets at the event's edges; Bounce, Dissolve Part, Merge and the other functions on pattern events; pattern
  events copied and moved to other tracks, copied again, equal patterns that aren't copies, Duplicate Track with
  patterns no event plays and other versions, and removing a track; the pattern offset after a change of pattern; a split's pieces against the whole event for a wrapping tie;
  the controller functions on pattern events; probability per repeat; Convert
  Pattern Event to MIDI Part; presets saved and loaded, with errors; the project file; the pattern editor content.
- `tests/patterneditor_ui_tests.cpp` drives the app with `ui::Automation`: the lanes of a drum pattern, the draw
  tool's click and drag with undo, a drag that the project changes under, on its lane and another, and one that is
  refused, a project read before the last `pattern.set` shown with and without a gesture going on, mono mode over a
  tie, Double Resolution and Double Length with lanes of their own length and resolution, selecting, deleting and double-clicking steps, Tie and Split, mute, the parameter
  lane's velocity and probability, the toolbar's menus and Double Length, Add Lane and melodic mode; New Melodic
  Pattern for a plain part, the pattern menu choosing, duplicating and converting; Open Pattern Editor and the Edit
  menu's entries.
