# Note expression setup of a track

A MIDI or instrument track's settings for its notes' per-note expression ([note-expression.md](note-expression.md)),
which the inspector's Note Expression section edits (#1513, reference manual pp. 1300-1302): its bypass and the input
assignments of the expressions, each with its value range. They belong to the track and are saved with the project.

## In the engine

`noteExpression.set` changes them (MCP `noteExpression_set`, Lua `gnubase.noteExpression.set`), one undo step, "Change
note expression setup"; what a call leaves out stays as it was. `project.get` lists them as the channel's
`noteExpression`, `{bypass, inputs}`, and the project keeps them in its GNUbase entry (`NoteExpressionSetup`, the
channel's `noteExpression` in `src/engine/document/document.h`).

- **Bypass** (p. 1300) plays the track's notes without their expression points: the render snapshot leaves them out
  (`compilePart` in `src/engine/document/rendersnapshot.cpp`), so neither the instrument nor MIDI sends before the
  modifiers get them, nor the MPE initial values that come before a note of a channel whose notes express. The notes
  keep their points, which play again once the bypass is off.
- **Input assignments** (p. 1301): each of the seven expressions has an input, the MIDI message mapped to it, named as
  the MPE setup's messages are (`midi::messageName`, `midi::messageNamed`): `none`, `pitchBend`, `aftertouch` or a
  controller, `cc0` to `cc127`. And its value range, `min` and `max` in the expression's unit and range
  (`expressionRange`: volume 0-4, tuning -120 to 120 semitones, the others 0-1), the whole range by default; `min`
  above `max` reverses it. `inputs` lists only the expressions that have a message or a narrower range, sorted as the
  expressions are (volume, pan, tuning, vibrato, expression, brightness, pressure), and an expression set back to no
  message and its whole range is no longer listed. `noteExpression.set` refuses other tracks, unknown expressions
  and messages and values out of range, changing nothing; a project file's values out of range load clamped, an
  unknown message as none and an unknown expression not at all.

`tests/noteexpression_tests.cpp` checks `noteExpression.set` with `project.get`, what a call leaves out, an expression
back at its default leaving the list, saving and loading, undo, the refusals and a file's values loaded clamped, and
that a bypassed track's note plays without its expression and plays it again with the bypass off.

## Not yet

- The inspector's Note Expression section (pp. 1300-1302) with its expression list and In column, Parameter Details
  with MIDI Learn, and the Global section's Show Additional Functions; the expressions' visibility, which Parameter
  Selection in the key editor's expression editor follows (p. 1309); the parameter colors.
- The track's MIDI input playing the assigned messages as the expression of its sounding notes over their ranges,
  live and when recording, and the bypass leaving out the expression of live input; MIDI Learn assigning the next
  message the track receives (p. 1304).
- Loading and saving input assignments as presets (p. 1304), Record MIDI as Note Expression and overdub (p. 1302,
  #1515).
