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
  .info-field[info-field=…]   a label and its control
    textdisplay[field=name]   a MIDI part's name; an audio event's clip name, disabled
    textdisplay[field=start], [field=end], [field=length]   in the ruler's time format
    valuefield[field=volume]  audio events, in dB; .hidden for parts
    button[field=mute], button[field=lock]                  toggle buttons
```

- **Editing** (p. 72). A double click edits a display or the volume as text, Enter takes it; mute and lock are toggles.
  The edit goes to every selected event or part, relative to its own value: a new start moves them all by as much
  as the first one moves, a new end or length resizes them all by as much (`event.move`, `event.resize` at the end).
  With Ctrl (Cmd) held when Enter takes the value, every one gets the value typed: the same start, end, length or
  volume, also when it is the first one's own value. Names go to every selected part (`midiPart.set`), the volume to every selected audio event
  (`audioEvent.set`, -144 to 6 dB), mute and lock to all (`event.set`). Each edit is one `history.transaction`, so one
  undo restores every event; a drag, wheel turns or a held arrow key on the volume are one undo step too
  ([edit-history.md](edit-history.md), gestures).
- **Positions.** In bars and beats the moves and resizes are in ticks, exact on musical tracks; in the other formats
  in samples. Lengths are typed as `formatLength` shows them, e.g. "2.0.0.0" for two bars.
- The themes give `.multiple` its own color, as the reference shows a multiple selection "in another color".
- **While hidden** it doesn't ask for the selection; it asks once it shows again.
- **Markers** (p. 430). `AppUi` gives the info line the markers selected on the marker tracks
  (`ui::MarkerLines::selection`, [markers.md](markers.md)). When that selection changed last, the info line has the
  class `markers` and shows the first selected marker in timeline order: its description (`field=name`, labelled
  Description) and position (`field=start`, labelled Position), and for a cycle marker its end and length; volume,
  mute and lock are `.hidden`. Edits go to every selected marker as one `history.transaction` of `marker.set`s, as for
  events: relative to the first one's value, or with Ctrl+Enter the same value for all. A new position keeps a cycle
  marker's length; end and length change the cycle markers. The project window selects events or markers:
  selecting an event deselects the markers (`AppUi`, `MarkerLines::deselect`), a marker clicked deselects the events.
  A selection that names only markers the project no longer has shows `.empty`, until the marker lines drop them.

## Inspector

`ui::Inspector` (`src/ui/inspector.h`) fills the Inspector panel of the left zone (`AppUi`, the panel `inspector`,
Show/Hide Left Zone or Mod+Alt+L, Alt+I). It shows the first selected track of the track list (pp. 76–77); without
one it has the class `empty` and says "No Track Selected".

```
.inspector                         contextmenu #inspector-setup
  textdisplay.inspector-name       the track's name: a double click renames it (track.set name)
  .inspector-sections[sections=inspector]
    [section=basics]               mute, solo, record, monitor, timeBase (button[control]); volume (dB), pan
                                   (-100 to 100)
    [section=routing]              select[control=input] (audio tracks), select[control=output]; MIDI tracks:
                                   select[control=device], textdisplay[control=bank], select[control=program]
    [section=inserts]              .insert per plugin slot, .bypassed and .inactive
    [section=notepad]              textarea.notepad
```

- **Sections** (pp. 79, 168–170). Each section has a `.section-header`; a click on it closes or opens the section
  (`.collapsed`, view state). What a track doesn't have gets the class `unavailable` and doesn't show: the sections of
  tracks without a channel (marker and ruler tracks, folders without a group channel), record, monitor and input on
  tracks other than audio tracks, inserts, volume and pan on MIDI tracks, which have no audio.
- **Basics** (pp. 170–182): the channel's switches, fader and panner, sent with `channel.set` (`mute`, `solo`,
  `recordEnable`, `monitor`, `volume`, `pan`), and Switch Time Base (pp. 179–181, 229) on audio, instrument, MIDI,
  group, effect and VCA fader tracks, checked when linear (`track.set` `timeBase`, [time-base.md](time-base.md)). The volume goes up to the project's
  `maxVolume` (`project.set`). A drag, wheel turns or a held arrow key on the volume or the pan are one undo step
  ([edit-history.md](edit-history.md), gestures), sent step by step so that they are heard.
- **Refused changes.** When the engine refuses a change, such as an output that would make a routing loop, the
  inspector shows the channel as it is again.
- **Routing** (p. 168): the input bus of an audio track, and where the channel plays: an output bus or a group
  channel, for MIDI tracks an instrument track's channel, as in the add track dialog (`channel.set` `input` and
  `output`). On MIDI tracks also the MIDI device, bank and program (pp. 1108, 1113, [midi-devices.md](midi-devices.md)).
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
  them. The choice is the user setting `ui.layout`, under the set `inspector` ([docking.md](docking.md)).

## In the engine

- **Notes.** `channel.set` `notes` is a channel's notepad text; `project.get` has it with the channel. In DAWproject it
  is the channel's `comment` attribute, so it comes back from `project.xml` without GNUbase's entry too.

## Not yet

The info line's other fields (offset, fades, snap point, transpose and velocity of parts, which `midiPart.set` changes: [midi-modifiers.md](midi-modifiers.md)), hiding fields from its
context menu, and its own modifier setting (Preferences, Editing - Tool Modifiers); the inspector's other sections
(track versions, chords, articulations, MIDI inserts and modifiers, equalizer, fader, cue sends, remote controls),
sends (#108), the MIDI tracks' MIDI volume and pan, the folder track inspector's list of the tracks inside, the
editor tab and the tempo, signature and marker track inspectors, Show All, Reset All and presets in Set up Sections,
Expand Sections Exclusively, and the Channel zone's channel strip (#107). Record and monitor on instrument and MIDI
tracks (pp. 173–176) wait for #97. Set up Sections for the inspector in a window of its own, where `#inspector-setup`,
which is in the project window's document, doesn't open, waits for #517.

## Checks

- `tests/document_tests.cpp`: notes on audio and MIDI channels, with undo, and a value that isn't text refused.
- `tests/dawproject_tests.cpp`: notes saved and loaded, also from `project.xml` alone.
- `tests/inspector_tests.cpp` drives the app's project window with `ui::Automation`: the info line of one and of two
  selected parts, start, length (relative and with Ctrl+Enter absolute, also with the first part's own value, after
  which an edit taken by leaving the field is relative again) and end edits with undo, name, mute and lock
  for every selected part, changes from another client, an audio event's volume, and asking only while it shows; a
  marker selected on the marker track with each field edited, two markers changed relatively and with Ctrl+Enter in
  one undo step, and an event selected after it; a marker clicked again after a part, Ctrl+Enter on the first marker's
  own position with a part shown before, and a selection naming removed markers; the inspector's buttons, volume and
  pan both ways, the output from another client and from the drop-down list, a refused output, the inserts overview
  with a bypassed slot, the notepad, renaming, notes and a name being typed when another track is selected and when
  another client loads a project, arrow keys in the notepad and the name, closing a section, hiding one in Set up
  Sections and opening it only from an empty area, and what a group and a marker track don't have. A drag of the
  inspector's volume up to the project's maximum, wheel turns on the pan and a drag of the info line's volume are one
  undo step each.
- `tests/mixerwindow_tests.cpp`: a drag of a mixer fader is one undo step.
