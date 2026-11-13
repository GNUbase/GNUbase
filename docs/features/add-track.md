# Add track dialog

Adding tracks (#80, reference manual pp. 139–161, 197–198): the add track dialog, with a page for each track type,
and a double click below the tracks. Both add tracks with the engine's `track.add`, so MCP and Lua scripts make
exactly the same tracks.

## The dialog

`resources/ui/add-track.rml` is a `<dialog>` from the widget library ([widgets.md](widgets.md)) whose Lua
([ui-lua.md](ui-lua.md)) reads what it offers from the engine and adds the tracks through engine commands, like the
studio setup dialog ([studio-setup.md](studio-setup.md)).

- **Opening it.** The UI command `project.addTrack` opens it, modal, in the window it runs in. The track list's
  global track controls have an Add Track button (`#add-track`) that runs it ([track-list.md](track-list.md)), and so
  do the main menu's Project > Add Track and the track list's context menu.
- **Pages.** The list on the left selects a track type, as the reference's dialog does; each page has the reference's
  settings for it that GNUbase has so far:

| Page | Settings | `track.add` parameters |
|---|---|---|
| Audio | Audio Inputs, Configuration, Output Routing, Name, Count | `input`, `format`, `output`, `name` |
| Instrument | Instrument (the built-in instruments of `processor.list`), Output Routing, Name, Count | `instrument`, `output`, `name` |
| Sampler | Output Routing, Name, Count (p. 144): sampler tracks, added with `sampler.create` instead of `track.add` | `output`, `name` |
| MIDI | Output Routing (the instrument tracks' channels), Name, Count | `output`, `name` |
| Group, Effect | Configuration, Output Routing, Add to Project 'Group' / 'FX' Folder, Name, Count | `format`, `output`, `projectFolder`, `name` |
| Folder | Group Channel, Configuration and Output Routing (for the group channel only), Name, Count | `groupChannel`, `format`, `output`, `name` |
| Marker | Name, Count | `name` |
| Ruler | Time Format, Time Offset, Count | `timeFormat`, `timeOffset` and `timeOffsetOn` ([ruler-tracks.md](ruler-tracks.md)) |

- **Choices** come from the engine whenever a page shows: the input busses (and No Bus), the output busses, the
  group channels of group tracks and folders and the effect channels (and No Bus), the instrument channels for MIDI tracks (and Not
  Connected), the formats and time formats of `track.add`'s schema (`engine.commands`) and the instruments of
  `processor.list`. Each starts at the engine's default: the first input bus, the main output, stereo, no instrument,
  bars and beats. An empty name takes the engine's default name ("Audio 01", "Audio 02" and so on).
- **Add Track** adds Count tracks (1 to 100, as in the reference) with the page's settings, all with the same
  parameters, in one `history.transaction` named "Add Track", so one undo removes them. New tracks go below the selected
  track (p. 198), the lowest one if several are, in its folder and in the order they were added, as `track.add`'s
  `parent` and `index`: the UI command passes it as the document attribute `below`. Tracks that stay at the top level
  (marker, tempo, signature, arranger, transpose and chord tracks) go below the selected track's top-level folder, and
  tracks for the project's Group or FX folder into that folder. Without a selected track they go to the end of the
  project. The dialog closes, unless Keep Dialog Open is checked: then it shows the page again, with the new group and
  instrument channels among the outputs, for adding more of another type, below the tracks it just added; tracks that
  went into the project's Group or FX folder leave `below` where it was, so the next tracks go below the selection, not
  into that folder. A track the engine refuses, such as an eleventh marker track, adds nothing, and the dialog stays
  open with the engine's message. Cancel and Escape close it.

- **For selected tracks.** Group Track to Selected Tracks and Effect Track to Selected Tracks in the track list's
  context menu open the dialog on the group or effect page with the document attributes `type` and `tracks` (the
  selected tracks' ids); Add Track then passes them as `track.add` `tracks`, so they play or send into the new track
  ([routing.md](routing.md)). With a Count above 1 they go to the first new track only, for group and effect tracks
  alike: each new group would take them from the one before, and each new effect track would take another send of
  every selected track. Keep Dialog Open's next Add Track adds tracks without them. Add Effect Track to Send, in the
  mixer's and the channel settings window's send destination lists ([routing.md](routing.md)), opens the effect page
  the same way, with the track and the send (`tracks`, `send` and `below` attributes): the new effect track goes
  below the track whose send it is.

## Double click

A double click in the empty part of the track list, below the last track, or in the event display below the last
track adds a track, as a double click in the empty part of the reference's track list does (p. 198):

- With a selected audio or MIDI track (the lowest one if several are), a track of the same type with the same input
  and output (an audio track's input bus and output, a MIDI track's MIDI input, all or none, and output), below it
  in its folder.
- With another selected track, a MIDI track below it.
- Without a selected track, a MIDI track at the end of the project.

`ui::TrackList` adds it (`EventDisplay::onAddTrack` hands it the event display's double click), with one
`track.add`; a selected MIDI track whose MIDI input is none takes a `channel.set` for the new channel after it,
which `track.add` doesn't take, in the same undo step (both go as `history.transaction`s whose merge key joins
them).

## In the engine

- **`track.add`** takes `input`, `output` and `instrument` and checks them as `channel.set` does (one function,
  `route`, checks both), with the new track in place, so a track that can't be routed so isn't added. They need a
  channel: marker and ruler tracks and folders without a group channel refuse them.
- **Ruler tracks** (`type` `ruler`, p. 157) show a ruler in a time format of their own: `timeFormat`, one of
  `barsBeats` (the default), `seconds`, `timecode`, `samples` and `userFrames`, the event display ruler's formats
  ([event-display.md](event-display.md)). They have no channel, may go into folders, and only ruler tracks have a
  `timeFormat`, in `project.get` too; `track.set` changes it. Their time offset and their display are in
  [ruler-tracks.md](ruler-tracks.md). DAWproject has no ruler tracks: only GNUbase's entry
  keeps them, and a project read from `project.xml` has none.
- **The tempo track** (`type` `tempo`, p. 162) has a page without settings: a project has one at most
  ([tempo-track.md](tempo-track.md)). So has **the signature track** (`type` `signature`, p. 163,
  [signature-track.md](signature-track.md)).
- **VCA tracks** (`type` `vca`, p. 154) have the name and the count ([link-groups.md](link-groups.md)).

## Not yet

The track color (the default color until the color picker joins the dialog), the reference's other track types
(sampler, drum, and the video track with their tickets; the arranger, transpose and chord tracks are there, once
per project, [arranger.md](arranger.md), [transpose.md](transpose.md), [chord-track.md](chord-track.md)), an
instrument or effect plugin chosen on the page (M05), MIDI inputs and MIDI 2.0 output groups, the event type
(pattern events), the project's VCA folder, and track presets.

## Checks

- `tests/addtrack_tests.cpp` opens the dialog with the toolbar button and through its UI command and drives it with
  `ui::Automation`: two mono audio tracks with a name, and with Keep Dialog Open a group, a folder with a group
  channel, an instrument track with a built-in instrument, three MIDI tracks into it, an effect track into the group,
  a named marker track and a timecode ruler track with a time offset make the same tracks and channels as the same `track_add` calls
  through MCP, and the three MIDI tracks go with one undo; an eleventh marker track shows the engine's message and
  keeps the dialog open. Group and Effect Track to Selected Tracks route the selected tracks into the new track;
  Group Track to Selected Tracks with a Count of 2 into the first one only, and not again with Keep Dialog Open.
- `tests/addtrack_tests.cpp` also adds tracks below a selected track in a folder with the dialog (in order, with
  Keep Dialog Open below those, and a marker track below the folder), with Keep Dialog Open below the selection
  after an effect track into the project's FX folder, and double-clicks in the track list and the
  event display with a selected audio track, a selected MIDI track (its MIDI input, in one undo step) and none.
- `tests/channelsettings_tests.cpp` adds an effect track to a send below the track whose send it is.
- `tests/eventediting_tests.cpp` double-clicks below the tracks of an event display without a track list.
- `tests/document_tests.cpp` checks `track.add`'s routing and what it refuses, and ruler tracks;
  `tests/dawproject_tests.cpp` saves and loads a ruler track.
