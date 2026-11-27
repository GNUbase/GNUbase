# Info line and inspector

The project window's info line and the basic sections of its inspector (#89, reference manual pp. 72, 76–79,
167–188): the info line shows the selected event or part and edits it, the inspector in the left zone shows the
selected track. Both are clients of the engine like the track list: they read the project the event display last read
(`EventDisplay::project`) and send engine commands, so whatever another client changes shows at once, and MCP and Lua
make the same changes with the same commands.

## Info line

`ui::InfoLine` (`src/ui/infoline.h`) fills the strip `#info-line` (`resources/ui/project.rml`), which Set up Window
Layout and Mod+I show and hide ([docking.md](docking.md)). It follows the event display's selection and asks
`event.get` for the selected events and parts; the one that starts first shows (p. 72).

```
#info-line.info-line          .empty without a selection, .multiple with more than one selected
  .info-empty                 "No Object Selected"
  .info-field[info-field=…]   a label and its control, a section of the set infoLine (section=…)
    textdisplay[field=name]   a MIDI part's name; an audio event's clip name, disabled
    textdisplay[field=start], [field=end], [field=length]   in the ruler's time format
    select[field=topTrack], [field=bottomTrack]             a selection range's top and bottom track
    textdisplay[field=pitch]  a note's pitch as a name (an in-place editor's notes, below)
    valuefield[field=volume]  audio events, in dB; .unavailable for parts
    button[field=mute], button[field=lock]                  toggle buttons
    valuefield[field=transpose]                             MIDI parts, -127 to 127; audio events, -24 to 24
    valuefield[field=velocity]                              MIDI parts, -127 to 127; .unavailable for others
    textdisplay[field=releaseVelocity], [field=channel]     a note's off velocity, 0-127, and channel, 1-16
    span[field=rootKeyTranspose]                            what the project root key adds, "(+2)", empty for 0
    select[field=rootKey], button[field=globalTranspose]    of MIDI parts, audio events and audio parts' events
    textdisplay[field=value], button[field=terminator]      an automation point's value and Terminator (Yes/No)
```

- **Editing** (p. 72). A double click edits a display or the volume as text, Enter takes it; mute and lock are toggles.
  The edit goes to every selected event or part, relative to its own value: a new start moves them all by as much
  as the first one moves, a new end or length resizes them all by as much (`event.move`, `event.resize` at the end).
  A drag, wheel turns or a held arrow key on the volume, transpose or velocity go from each one's value where the
  gesture started, so that one held at its limit on the way keeps its distance to the others on the way back.
  With Ctrl (Cmd) held when Enter takes the value, every one gets the value typed: the same start, end, length or
  volume, also when it is the first one's own value. Names go to every selected part, audio and MIDI alike (`event.set`), the volume to every selected audio event
  (`audioEvent.set`, -144 to 6 dB), mute and lock to all (`event.set`). Each edit is one `history.transaction`, so one
  undo restores every event; a drag, wheel turns or a held arrow key on the volume are one undo step too
  ([edit-history.md](edit-history.md), gestures).
- **Transpose and Velocity** (p. 1107): a MIDI part's transpose in semitones and velocity offset, which add to the
  track's MIDI modifiers ([midi-modifiers.md](midi-modifiers.md)). Like the volume they change every selected part by
  as much as the first one changes, kept from -127 to 127, or with Ctrl+Enter give them all the value typed
  (`midiPart.set` `transpose` and `velocity`). An audio event has a Transpose too (p. 420, #1555), from -24 to 24, the
  time stretcher's real-time range (`audioEvent.set` `transpose`), and no velocity.
- **Root Key and Global Transpose** (pp. 417, 421, [transpose.md](transpose.md)): a MIDI part's or an audio event's
  root key, from a drop-down list, and a toggle that is checked for Independent; both go to every selected part and
  audio event (`midiPart.set` and `audioEvent.set` `rootKey` and `globalTranspose`, #1368), `.unavailable` for others.
  An audio part has none of its own (p. 417): the fields show its first event's and set them on all its events
  (#1555); they are `.unavailable` for an empty one.
- **Positions.** In bars and beats the moves and resizes are in ticks, exact on musical tracks; in the other formats
  in samples. Lengths are typed as `formatLength` shows them, e.g. "2.0.0.0" for two bars.
- The themes give `.multiple` its own color, as the reference shows a multiple selection "in another color".
- **Setup pane** (p. 72, 1575). A right click on the info line opens its setup pane `#info-line-setup`, which shows,
  hides and orders its fields, the sections of the set `infoLine` ([docking.md](docking.md)), with presets, Show All
  and Reset All. A field the selection doesn't have is `.unavailable`, not `.hidden`, so it doesn't show while the
  setup pane has it shown, and a field hidden there stays hidden whatever is selected. Its buttons name the fields as
  the info line does, Description and Position with markers selected. `AppUi` hands the info line to
  `Docking::arrangeSections` once `ui::InfoLine` has filled it.
- **While hidden** it doesn't ask for the selection; it asks once it shows again.
- **Markers** (p. 430). `AppUi` gives the info line the markers selected on the marker tracks
  (`ui::MarkerLines::selection`, [markers.md](markers.md)). When that selection changed last, the info line has the
  class `markers` and shows the first selected marker in timeline order: its description (`field=name`, labelled
  Description) and position (`field=start`, labelled Position), and for a cycle marker its end and length; volume,
  mute and lock are `.unavailable`. Edits go to every selected marker as one `history.transaction` of `marker.set`s, as for
  events: relative to the first one's value, or with Ctrl+Enter the same value for all. A new position keeps a cycle
  marker's length; end and length change the cycle markers. The project window selects events or markers:
  selecting an event deselects the markers (`AppUi`, `MarkerLines::deselect`), a marker clicked deselects the events.
  A selection that names only markers the project no longer has shows `.empty`, until the marker lines drop them.
- **Arranger events** (p. 405). The arranger events selected on the arranger track's lane show the same way, with the
  class `arranger`: name (labelled Name), start, end and length, edited with `arranger.set`
  ([arranger.md](arranger.md)).
- **Automation points** (p. 881, [virgin-territory.md](virgin-territory.md)). When the event display's selected
  automation points changed last (`EventDisplay::selectedPoints`), the info line has the class `points` and shows the
  first one in time: its position (`field=start`, labelled Position), its value in the parameter's unit and Terminator,
  Yes or No; its other fields are `.unavailable`. Edits go to every selected point as one `history.transaction` of
  `automationPoint.set`s, one per lane: position and value relative to the first one's, or with Ctrl+Enter the value
  typed for all, values kept in each parameter's range (`automationLane.targets`); Terminator goes to all, which ends
  the curve at each point up to the next one, a gap. Selecting events deselects the points, and the other way round.
- **An in-place editor's notes** (p. 1259, [in-place-editor.md](in-place-editor.md)). When an in-place editor's
  selection changed last, the info line has the class `notes` and shows its first selected note as that editor's own
  info line shows it ([key-editor.md](key-editor.md)): start, end and length in bars and beats, the pitch as a name such
  as C3, the velocity 0-127, the off velocity and the channel 1-16; its other fields are `.unavailable`. It follows the
  editors' `editor.changed`, so it shows whichever editor's selection changed last, also while the editor's own toolbar,
  and with it its info line, is hidden. Edits go to every selected note as one `history.transaction` of `note.set`s per
  part (p. 1140): relative to the first note's value, or with Ctrl+Enter the value typed for every one, the channel set
  for all; positions are in ticks. The velocity field's steps work as the events' volume does: each keeps the velocity
  of every note it changes, by note id, so another note selected while the last step is out shows its own velocity and a
  dragged note the drag's, and wheel turns over the field on two selections are an undo step for each (#1550,
  [ui-engine-binding.md](ui-engine-binding.md)). An event or part selected in the project window, an empty selection in
  the editor or the editor closing gives the info line back to what was selected before.
- **The selection range** (pp. 287-288, #1910, [range-editing.md](range-editing.md)). When the event display's selection
  range changed last (`EventDisplay::range`, made with the range selection tool or, with the selection tools combined,
  the object selection tool), the info line has the class `range` and shows the range: its start, end and length in the
  ruler's time format, labelled Range Start, Range End and Range Length, and its top and bottom track, drop-down lists
  of the display's tracks; its other fields are `.unavailable`. Edits set the range (`EventDisplay::setRange`), not the
  project, so they are no undo step: Range Start moves the start and Range End the end, each at most up to the other,
  and Range Length moves the end. Top Track and Bottom Track take the tracks between the chosen one and the range's edge
  in or out, as the top and bottom nudges take them one by one, but not past the other edge. Another tool takes the
  range away and gives the info line back to the selected events.

## Inspector

`ui::Inspector` (`src/ui/inspector.h`) fills the Inspector panel of the left zone (`AppUi`, the panel `inspector`,
Show/Hide Left Zone or Mod+Alt+L, Alt+I). It shows the first selected track of the track list (pp. 76–77); without
one it has the class `empty` and says "No Track Selected".

```
.inspector                         contextmenu #inspector-setup
  textdisplay.inspector-name       the track's name: a double click renames it (track.set name)
  .inspector-sections[sections=inspector]
    [section=basics]               mute, solo, listen, record, monitor, timeBase, editInPlace (button[control]),
                                   button.inspector-retrospective (MIDI and instrument tracks, its pop-up menu
                                   #inspector-retrospective-menu); volume (dB), pan (-100 to 100)
    [section=routing]              select[control=input] (audio tracks), select[control=output]; MIDI tracks:
                                   select[control=device], textdisplay[control=bank], select[control=program];
                                   MIDI and instrument tracks: select[control=inputTransformer]
    [section=modifiers]            MIDI and instrument tracks: button[control=modifiers-bypass], valuefields and the
                                   random generators' and ranges' select[control=modifiers-random-0-target] etc.
    [section=midiInserts]          MIDI and instrument tracks: button[control=midiInserts-bypass],
                                   select[control=midiInserts-divider], .midi-divider[gap=0..4] (.at where the
                                   divider is) with .midi-divider-line, .midi-effect-slot[slot=0..3] (.empty, .off,
                                   .recorded above the divider, .open with its control panel): button[control=
                                   midiInserts-0-on], select[control=midiInserts-0-effect], button.midi-effect-edit,
                                   .midi-effect-panel with [control=midiInserts-0-parameter-rate] etc.
    [section=midiSends]            the same with midiSends-, button[control=midiSends-pre] and per slot
                                   select[control=midiSends-0-output]
    [section=noteExpression]       MIDI and instrument tracks: button[control=noteExpression-bypass], a row per
                                   expression [expression=0..6] (.selected) with its .expression-in,
                                   select[control=noteExpression-input], button[control=noteExpression-learn],
                                   valuefield[control=noteExpression-max] and [control=noteExpression-min]
    [section=chords]               MIDI and instrument tracks with a chord track: select[control=followChords]
    [section=articulations]        MIDI and instrument tracks: select[control=articulationMap],
                                   button.inspector-articulation-setup
    [section=trackVersions]        audio, instrument and MIDI tracks: textdisplay[control=versionName],
                                   .inspector-version-id, .inspector-version-indicator (.multiple on the section),
                                   .inspector-version[version] per version (.selected on the active one), and
                                   button.inspector-version-add, -duplicate and -remove
    [section=inserts]              .insert per plugin slot, .bypassed and .inactive
    [section=notepad]              textarea.notepad
```

- **Sections** (pp. 79, 168–170). Each section has a `.section-header`; a click on it closes or opens the section
  (`.collapsed`, view state), except an Alt-click on the Basics header, which opens the Choose Track Color pane
  ([colors.md](colors.md)). What a track doesn't have gets the class `unavailable` and doesn't show: the sections of
  tracks without a channel (marker and ruler tracks, folders without a group channel), record, monitor and input on
  tracks other than audio tracks, inserts, volume and pan on MIDI tracks, which have no audio.
- **Basics** (pp. 170–182): the channel's switches, fader and panner, sent with `channel.set` (`mute`, `solo`,
  `listen`, `recordEnable`, `monitor`, `volume`, `pan`; S and L as in the mixer,
  [solo-mute-listen.md](solo-mute-listen.md)), and Switch Time Base (pp. 179–181, 229) on audio, instrument, MIDI,
  group, effect and VCA fader tracks, checked when linear (`track.set` `timeBase`, [time-base.md](time-base.md)).
  Edit In-Place (p. 1259) on MIDI and instrument tracks opens and closes the track's in-place editor, checked while it
  is open (`editor.inPlace`, [in-place-editor.md](in-place-editor.md)). Retrospective Recording on MIDI and instrument
  tracks opens a pop-up menu (pp. 354-356): Insert as Linear Recording and Insert as Cycle Recording
  (`midi.insertRetrospective`, `cycle`) and Empty Retrospective Record Buffer (`midi.clearRetrospective`) for the
  track ([midi-recording.md](midi-recording.md)).
  The volume goes up to the project's `maxVolume` (`project.set`). A drag, wheel turns or a held arrow key on the
  volume or the pan are one undo step ([edit-history.md](edit-history.md), gestures), sent step by step so that they
  are heard, one at a time; while they are on their way the fields keep their own values rather than the engine's
  older ones (#716, [ui-engine-binding.md](ui-engine-binding.md)).
- **Refused changes.** When the engine refuses a change, such as an output that would make a routing loop, the
  inspector shows the channel as it is again.
- **Routing** (p. 168): the input bus of an audio track, and where the channel plays: an output bus or a group channel,
  for MIDI tracks an instrument track's channel, as in the add track dialog (`channel.set` `input` and `output`). On
  MIDI tracks also the MIDI channel their patch, MIDI volume and pan go out on (Channel: Any or 1 to 16, p. 349,
  `channel.set` `midiChannel`, [mixer.md](mixer.md)) and the MIDI device, bank and program (pp. 1108, 1113,
  [midi-devices.md](midi-devices.md)). On MIDI and instrument tracks the Input Transformer (pp. 1097, 1393,
  [midi-rules.md](midi-rules.md)): Off, Project or Track (`channel.set` `inputTransformer` `mode`) and, while it isn't
  off, Open Panel..., which opens the project input transformer or the track's and leaves the mode as it was.
- **MIDI Modifiers** (pp. 1099–1102, [midi-modifiers.md](midi-modifiers.md)): Bypass, Transpose, Velocity Shift,
  Velocity and Length Compression as numerator and denominator, Random 1 and 2 with their target and minimum and
  maximum, and Range 1 and 2 with their mode and minimum and maximum, sent with `channel.set` `modifiers`. Each control
  is named after what it changes: `modifiers-` and the key, then the index of a pair and the field of a generator or
  range (`modifiers-velocityCompression-1`, `modifiers-range-0-minimum`). Random positions and lengths show in ticks of
  480 to a quarter note, up to 480 either way; the other values as `channel.set` takes them. A drag, wheel turns or a
  held arrow key on a value are one undo step, and a value the engine refuses, such as a minimum above the maximum,
  shows as it is again.
- **MIDI Inserts and MIDI Sends** (pp. 1104-1106, [midi-effects.md](midi-effects.md)): Bypass, the inserts' divider
  (the slots above it are recorded, marked at their side; a line between the slots that is dragged to another place,
  p. 1105, or chosen from a drop-down list; a drag ends without moving it when the inspector shows another track
  before it ends, also one without a channel), switched-off slots dimmed, and the sends' Pre/Post MIDI Modifiers and
  Inserts, and per slot Activate, Select
  Effect Type (No Effect and the effects `midiEffect.list` lists), the sends' output (Track Output or another
  instrument track) and the edit button, which opens and closes the effect's control panel below the slot: a
  drop-down list for each parameter with options, a value field for the others, sent with `channel.set` `midiInserts`
  and `midiSends`. An effect chosen is on and its control panel opens (p. 1105). A drag, wheel turns or a held arrow
  key on a value are one undo step.
- **Note Expression** (pp. 1300-1304, [note-expression-setup.md](note-expression-setup.md)): Bypass, the expressions
  with their inputs' short names in the In column, and the selected one's Parameter Details: Input, MIDI Learn and its
  Max. and Min. Value, sent with `noteExpression.set`.
- **Chords** (pp. 1338-1340, [chord-track.md](chord-track.md)): MIDI and instrument tracks' Follow Chord Track
  (`chord.follow` `mode`), with Single Voice its voice and master, This Track's Voicings and Live Input, while the
  project has a chord track.
- **Chord Track** (pp. 1329, 1333-1335): the chord track's Adaptive Voicings, Automatic Scales, Acoustic Feedback,
  voicing library and subset, Octave Offset, the piano's Custom Voicing options and range, and Shift Chords
  (`chord.setTrack`).
- **Arranger Track** (pp. 405, 409-411, [arranger.md](arranger.md)): the arranger track's Open Arranger Editor beside
  the header, the Current Arranger Chain and Arranger Events lists, whose arrows play a step and trigger an event, with
  the one playing highlighted, Jump Mode, renaming a step's event with Alt and a double click on its name, a step's
  repeats (a double click types them) and repeat mode, and Remove Touched on a step's context menu.
- **Articulation Map** (pp. 1263, 1286, [articulation-maps.md](articulation-maps.md)): MIDI and instrument tracks'
  map, or No Map (`track.set` `articulationMap`), and Setup..., which opens the articulation map setup for the track.
- **Track Versions** (pp. 231-232, [track-versions.md](track-versions.md)): the first selected track's active
  version, its name, which a double click changes for every selected track that has versions (`trackVersion.set`), and
  its ID, an indicator while the track has more than one version, the list of them all, in which a click activates one
  on those tracks (`trackVersion.activate`), and New, Duplicate and Delete (`trackVersion.add`, `.duplicate` and
  `.remove`), which work on them too; the engine refuses a command that names a track without versions, so the others
  in the selection are left out. Delete only while there is a version to delete.
- **Inserts**: an overview of the channel's plugin slots by name, top to bottom (`plugin.*` changes them).
- **Notepad** (p. 168): the channel's notes. What is typed goes to the engine when the field loses the focus, also
  with Escape (`channel.set` `notes`, one undo step). When another track shows while the notepad or the name is being
  edited, the edit is taken first, for the track it started on. When another client replaces the project
  (`project.reset`) the edits in progress are dropped, in the info line too, since the new project's tracks, channels
  and events may have the same ids. The notepad and the name keep their arrow keys, also at the first and last line
  ([key-commands.md](key-commands.md)), so that they don't select another track. The mixer's notepad (p. 497,
  [mixer-window.md](mixer-window.md)) shows the same notes.
- **Set up Sections** (p. 167): a right click on an empty area of the inspector (not on a control, the inserts or the
  notepad, which have an empty `contextmenu`) opens the setup pane `#inspector-setup`, which the
  docking system fills with a toggle per section: it shows and hides sections, and dragging one onto another reorders
  them, with Show All, Reset All and presets. The choice is the user setting `ui.layout`, under the set `inspector`,
  the presets `ui.setupPresets` ([docking.md](docking.md)).

## In the engine

- **Notes.** `channel.set` `notes` is a channel's notepad text; `project.get` has it with the channel. In DAWproject it
  is the channel's `comment` attribute, so it comes back from `project.xml` without GNUbase's entry too.

## Not yet

The info line's other fields (offset, fades, snap point; audio events' global transpose and root key), hiding fields
from its context menu, and its own modifier setting (Preferences, Editing - Tool Modifiers); the inspector's other
sections (chords, articulations, equalizer, fader, cue sends, remote controls), sends (#108), the MIDI tracks' MIDI
volume and pan, the folder track inspector's list of the tracks inside, the editor tab and the tempo, signature and
marker track inspectors, Expand Sections Exclusively, and the Channel zone's channel strip (#107). Record and monitor on
instrument and MIDI tracks (pp. 173–176) wait for #97. Set up Sections for the inspector in a window of its own, where
`#inspector-setup`, which is in the project window's document, doesn't open, waits for #517.

## Checks

- `tests/document_tests.cpp`: notes on audio and MIDI channels, with undo, and a value that isn't text refused.
- `tests/dawproject_tests.cpp`: notes saved and loaded, also from `project.xml` alone.
- `tests/inspector_tests.cpp` drives the app's project window with `ui::Automation`: the info line of one and of two
  selected parts, start, length (relative and with Ctrl+Enter absolute, also with the first part's own value, after
  which an edit taken by leaving the field is relative again) and end edits with undo, name, mute and lock for every
  selected part, changes from another client, an audio event's volume, an untouched Ctrl+Enter on it giving three events
  the first one's in one undo step, the parts' transpose and velocity (relative, kept in range, with Ctrl+Enter, also on
  the first part's own value, undo, a drag that holds one at its limit and comes back, hidden for audio events), and asking only while it shows; a marker selected on the marker track with each field
  edited, two markers changed relatively and with Ctrl+Enter in one undo step, and an event selected after it, the setup
  pane naming the fields as the info line does; a marker clicked again after a part, Ctrl+Enter on the first marker's
  own position with a part shown before, a selection naming removed markers, and an untouched Ctrl+Enter on the position
  of three markers giving them the first one's in one undo step; a note selected in an in-place editor with its fields
  shown and edited, pitch relative and with Ctrl+Enter, velocity, channel and start, a part selected after it, its
  editor deselecting and closing; two automation points selected with their position, value and Terminator shown,
  position and value relative and value with Ctrl+Enter kept in range, Terminator set on one point in one undo step and
  cleared again, and an event and a marker selected after them showing their Name field; the inspector's buttons, volume
  and pan both ways, the output from another client and from the drop-down list, a refused output, an output list
  without the channels that would close a loop, the inserts overview with a bypassed slot, the notepad, renaming, notes
  and a name being typed when another track is selected and when another client loads a project, arrow keys in the
  notepad and the name, closing a section, hiding one in Set up Sections and opening it only from an empty area, and
  what a group and a marker track don't have; the info line's and the toolbar's setup panes, a hidden field staying
  hidden when the selection shows again and Show All leaving an unavailable one out; the MIDI Modifiers section's
  bypass, values, a fraction's denominator, a generator's target with positions in ticks, a range the engine refuses,
  changes from another client with undo, and an audio track without it; the MIDI Inserts and MIDI Sends sections'
  effect, a parameter from a drop-down list and from a value field, on, bypass, the divider from the list and dragged in
  one step, a drag dropped when the inspector shows another instrument track or a marker track, the control panel opened
  and closed, a send's output and Pre, changes from another client with undo, and an audio track without them; the Note
Expression section's bypass, an expression's input chosen and its range typed, its In column, and MIDI Learn taking a
controller from a MIDI input, not from one outside all MIDI inputs, and ending when another track shows, with undo. A drag of
  the inspector's volume up to the project's maximum, wheel turns on the pan and a drag of the info line's volume are
  one undo step each, and a MIDI modifier and a part's transpose dragged back to where they started while the engine
  answers late end there. The info line's volume step that the engine refuses, one of the two selected events being
  gone, shows in the command log, leaves no undo step and shows the engine's value again. While a dragged note's last
  velocity step is out, another note selected in an in-place editor shows its own velocity and the dragged one the
  drag's; wheel turns on the velocity of one note, then of the next with the mouse still over the field, are two undo
  steps.
- `tests/rangeinfoline_ui_tests.cpp`: the info line of a selection range, its labels and fields and the others
  unavailable; Range Start, Range Length and Range End setting the range with no undo step, a start past the end and an
  end before the start changing nothing; Top Track and Bottom Track taking tracks in and out, not past the other edge;
  and another tool giving the info line back to the selected part with its own labels.
- `tests/mixerwindow_tests.cpp`: a drag of a mixer fader is one undo step.
