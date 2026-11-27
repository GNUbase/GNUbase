# Note expression setup of a track

A MIDI or instrument track's settings for its notes' per-note expression ([note-expression.md](note-expression.md)),
which the inspector's Note Expression section edits (#1513, reference manual pp. 1300-1302): its bypass, the input
assignments of the expressions, each with its value range, and which expressions the editors show. They belong to the
track and are saved with the project.

## In the engine

`noteExpression.set` changes them (MCP `noteExpression_set`, Lua `gnubase.noteExpression.set`), one undo step, "Change
note expression setup"; what a call leaves out stays as it was. `project.get` lists them as the channel's
`noteExpression`, `{bypass, inputs, hidden}`, and the project keeps them in its GNUbase entry (`NoteExpressionSetup`,
the channel's `noteExpression` in `src/engine/document/document.h`).

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

- **The inspector's Note Expression section** (pp. 1300-1304, `src/ui/inspector.cpp`): its bypass button, a row per
  expression with its input's short name in the In column (PB, AT or the controller's number), of which a click
  selects one, and the selected one's Parameter Details: Input (Off, Pitchbend, Aftertouch, CC 0-127), MIDI Learn and
  its Max. and Min. Value, in the expression's range. MIDI Learn listens to the MIDI inputs (`midi.activityListen`,
  `midi.activity`) until the next controller, pitch bend or aftertouch the track receives, which becomes the selected
  expression's input: from the inputs among all MIDI inputs (`midi.ports` `inAllInputs`) while the track's MIDI input
  is All MIDI Inputs, else none. It ends when another expression is selected or the inspector shows another track.
  Every change is a `noteExpression.set`.
- **Visibility** (p. 1301, #2120): `hidden` lists the expressions that the key editor doesn't show, in the
  expressions' order, none by default; a call with `hidden` replaces the list. Over a track's notes in the key editor
  and the in-place editors, a hidden expression draws no lines, and the expression editor's Parameter Selection lists
  only the expressions that the selected notes have points of or their tracks show (p. 1309). A change of the list
  emits `noteExpression.changed` besides `channel.changed`, so that the editors read the notes again, which they don't
  after a step of channels only. In the inspector each expression's row has its visibility button. The Global
  section's Show Additional Functions (p. 1302) opens a menu: Show only used Expressions shows only the rows of the
  expressions that the track's notes have points of, which are marked with an asterisk (\*) whether it is on or not;
  Make all Expressions visible empties `hidden`, and Make only edited Expression visible hides all but the selected
  expression.
- **Live input** (p. 1303): the track's MIDI input plays each assigned message as the expression of the notes the
  input holds on the message's channel, every one of them getting the same value, over the expression's range: the
  message's lowest value plays `min`, its highest `max`. The sequencer turns them into note expression events before
  the inserts above the divider and the sends before the modifiers get the input, in place of the message, which no
  longer plays (`MidiSequencer::assignExpression`, `src/engine/audio/sequencer.cpp`); one message can play several
  expressions, and a message with no note held plays nothing. The track records the input with them, so they record
  into the notes it records as their expression points (#1515). The bypass leaves the input's expression out of what
  plays, MPE input's too, but not out of what records.

- **Recording** (pp. 1302, 1316, #1515): what the input plays of an assigned message records into the notes that sound
  as their expression points, as MPE input does, and not as controller events; every note held on the message's channel
  gets the value.
- **Convert to Note Expression** (p. 1316, #1515): `noteExpression.convert` turns a part's controller events whose
  message the setup assigns into points of the notes that sound at them on their channel, over the expression's range,
  and removes them from the part, one undo step ("Convert to note expression"). An event that no note sounds at stays,
  so that nothing is lost, as the reference's release phases keep it. MIDI > Note Expression > Convert to Note
  Expression (`midi.convertToNoteExpression`, `src/app/appmidifunctions.cpp`) runs it on the parts the MIDI menu's
  functions take: an editor's (those with selected notes, else all it edits) or the project window's selected ones,
  several in one undo step.

`tests/noteexpression_tests.cpp` checks `noteExpression.set` with `project.get`, what a call leaves out, an expression
back at its default leaving the list, saving and loading, undo, the refusals and a file's values loaded clamped, and
that a bypassed track's note plays without its expression and plays it again with the bypass off, and that the input
plays a controller assigned to two expressions as the expression of the notes it holds, with and without the bypass,
records an assigned controller into the notes that sound, and that `noteExpression.convert` turns a part's assigned
controllers into its notes' points, leaves the others and those no note sounds at, and undoes in one step.
`tests/noteexpression_ui_tests.cpp` checks Convert to Note Expression in the MIDI menu on an editor's two parts;
Parameter Selection and the lines over the notes without the hidden expressions but those a note uses; and the
inspector's visibility buttons, the used expression's asterisk and Show Additional Functions' three items. The engine
tests check `hidden` with its order, what a call leaves out, `noteExpression.changed`, saving, loading and undo.

## Not yet

- In the inspector's Note Expression section, the parameter colors (p. 1301), which the key editor would draw the
  expressions in, and Show Additional Functions' MIDI Controller Setup (pp. 1165, 1302, #737) (#2120).
- Loading and saving input assignments as presets (p. 1304, #2120); Record MIDI as Note Expression, Overdub Note Exp.
  and its Latch Buffer (p. 1302): recording into the notes of the parts that play, which the input doesn't hold (#1515);
  the release phases of Convert to Note Expression (p. 1316, #1515).
