# Virgin territory

Automation without data, as the reference manual describes it on p. 881 (#124): a lane's curve ends at a terminator
point, and with the project's Use Virgin Territory setting before its first and after its last point too. Where a lane
has no curve, a gap, its parameter has its static value, as if the lane didn't read: the fader, the instrument's or
plugin's parameter as the user sets it. The lanes and their playback are in [automation.md](automation.md).

## In the engine

- **`automation.set`** `{virginTerritory}` switches between the two modes, `automation.get` reports it. It is saved
  with the project (in GNUbase's entry of the file), not part of undo, so that a project renders the same wherever it
  is opened; a new project starts with initial values. Emits `automation.changed`.
  - **Initial value** (the default): a lane's curve covers the whole timeline: before its first point the first
    point's value, after its last point the last one's.
  - **Virgin territory**: no curve before the first point and after the last one.
- **Terminator points** (`automationPoint.add` and `automationPoint.set` `terminator`, listed only when true): the
  gap starts at the point and goes up to the next one, in both modes. The value arrives at the terminator's value on
  the curve to it and jumps to the static value there, like points at one position (the last one is the value from
  there on).
- **Playback**: `compile()` (`rendersnapshot.cpp`) adds a gap point, a step marked `gap`, at a terminator's position,
  and with virgin territory one at the first and one at the last point's position. `channelAutomation()` gives gap
  points the lane's static value, and a mute lane's gaps have the mute button's value, since a mute lane that plays
  overrides the button. So the automation player plays these gaps as ordinary steps, sample-accurately and from any
  position. MIDI lanes without a static value of the channel (any controller but a MIDI channel's volume and pan,
  pitch bend, channel pressure) keep their gap points and send nothing in their gaps, also at a locate or while
  stopped there, like a lane that doesn't read; the value after a gap is always sent.
- **Range edits** (`range.delete`, `range.cut`, `range.copy`, `range.paste`, `range.insertSilence`, in
  `eventedits.cpp`) keep the gaps: an edge in a gap gets no point with a value, but a terminator or nothing. With
  virgin territory `range.delete` makes the range a gap (p. 881): the point at its start is a terminator, and across
  a lane's last point the curve ends there. Delete Time closes the range and leaves no gap; Insert Silence in a gap
  makes it longer. Copied automation keeps its gaps as terminators.

## Not yet

Gaps on the automation tracks (they still draw the curve over the whole timeline), the info line's Terminator field,
the automation panel's settings page.

## Checks

- `tests/automation_tests.cpp`: the setting and terminators through the engine API, saved with the project, not undo
  steps, and the terminator's undo; the values of a volume and a mute lane in both modes, and a controller lane that
  sends nothing in its gaps, also at a locate and while stopped; volume automation rendered with both modes and block
  sizes 64 and 100, sample by sample.
- `tests/eventedit_tests.cpp`: `range.delete` across a terminator and, with virgin territory, in a lane's curve and
  across its last point, Delete Time, Insert Silence in a gap, and a copy with a gap pasted.
