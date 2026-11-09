# Rule editor and input transformers

Rule-based MIDI transforming (#178, reference manual pp. 1392-1423 and the track input transformer, pp. 1097-1099):
rules find MIDI events and change, copy or remove them, in MIDI parts (the rule editor) or in what a track's MIDI
input plays (the input transformers). A preset library holds named rules for both.

- **Rules** (`src/engine/midi/logical.h`) are filter lines, a function and action lines. They see an event as its type
  (note, poly pressure, controller, program change, aftertouch, pitch bend), its MIDI channel (1 to 16), its subtype and
  main value in MIDI 1.0's range (a note's pitch and velocity, poly pressure's pitch and pressure, a controller's number
  and value, a program change's program, aftertouch's pressure as the subtype, pitch bend's low and high 7 bits), its
  secondary value (a note's note-off velocity) and its position and length, in ticks of 480 to a quarter note from the
  project's start, as the reference shows them, so a preset means the same in every project. SysEx isn't seen.
  - **Filter lines** each test a target: equal, unequal, bigger, bigger or equal, less, less or equal, inside or outside
    a range (parameters 1 and 2, both included in the range) and, for the subtype, note is equal to (the same pitch class
    in any octave); the type is equal or unequal to a type. Lines join with and or or, and binding more tightly than or,
    and up to three brackets open before and close after each line; brackets must match. No lines find every event.
  - **Functions:** delete (the input transformer's Filter), transform (found events change by the actions), insert
    (changed copies are added beside the events found), insert exclusive (found events change, the others go) and, in
    the rule editor only, select (finds and changes nothing).
  - **Action lines** change a target in order, each from what the ones before left: add, subtract, multiply by, divide
    by (by 0 leaves the value), round by (to the nearest multiple), set random values between and set relative random
    values between (whole numbers in the range, either way round), set to fixed value, use subtype (main value only),
    use main value (subtype only), mirror around parameter 1 (subtype and main value), transpose to scale (subtype only:
    the closest pitch of the scale with root parameter 1 and the scale type of parameter 2, the lower one on a tie, as
    MIDI > Transpose Setup's scale correction finds it) and add length (position only). The type can only be set to a
    fixed type. After the actions, a note whose pitch left 0 to 127 is deleted, as the transformer MIDI effect deletes
    it; velocities stay from 1 to 127, other values from 0 to 127, channels from 1 to 16, positions from 0 and lengths
    from 1 tick. Random values come from a generator seeded per command, so the same seed gives the same values on every
    platform.
  - JSON (`toJson`, `logicalRulesFromJson`, `logicalRulesSchema`): `{filters: [{open, target, condition, parameter1,
    parameter2, close, join}], function, actions: [{target, operation, parameter1, parameter2}]}` with names in camel
    case (`mainValue`, `insideRange`, `insertExclusive`, `setRelativeRandom`); a type is its name (`"note"`), a scale
    type too (`"major"`).
- **Rule editor:** `midi.applyRules` applies rules, or a preset by name, to the notes of MIDI parts (all, or those
  given) and, without a note selection, to their controllers, pressure, pitch bend and program changes, in one undo
  step. Values the rules don't change keep their full resolution (a velocity left alone stays as precise as it was).
  Events don't move before their part's start; in parts, notes don't turn into other events or the other way round
  (the type stays). Insert's copies get new ids. `midi.findByRules` returns the notes and controllers the filters find
  and changes nothing, which is what select does; `midi.applyRules` refuses select.
- **Input transformers:** a MIDI or instrument track's `inputTransformer` in `channel.set` and `project.get`, `{mode,
  modules}`: off, track (the track's own up to four modules) or project (the project's modules, set with
  `midi.setInputTransformer`, part of the project and its undo, in `project.get`'s `inputTransformer`). Each module is
  `{on, rules}`; the modules that are on change, in order, what the track's MIDI input plays (`midi.inject` and the MIDI
  thru), before it plays and records it. Both are saved in GNUbase's entry of the project file. The audio engine runs
  them on the audio thread (`processors::InputTransformerProcessor`, made on the command thread when a track first
  has rules and kept with the track from then on, while the rules come with each render snapshot), on buffers
  allocated beforehand.
  - **Live events** (`processors::LogicalStage`): rules see note-ons; a note-off, and the poly pressure of a held note,
    go where its note-on went, so a note ends where it sounds even when the rules changed in between or insert played it
    twice. A note-on that became another kind of event loses its note-off. A controller or other event turned into a
    note plays a note-on for a main value above 0 and a note-off for 0, so a sustain pedal can play a bass drum.
  - **The transformer MIDI effect** (#135, [midi-effects.md](midi-effects.md)) is a rule of this kind with a fixed shape
    (type is note and a range of pitch, velocity or channel; one action or delete) and plays through the same
    `LogicalStage`.
- **Presets** belong to the user, as quantize presets do ([quantize.md](quantize.md)): `midiRules.presets` lists the
  factory presets, named by category (`Init`, `Delete/Short notes`, `Transform/Octave up`, `Transform/Aftertouch to
  modulation`, `Input/Sustain pedal to bass drum` and more), and the user's, each with its rules. The user's are user
  settings, `midi-rule-presets.json` in the settings folder (`logicalPresetsFormat()`), saved with `midiRules.savePreset`
  and removed with `midiRules.removePreset`; factory names can't be taken. Each change starts from the file as it is then,
  so GNUbase processes on one settings folder keep each other's presets. A preset this GNUbase can't read is skipped
  with a problem in `midiRules.presets` and stays in the file. One library serves the rule editor and the input
  transformers; input transformers refuse select.

## In the UI

MIDI > Rule Editor... (`midi.applyRulesor`) opens the rule editor (`resources/ui/rule-editor.rml`) for what
the MIDI functions work on ([midi-functions.md](midi-functions.md)): an editor's selected notes, or the selected MIDI
parts. Project > Project Input Transformer... (`project.inputTransformer`) opens the same document for the project's
modules. It shows the presets, the filter lines (brackets, target, condition, parameters, and/or), the function and the
action lines, each line with Insert and Remove; Save Preset saves what it shows under a name. Apply runs
`midi.applyRules` (select shows how many events `midi.findByRules` found), or, for the input transformer, sets the
module shown with `midi.setInputTransformer`.

## Not yet

Filter targets beyond the event's values (Property, Last Event, Context Variable, the score editor's voices and staves)
and the position conditions that need the transport (bar ranges, cursor, cycle, track loop, markers); time bases other
than ticks; the operations for note expression, VST 3 values, the score editor, loop range ramps and Move to Cursor;
the functions Copy, Extract to Track, Extract to Lanes and Deselect, and select changing an editor's selection; type
changes between notes and other events in parts; dragging events into the filter list; a presets browser with folders,
search and Show User Presets Location; MIDI > Rule Editor > Apply Preset; the inspector's Input Transformer pop-up
and the track input transformer's panel.

## Checks

`tests/logical_tests.cpp`: filter conditions, and before or, nested brackets; every operation, the value ranges and
repeatable random values; every factory preset round-tripping through JSON and the rules JSON refuses; `midi.applyRules`
with presets and rules (delete, transform, positions, insert, insert exclusive, a note selection, pitches leaving the
range, seeds), on controllers with type changes, `midi.findByRules` and undo; the preset library (listing, saving,
removing, the file in the settings folder, a preset that can't be read); input transformers in track and project mode
through `midi.inject` and `midi.capture` (note-offs following their note-ons while the rules change, modules in order,
filter, insert, insert exclusive, a sustain pedal playing a bass drum), undo, saving and loading, and what
`channel.set` refuses. `tests/midieffects_tests.cpp`'s golden files check that the transformer effect plays as before.
`tests/rtcheck_tests.cpp` runs input transformers without allocating.
