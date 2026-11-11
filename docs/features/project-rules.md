# Project rule editor

Rule-based search and replace in the project window (#179, reference manual pp. 1424-1445): rules find tracks, parts
and events and delete, change, select or deselect them, with key commands that run before and after. A preset library
holds named rules, as the MIDI rules have one ([midi-rules.md](midi-rules.md)).

- **Rules** (`src/engine/document/projectrules.h`) are filter lines, a function, action lines and pre and post
  commands. They see the project's tracks from top to bottom, then its audio and MIDI parts, audio events and markers.
  Positions and lengths are in ticks of 480 to a quarter note from the project's start, as the MIDI rules have them.
  - **Filter lines** test a target: the media type (audio, MIDI for MIDI and instrument tracks and parts, marker,
    tempo, signature, arranger, transpose, chord, group, effect, VCA; folders and ruler tracks have none) and the
    container type (folder, track, part, event) with equal, unequal or all types; the name (an audio event's is its
    clip's) and the color name (the name the project's colors give the element's color, a part's or event's own or else
    its track's) with equal, unequal, contains and contains not, case sensitive; the position and length with the
    numeric conditions of the MIDI rules (tracks have neither, so they never match); and a property with is set or is
    not set: is muted (a track's channel, a part, an event), is selected and parent is selected (by the ids the command
    is given as `selected`: the project window's selection of tracks, parts, events and markers), is empty (tracks
    without events, parts or markers, folders without tracks, parts without notes, controllers or events), is hidden
    (track visibility), has track version, follows chord track and is disabled. Brackets and and/or work as in the MIDI rules,
    through the same reader (`midi::evaluateLines`).
  - **Functions:** delete, transform, select and deselect. Select and deselect change nothing: the UI selects what was
    found, or deselects everything.
  - **Action lines** change, in order: position and length (add, subtract, multiply by, divide by, round by, random,
    relative random, fixed value; each result stops at plus or minus the longest project, `time::g_maxPosition`
    samples or, below 8 kHz, as long as at 8 kHz, so that its ticks fit at any sample rate; it stops at the project's
    start when it is applied, and a cycle marker stays at least one sample long), name (replace, append, prepend, generate name: text and a
    number counting up from parameter 2 over the elements that are renamed, so locked parts and audio events take
    none, replace search string, erase before, erase after, erase the first or last character), track operations
    (mute, solo, record, monitor: on, off or toggle; time domain: musical, linear or toggle) and color (a project
    color's name or `#rrggbb`, the next or previous project color).
  - **Pre and post commands:** up to four key commands each ([key-commands.md](key-commands.md)), engine or UI
    commands with parameters, kept with the rules and their presets. The UI runs them before and after the rules.
  - JSON (`toJson`, `projectRulesFromJson`, `projectRulesSchema`): `{filters: [{open, target, condition, parameter1,
    parameter2, close, join}], function, actions: [{target, operation, parameter1, parameter2}], pre: [{command,
    parameters}], post}`; parameter 1 is text for the text targets.
- **Commands:** `project.applyRules` applies rules, or a preset by name, in one undo step, through the commands that
  make such changes (`event.move`, `event.resize`, `event.set`, `event.remove`, `track.set`, `track.remove`,
  `channel.set`, `marker.set`, `marker.remove`), so their checks and events are the same. A found folder takes its
  tracks with it; locked parts and events stay on the tracks that stay; an audio event inside a found audio part goes
  with the part, and delete keeps an audio part that holds a locked event, while the unlocked events found in it go.
  An audio part that moves takes its events along, locked ones too, as `event.move` does. The active marker track is
  removed after the other marker tracks, so that no marker track that goes is activated in passing. `seed` makes
  random values repeatable. `project.findByRules` returns what the filters find (`tracks`, `events` for parts and audio
  events, `markers`) and changes nothing; `project.applyRules` refuses select and deselect. `event.set` renames parts
  (`name`) for the name actions.
- **Presets** belong to the user: `projectRules.presets` lists the factory presets (`Init`, `Delete/Empty parts`,
  `Select/MIDI parts`, `Transform/Number the selected tracks` and more) and the user's, kept in
  `project-rule-presets.json` in the settings folder; `projectRules.savePreset` and `projectRules.removePreset` change
  them. The library (`src/engine/rulepresets.h`) is the MIDI rules' one, made generic: each change starts from the file
  as it is then, and presets this GNUbase can't read are skipped with a problem and stay in the file.

## In the UI

Project > Project Rule Editor... (`project.ruleEditor`) opens the rule editor document (`resources/ui/rule-editor.rml`)
for the project: presets, filter lines, function, action lines and the pre and post commands, one command name per
field. Apply runs the pre commands with `gnubase.ui.command`, then `project.applyRules` (select and deselect:
`project.findByRules`, and the track list and the event display select what was found), then the post commands, and
closes. The project window selects events or markers, so found markers are selected when the rules find no parts or
audio events, and Deselect deselects markers too. The track list's, the event display's and the markers' selection go
to the rules as `selected`.

## Not yet

Selecting parts or audio events together with markers, as long as the project window selects one or the other;
automation tracks and points, arranger, transpose, tempo, signature and chord events and video; the conditions that
need the transport or a bar range (inside bar range, cursor, cycle, track loop, selected marker) and time bases other
than ticks; the properties Inside NoteExp and Is valid VST 3; the actions Trim, Move to Cursor, Folder, Read, Write,
EQ, inserts and sends bypass, insert and send slots, Lanes Active, Hide Track, Connect Input and Output and the volume
operations; Output Name; renaming audio events; Project > Project Rule Editor > Apply Preset and key commands for
presets; a presets browser with folders, search and Show User Presets Location; dragging commands between the pre
and post lists and a command browser.

## Checks

`tests/projectrules_tests.cpp`: every factory preset round-tripping through JSON with pre and post commands, and the
rules JSON refuses; `project.findByRules` on every filter target, brackets, properties and the selection;
`project.applyRules` moving, resizing, renaming and coloring parts and markers, track names, mute, solo, time domain
and colors, delete with folders, locked parts, audio parts that hold locked events and the active marker track last,
generated names without gaps, positions and lengths at the longest project, also at sample rates far below 8 kHz,
cycle markers made zero long on a musical track, and repeatable random values, in one undo step, undone and redone; `event.set` renaming parts; the preset library (listing, saving, removing, the file in the settings folder, a
preset that can't be read). `tests/projectrules_ui_tests.cpp` drives Project > Project Rule Editor in the app with
`ui::Automation`, selecting parts and markers.
