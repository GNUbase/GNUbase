# Controller lanes

The controller display under the key editor's note display (#128, reference manual pp. 1157-1188): lanes for the
notes' velocities, off velocities, play probabilities and velocity variances, and for controller events, which the
key editor's tools edit; lane presets, the line tool's modes and Auto Select Controllers.

## In the engine

**Notes** have two more fields (`src/engine/document/document.h`), which `note.add` and `note.set` take and
`project.get` lists unless they have their default; they are saved in the project's GNUbase entry.

| Field | What it is | Reference |
|---|---|---|
| `probability` | 0-1, the chance that the note plays; default 1 | pp. 1171-1172 |
| `velocityVariance` | -1 to 1: playback raises (positive) or lowers (negative) the velocity by up to this share of it; default 0 | pp. 1169-1170 |

- **Playback** (`src/engine/midi/controllerlanes.h`): the render snapshot leaves out the notes that don't play and
  gives the others their varied velocity, by a number from 0 to 1 that the part's and the note's ids fix. So the
  choice is the same on every playback and for any number of threads (rendering stays deterministic), and shared
  copies of a part choose apart. It is also the same on every pass of a cycle.
- **`note.apply`** (pp. 1171, 1173) makes it permanent: `what: probability` removes the notes that don't play and
  gives the others a probability of 1; `velocityVariance` writes the velocity they play with and a variance of 0. For
  the notes named, by default all of the part's. One undo step. Shared copies share their notes, so Apply in one copy
  applies that copy's choice to every copy: the others then play what it played, not their own choice from before.
- **`controller.line`** (p. 1186) draws on a lane: `lane` is `{type, number, channel}`, `from` and `to` are positions
  in the part and values, `mode` is `line`, `parabola` (with `reverse` fast first), or `sine`, `triangle` and `square`,
  which go from the start value to the end value and back every `period` ticks. On a note lane the notes starting in
  the range (or those of `notes`) take the value at their position; on a controller lane the lane's events of the
  channel in the range go and new ones come every `density` ticks and at the end, at most 65536 events. Period and
  density default to the quantize grid. `to` lies before the part's end, since playback leaves out events from there
  on. One undo step.

**Editor views** (`src/engine/document/editors.cpp`) have the lanes, view state like the rest of a view:

| View field | What it is | Reference |
|---|---|---|
| `lanes` | From the top, each `{type, number}`: `velocity`, `offVelocity`, `probability`, `velocityVariance`, or controller events: `controller` (number: the controller), `pitchBend`, `channelPressure`, `polyPressure` (number: the pitch), `program`. Default: a velocity lane | pp. 1157-1163 |
| `showLanes` | Whether the controller display shows the lanes (Show/Hide Controller Lanes) | p. 1162 |
| `autoSelectControllers` | Selecting notes selects the controller events from their start to their end | p. 1188 |
| `controllerSelection` | The selected controller events, each `{part, controller}`, of the editable parts; removed ones leave it, closing the editor clears it | p. 1188 |

**Lane presets** are the user setting `ui.controllerLanePresets`, each a name and its lanes (p. 1166).

## In the UI

`ui::ControllerLanes` (`src/ui/controllerlanes.h`) fills the key editor's `.controller-lanes`, below the note display
and on its time axis. It shows the project and the view as the key editor hands them on and sends `editor.set`,
`note.*` and `controller.*`; edits of several parts or commands go in one `history.transaction`.

```
.controller-lanes                     .lanes-hidden while showLanes is off
  .controller-lane-list               scrolls, like the note display, so that their time axes line up
    .controller-lane[lane][lane-kind]
      button.controller-lane-type     the lane's name, opens #controller-lane-menu (the lane's type, Select All
                                      Events on Lane, Remove This Lane, Apply and Reset MIDI Play Probability and
                                      Velocity Variance)
      .controller-lane-area[lane]     .controller-bar[part][note] per note (note lanes), .controller-step[part]
                                      [controller] per event, as wide as until the next one; .selected, .inactive
  .controller-lanes-footer
    #controller-lane-create           Create Controller Lane, menu #controller-create-menu: the lane types,
                                      named controllers and all 128 in Continuous Controllers; Show Used
                                      Controllers, Show Velocity Lane Only, Show/Hide Controller Lanes
    #controller-lane-setup            Controller Lane Setup, menu #controller-setup-menu: the presets, Add
                                      Preset... (controller-lane-preset.rml), Remove Preset
```

Code places and sizes the bars and steps in percent of their area; `resources/styles/controllerlanes.rcss` lays the
lanes out and the themes color them (`tools/skin/generate.py`), play probability and velocity variance bars dashed
(`lane-kind`, pp. 1169, 1171). A lane's value goes from 0 at its bottom to 1 at its top; the velocity variance's 0 is
in the middle, a program change's 0-1 is program 0-127. A poly pressure lane takes the pitch of the first selected
note. A lane shows the events of every MIDI channel: painting and the line tool replace those of every channel in
their range, and the new events get the channel of the lane's first event there, or else of its first event in the
part, or else channel 0.

| Tool | On a lane | Reference |
|---|---|---|
| Object Selection | A click selects a controller event, Shift or Ctrl (Cmd) and a click adds or removes it, a click beside the events deselects them; a drag moves the value of the event or bar pressed, and of all selected ones with a selected one | pp. 1166, 1181 |
| Draw | Paints: on a note lane the notes from the cell of the quantize grid where it started to where it ended take the value painted there, on a controller lane an event at every length quantize step, replacing those there (Ctrl: only where the mouse went) | pp. 1166, 1180 |
| Erase | A click removes the controller event | |
| Line | `controller.line` in its mode from where the drag started to where it ended, snapped to the quantize grid; the grid is the period, the length quantize the density; on a note lane for the selected notes of the part, or all; Paint paints like the draw tool. A second click on the tool's button opens `#key-line-menu` with the modes | p. 1186 |

- **Auto Select Controllers** is a button of the key editor's toolbar (`.key-input[setting=autoSelectControllers]`).
  Selected controller events move with the notes that the object selection tool moves (p. 1188).
- **Bars are thin**: a press on a note lane that misses a bar takes the note starting nearest in the grid cell.
- **Gestures**: while a drag goes on the lanes hear the mouseup in the whole document, so a drag released outside the
  lanes ends there, with its effect at that point. A left press anywhere in the document while a drag goes on means its
  mouseup never came (released outside the window): the drag ends without its effect.
- **Rendering**: `show` keeps the view and the parts it shows, not the whole project, and indexes the parts and the
  selection once, so each lane's items take a pass over the parts' notes or events.

## Not yet

The controller event editor and its smart controls (p. 1187); ramps and curves between controller events and Type of New
Controller Events (pp. 1159, 1181); the pitchbend lane's semitone grid, Grid Settings and Snap Pitchbend Events (pp. 1173-1176);
SysEx, articulation and dynamics lanes, and the Dynamics Mapping Setup (pp. 1177-1179); the MIDI Controller Setup
dialog, RPN and NRPN controllers (p. 1165); the poly pressure lane's pitch menu and editing its events with
Ctrl+Alt (pp. 1179-1180); the context menu's Cut, Copy, Paste and Paste at Origin, and pasting notes as modulation
(pp. 1160, 1180); the rhombus for controllers with automation and the value at the project cursor (pp. 1161, 1166);
the divider between the note display and the lanes (p. 1158); renaming presets in an Organize Presets dialog; a
channel per lane; a velocity variance and play probability chosen anew on each cycle pass; MIDI file export,
Merge MIDI in Loop and Freeze MIDI Modifiers applying them; the line tool's modifier keys and its use in the note
display; the selection rectangle on lanes; the info line for controller events; the in-place editor's lanes. The drum editor ([drum-editor.md](drum-editor.md))
has the key editor's lanes.

## Checks

- `tests/controllerlanes_tests.cpp`: play probability and velocity variance on `note.add` and `note.set`, their ranges
  and undo, saved and loaded; playback leaving out notes of probability 0, about half of those of 0.5 and the same ones
  each time, velocities varied within the variance up and down; `note.apply` keeping what plays and writing the played
  velocities, for named notes, with undo; `controller.line`'s line, parabola (and reversed), sine, triangle and square
  on a controller lane, replacing only that lane's events in the range, a program change lane, its errors; on note
  lanes for velocities, off velocities, probabilities and variances, named notes and undo; events it draws playing
  back through `midi.capture`; `controller.line` refusing positions at or past the part's end, also on a linear track, and
  lines of more than 65536 events; `note.apply` in a shared copy; the views' lanes, `showLanes`,
  `controllerSelection` and Auto Select Controllers.
- `tests/controllerlanes_ui_tests.cpp` drives the app with `ui::Automation`: velocity bars at their notes, dragged,
  painted and drawn with the line tool; a lane from the Create Controller Lane menu, events drawn, painted, selected,
  dragged and erased; the line tool's line, square, parabola, sine, triangle and Paint modes from its menu, which
  shows only after the second click; Ctrl painting only where the mouse went; poly pressure lanes taking the selected
  note's pitch and program change lanes; Show Used Controllers, a lane's type, Select All Events on Lane, Remove This
  Lane, play probability drawn, applied and reset, Apply and Reset MIDI Velocity Variance over two parts as one undo
  step, Show/Hide and Show Velocity Lane Only; the line tool and painting on a lane whose events are on another
  channel; drags released outside the lanes and never released; Auto Select Controllers with a note moved; a preset
  saved with Add Preset, loaded and removed with Remove Preset; events drawn on a lane playing back.
  `tests/uithemes_tests.cpp`: every theme draws play probability and velocity variance bars dashed.
