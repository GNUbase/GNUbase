# Transpose track and project root key

Global transposition (#168, reference manual pp. 165, 414-421): the Project Root Key, the transpose track with its
transpose events and controls, and MIDI parts that follow it or are excluded from it.

## Engine

- **Project Root Key**: `project.set` `rootKey`, `""` for none or `C`, `C#` and so on to `B`; in `project.get` and
  `project.changed`. An undo step, saved with the project.
- **A part's root key and Global Transpose**: `midiPart.set` `rootKey` (the same names) and `globalTranspose`,
  `follow` (the default) or `independent`; in the part's JSON, saved with the project, kept by splits and copies.
  `transpose.assignRootKey` (Set Root Key for Unassigned Events, p. 416) gives every part without a root key the
  project's; refused without one. `project.get` gives each part `rootKeyTranspose` too, the step that the project
  root key adds to it (see Playback); it is derived, so the project file doesn't keep it.
- **The transpose track** (`TrackType::Transpose`, `"transpose"`) has no channel, stays at the top level and is there
  at most once. Its events and controls are `Project::transpose`; removing the track removes them. Switching its time
  base and changing the sample rate keep the events where they are, as for markers.
- **Transpose events**: a position in the track's time base and a value from -24 to 24 semitones, in timeline order,
  at most one per position. Each plays from its position to the next one; before the first one nothing is
  transposed. `transpose.add` (default: the cursor, value 0; adds the transpose track if there is none),
  `transpose.set` (moves or changes events) and `transpose.remove` are undo steps; `transpose.changed` announces
  every change with what `transpose.get` returns.
- **Controls** (`transpose.setTrack`, an undo step): `mute` (Mute Transpose Events: the events don't play),
  `keepInOctave` (Keep Transpose in Octave Range) and `lock` (the events can't be added, changed or removed).
- **Recording** (p. 420): a recorded part follows global transpose with the project root key, or, if the project has
  transpose events, is independent without a root key.

## Playback

`globalTranspose` (`document.h`) is what a part adds at a position: nothing for an independent part; otherwise the
step from its root key to the project root key the nearer way (-5 to 6 semitones, nothing unless both have one), plus
the value of the transpose event playing there unless the track is muted. With Keep Transpose in Octave Range the sum
stays within 7 semitones either way (8 up plays 4 down). The render snapshot adds it to each note's MIDI modifiers'
transpose at the note's start, with the part's own transpose and the track's modifiers, before their ranges.

## UI

- **The toolbar** has the Project Root Key (`#root-key` with `#root-key-menu`), which the event display shows and
  sets.
- **The info line** has a MIDI part's Root Key and Global Transpose ([inspector.md](inspector.md)). Its Transpose
  field shows what the project root key adds in brackets after the part's own transpose (p. 420), such as `(+2)`
  for a part in C in a project in D, from `rootKeyTranspose`.
- **Independent parts** show an icon (`part-independent.svg`) in their upper right corner in the event display
  (p. 421): a `.part-mark.independent` with attribute `part` in the display's `.part-marks`.
- **The transpose track's lane** (`ui::TransposeLane`, `src/ui/transposetrack.h`) shows each event's value from its
  position: the draw tool adds an event at the nearest bar and opens its value for typing, a double click opens it
  too, a drag moves it to the nearest bar and the erase tool removes it.
- **The track list** has the transpose track's controls by default (`ui.trackControls` `transpose`): Mute Transpose
  Events (`transposeMute`), Keep Transpose in Octave Range (`keepInOctave`), Switch Time Base and Lock
  (`transposeLock`).
- **Add Track** has Transpose, once per project.
- **The track list's context menu** has Set Root Key for Unassigned Events (`transpose.assignRootKey`, p. 416),
  enabled while the transpose track is selected, as a right click on it selects it.

## Checks

`tests/transpose_tests.cpp`: the pitches the render snapshot plays for the root key (both ways, with the part's own
transpose, Independent, undo), transpose events (adding the track, timeline order, Keep Transpose in Octave Range,
mute, moving, the value range, lock, the track's removal), Set Root Key for Unassigned Events, recorded parts with and
without transpose events, and saving and loading. `tests/transpose_ui_tests.cpp`: the toolbar's root key, the info
line's Root Key, Global Transpose and the root key's step, the mark of Independent parts, drawing, typing, dragging,
double-clicking and erasing transpose events on the lane, the track list's controls and its context menu's Set Root
Key for Unassigned Events.

## Not yet

Audio events: their transposition and root key need real-time pitch shifting in the disk streams (#144), and the
pool's Key column; live MIDI input through the
transpose track; Indicate Transpositions in the key editor; the transpose track's controls in the track controls dialog; drums and effects tagged Independent by default; selecting several transpose events and the Delete key.
