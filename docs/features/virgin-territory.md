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
  points the lane's static value; MIDI lanes without one of the channel's have those of `automationLane.targets`
  (pitch bend 0.5, otherwise 0). A mute lane's gaps have the mute button's value, since a mute lane that plays
  overrides the button. So the automation player plays gaps as ordinary steps, sample-accurately and from any
  position, with nothing new on the audio thread.

## Not yet

Gaps on the automation tracks (they still draw the curve over the whole timeline), the info line's Terminator field,
and deleting a range of automation with the range selection tool, which makes a gap with points at both ends; the
automation panel's settings page.

## Checks

- `tests/automation_tests.cpp`: the setting and terminators through the engine API, saved with the project, not undo
  steps, and the terminator's undo; the values of a volume and a mute lane in both modes, and of MIDI lanes' gaps;
  volume automation rendered with both modes and block sizes 64 and 100, sample by sample.
