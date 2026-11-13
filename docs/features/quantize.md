# MIDI quantizing

Quantizing notes to grids and grooves (#132).

- **MIDI quantizing** (`src/engine/midi/quantize.h`, #132) moves note starts or ends toward a grid or a groove, or sets note lengths. A grid is a note value (straight, triplet or dotted) with optional tuplets and swing; it is in project time and starts again at every bar, so it follows time signature changes. A groove is a list of positions repeating every groove length, given directly or extracted from a MIDI part. Strength, catch range, safe range, randomizing (seeded per note, so the same seed gives the same result on every platform) and, for grooves, pre-quantizing, max. move and velocity and length strengths shape how notes move. A quantized note keeps its timing from before it was first quantized and what quantizing made of it (`Note::origin`): quantizing again starts from the original unless it is soft (iterative), `quantize.reset` puts the original back and `quantize.freeze` drops it. A note whose timing no longer is the quantized one was changed by other means, such as `note.set`, so its original no longer counts; other commands don't need to know about quantizing. The commands are `quantize.*` (`quantizecommands.h`), registered through `ProjectModel::addEdit`. The project keeps its current quantize settings and the auto quantize switch, saved in the DAWproject extension entry but not undo steps; recording applies auto quantize ([midi-recording.md](midi-recording.md)). Presets (the factory grids and the user's, with grooves) belong to the user, not to a project, so every project has them. The user's presets are user settings (#278, see [settings.md](settings.md)): `quantize-presets.json` in the settings folder (`quantizePresetsFormat()`), a versioned file whose setting `quantize.presets` lists each preset's name and settings as `quantize.presets` returns them, groove points included; factory presets aren't written. The engine reads it when it starts and saves it whenever a preset is saved, renamed or removed. A preset that can't be read (settings out of range or of a newer GNUbase, no or a repeated name, a factory preset's name) is skipped with a log message and stays in the file, so the GNUbase that wrote it still has it. Its name stays taken: saving or renaming a preset to it fails, and `quantize.removePreset` removes it. `quantize.presets` reports the file, whether changes are saved (`readOnly`, as while the file belongs to a newer GNUbase) and the problems found when it was read, skipped presets included. Several GNUbase processes can share the file: each change starts from the presets in the file as they are then (`settings::Store::modify`), so no process drops another one's presets, and the process sees them from then on, even if the change fails. If the file can't be written, the command fails and nothing changes.
- **The preset the settings are** (#770). Presets can have equal settings, such as a user preset saved from a factory
  one's. So that every client (the quantize panel, the toolbar's quantize section, MCP and Lua) shows the same one, the
  engine reports it: `quantize.get` and `quantize.changed` have `preset`, the preset last chosen with `quantize.set`,
  saved or renamed while the settings are its own (`Project::quantizePreset`), else the first of the user's presets with
  the settings, else the factory one, or null for none. The project forgets the preset once the settings change, so
  changing them back shows the user's preset first. Saving, renaming or removing a preset, or extracting a groove, can
  change the one shown without changing the settings: then those commands announce `quantize.changed` too. The name is
  saved with the project, as only a name: where the user has no such preset, or it has other settings, another preset
  with the settings shows. So a preset command that changes the preset shown changes the project: it advances
  `project.revision` without an undo step (`ProjectModel::setQuantizePreset`), so the project counts as modified, asks
  to be saved when it closes and is auto saved. One that leaves the preset shown as it was leaves the project as it was:
  a preset saved with the settings that already shows isn't remembered again.

## In the UI

The Edit menu, the quantize panel and the project window toolbar's quantize section (reference manual pp. 367-378).
The app adds the commands in `src/app/appquantize.cpp`; `ui::QuantizePanel` (`src/ui/quantizepanel.h`) is the panel
and the toolbar's section.

- **Edit menu.** Quantize (`edit.quantize`, Q), Reset Quantize (`edit.resetQuantize`), Quantize Panel
  (`window.quantizePanel`) and Advanced Quantize: Quantize MIDI Event Lengths (`edit.quantizeLengths`), Quantize MIDI
  Event Ends (`edit.quantizeEnds`), Freeze MIDI Quantize (`edit.freezeQuantize`) and Create Groove Quantize Preset
  (`edit.createGrooveQuantize`, which makes a preset of the first MIDI part with `quantize.extractGroove` and makes
  it the project's settings). They are UI commands that send `quantize.apply`, `quantize.reset`, `quantize.lengths`,
  `quantize.ends` and `quantize.freeze`, so what they do is what MCP and Lua can do.
- **What they quantize.** Run from an editor (the editor window, the lower zone while it shows the Editor tab, or an
  in-place editor in the project window, [in-place-editor.md](in-place-editor.md)), the
  editor's selected notes, or with none selected every note of the parts it edits; Quantize MIDI Event Lengths takes
  the editor's length quantize there (Quantize Link: the grid's step). Run from the project window, its selected MIDI
  parts. Run without a zone, as the quantize panel, `gnubase.ui.command` and MCP run them, an open editor's selected
  notes, else the project window's selected parts. The notes of several parts are one undo step
  (`history.transaction`). Menus disable the items while there is nothing to quantize.
- **Quantize panel.** A panel of the project window's right zone (`quantize`, which can go into a window of its own),
  opened by Edit > Quantize Panel. It shows the project's settings as `quantize.changed` has them, whichever client
  changed them: the Quantize Presets menu, which shows the preset the settings are as the engine reports it (`preset`,
  see above, so the toolbar shows the same one; No Preset for none), and a name field with Save Preset, Rename Preset
  and Remove Preset (the user's presets only); Soft Quantize and the strength (Position for a groove); for a grid, Grid
  Type, Quantize Tuplet, Quantize Swing (disabled unless the grid is straight without tuplets) and Catch Range; for a
  groove, Velocity, Length, Pre-Q, Max. Move and Orig. Position; Safe Range and Rough Quantize. Ticks show as the
  reference counts them, 120 to a 1/16. The grid display shows a bar of 4/4 of the grid's or groove's positions
  (`QuantizeGrid`, as quantizing finds them) with the catch range around each. Every change is a `quantize.set`. Apply
  and Reset run `edit.quantize` and `edit.resetQuantize`; with Auto Apply on, every change runs `edit.quantize` too.
- **Toolbar.** Apply Quantize, the Quantize Presets menu, Soft Quantize and Open Quantize Panel, after the snap
  controls. The section shows where the toolbar has room for it ([docking.md](docking.md)).

Audio events and parts, Warp Quantize On/Off and the panel's section for several audio tracks are in
[audio-quantize.md](audio-quantize.md).

Not yet: Move MIDI Controllers (moving controllers with their notes) and dragging a part onto the grid display to make
a groove; the Edit menu's items always take the project window's selection, since the menu bar is in its zone.

`tests/quantize_ui_tests.cpp` drives all of it in the app's UI with `ui::Automation`, as MCP's UI tools do.
