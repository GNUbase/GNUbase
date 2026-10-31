# Add track dialog

Adding tracks (#80, reference manual pp. 139–161, 197–198): the add track dialog, with a page for each track type,
and a double click below the tracks. Both add tracks with the engine's `track.add`, so MCP and Lua scripts make
exactly the same tracks.

## The dialog

`resources/ui/add-track.rml` is a `<dialog>` from the widget library ([widgets.md](widgets.md)) whose Lua
([ui-lua.md](ui-lua.md)) reads what it offers from the engine and adds the tracks through engine commands, like the
studio setup dialog ([studio-setup.md](studio-setup.md)).

- **Opening it.** The UI command `project.addTrack` opens it, modal, in the window it runs in. The project window's
  toolbar has an Add Track button (`#add-track`) that runs it, until the track list's global track controls take it
  over (#81); the main menu's Project > Add Track will run it too.
- **Pages.** The list on the left selects a track type, as the reference's dialog does; each page has the reference's
  settings for it that GNUbase has so far:

| Page | Settings | `track.add` parameters |
|---|---|---|
| Audio | Audio Inputs, Configuration, Output Routing, Name, Count | `input`, `format`, `output`, `name` |
| Instrument | Instrument (the built-in instruments of `processor.list`), Output Routing, Name, Count | `instrument`, `output`, `name` |
| MIDI | Output Routing (the instrument tracks' channels), Name, Count | `output`, `name` |
| Group, Effect | Configuration, Output Routing, Name, Count | `format`, `output`, `name` |
| Folder | Group Channel, Configuration and Output Routing (for the group channel only), Name, Count | `groupChannel`, `format`, `output`, `name` |
| Marker | Name, Count | `name` |
| Ruler | Time Format, Count | `timeFormat` |

- **Choices** come from the engine whenever a page shows: the input busses (and No Bus), the output busses and the
  group channels of group tracks and folders (and No Bus), the instrument channels for MIDI tracks (and Not
  Connected), the formats and time formats of `track.add`'s schema (`engine.commands`) and the instruments of
  `processor.list`. Each starts at the engine's default: the first input bus, the main output, stereo, no instrument,
  bars and beats. An empty name takes the engine's default name ("Audio 01", "Audio 02" and so on).
- **Add Track** adds Count tracks (1 to 100, as in the reference) with the page's settings, all with the same
  parameters, in one `history.transaction` named "Add Track", so one undo removes them. New tracks go to the end of
  the project; below the selected track comes with track selection (#81). The dialog closes, unless Keep Dialog Open
  is checked: then it shows the page again, with the new group and instrument channels among the outputs, for adding
  more of another type. A track the engine refuses, such as an eleventh marker track, adds nothing, and the dialog
  stays open with the engine's message. Cancel and Escape close it.

## Double click

A double click in the event display below the last track adds a MIDI track, as a double click in the empty part of
the reference's track list does without a selected track (p. 198). Its other case, a track like the selected one
with its input and output, comes with track selection and the track list (#81), which gets the same double click.

## In the engine

- **`track.add`** takes `input`, `output` and `instrument` and checks them as `channel.set` does (one function,
  `route`, checks both), with the new track in place, so a track that can't be routed so isn't added. They need a
  channel: marker and ruler tracks and folders without a group channel refuse them.
- **Ruler tracks** (`type` `ruler`, p. 157) show a ruler in a time format of their own: `timeFormat`, one of
  `barsBeats` (the default), `seconds`, `timecode`, `samples` and `userFrames`, the event display ruler's formats
  ([event-display.md](event-display.md)). They have no channel, may go into folders, and only ruler tracks have a
  `timeFormat`, in `project.get` too; `track.set` changes it. DAWproject has no ruler tracks: only GNUbase's entry
  keeps them, and a project read from `project.xml` has none.

## Not yet

The track color (the default color until the color picker joins the dialog), the reference's other track types
(sampler, drum, VCA, and the tempo, signature, arranger, transpose, chord and video tracks with their tickets), an
instrument or effect plugin chosen on the page (M05), MIDI inputs and MIDI 2.0 output groups, the event type
(pattern events), the project's Group, FX and VCA folders, a ruler track's time offset and the ruler track's own
display and controls (#81), and track presets.

## Checks

- `tests/addtrack_tests.cpp` opens the dialog with the toolbar button and through its UI command and drives it with
  `ui::Automation`: two mono audio tracks with a name, and with Keep Dialog Open a group, a folder with a group
  channel, an instrument track with a built-in instrument, three MIDI tracks into it, an effect track into the group,
  a named marker track and a timecode ruler track make the same tracks and channels as the same `track_add` calls
  through MCP, and the three MIDI tracks go with one undo; an eleventh marker track shows the engine's message and
  keeps the dialog open.
- `tests/eventediting_tests.cpp` double-clicks below the tracks.
- `tests/document_tests.cpp` checks `track.add`'s routing and what it refuses, and ruler tracks;
  `tests/dawproject_tests.cpp` saves and loads a ruler track.
