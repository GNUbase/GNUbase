# Copying automation to other tracks

Copy and paste of the automation between the locators (#125, reference manual pp. 878–879): of all tracks, of the
selected tracks or of the selected automation tracks' parameters, onto the same tracks in another range, onto other
tracks or onto other tracks' parameters. It builds on the automation lanes ([automation.md](automation.md)) and the
automation tracks ([automation-tracks.md](automation-tracks.md)). The engine part is in
`src/engine/document/eventedits.cpp`, next to range editing ([range-editing.md](range-editing.md)), whose automation
cuts and pastes it shares; the track list's commands are in `src/ui/tracklist.cpp`.

## In the engine

- **`automation.copy`** `{start, end, tracks | lanes}` copies the automation between start and end (project samples,
  default the locators) of every track, of the tracks or of the lanes, as `range.copy` copies a range's automation:
  each lane with points, with the curve's values at the range's edges and its gaps. It has a clipboard of its own,
  apart from the range clipboard, keeps it outside the project and its undo history, and returns how many lanes it
  copied. A new or loaded project clears this clipboard.
- **`automation.paste`** `{start, end, tracks | lanes}` pastes it between start and end, one undo step (Paste
  Automation), and returns the lanes it pasted into:
  - without tracks and lanes each lane goes onto its own track (Paste Automation of All Tracks), in the new range;
  - onto tracks, the copied tracks go onto them from the top, the first onto the first; a single copied track goes
    onto each of them;
  - onto lanes, each gets the copied lane of its parameter, if there is one.
- **Other parameters.** A track takes the copied parameters it has and leaves out the others: volume, pan and mute on
  tracks with audio channels, a VCA fader's volume on VCA tracks, MIDI controllers, pitch bend and channel pressure
  on MIDI and instrument tracks, an instrument's parameters on a track with the same instrument, and a plugin's
  parameters only when the slot at the same place holds the same plugin (p. 878). The track's lane for the parameter
  gets the automation, and is added if it has none.
- **Lengths** (p. 879). The paste replaces the automation in the range, and the curve outside goes on as before, as
  `range.paste` does. A range shorter than the copy cuts it at the curve's value there; in a longer one the copy's
  last value holds up to the range's end.

## In the track list

```
#track-list listitem
  .automation-row[lane=<id>]    .selected while its automation track is selected
```

- **Selecting automation tracks.** A click on an automation track's row (not on its buttons) selects it, Ctrl or
  Shift adds or removes it; a click on a track's own row deselects them. A right click on an automation track that
  isn't selected selects it, for the context menu. `TrackList::selectedLanes()` has them.
- **Commands** in the track list's context menu and as key commands (without keys of their own, as in the reference
  manual): `track.copyAutomationAll`, `track.copyAutomationSelected` and `track.copyAutomationSelectedParameters`
  (Copy Automation of All Tracks, of Selected Tracks, of Selected Parameters (Locator Range)), and
  `track.pasteAutomationAll`, `track.pasteAutomationSelected` and `track.pasteAutomationSelectedParameters` (Paste
  ...). They send `automation.copy` and `automation.paste` with the selected tracks or lanes; the engine takes the
  locators.

## Not yet

The automation panel's commands (p. 882), pasting one parameter's automation onto another parameter, and the
Automation Functions menu (p. 889).

## Checks

- `tests/automation_tests.cpp`: copying selected tracks and pasting them onto tracks with other parameter sets (a
  plugin in the same slot and another plugin in the next, a MIDI track that takes only the MIDI controller), replacing
  the automation in the range, one undo step; a shorter and a longer range; one parameter onto the lanes of the same
  parameter, one copied track onto several tracks; all tracks onto all tracks; pasting a VCA fader's volume and
  finding nothing to paste after a new project; the checks.
- `tests/automationtracks_tests.cpp`: selecting automation tracks with clicks and Ctrl, and the track list's copy and
  paste commands for selected parameters and selected tracks.
