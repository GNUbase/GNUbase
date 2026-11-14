# Tempo track

The tempo track, its tempo events and ramps, the project tempo modes and tap tempo (#155, reference manual pp. 162,
1446-1458). The tempo map itself, which converts between ticks and samples, is the time model's (#13,
`engine/time/tempomap.h`).

## Engine

- **Tempo modes** (p. 1446). A project has a fixed tempo (fixed tempo mode, a new project's) or follows the tempo
  track (tempo track mode): `project.set` `tempoTrackActive`. `Project::tempo` is always the tempo the project plays,
  which every conversion, the render snapshot and the transport use; `Project::otherTempo` keeps the tempo of the mode
  not in use: the tempo track's events in fixed tempo mode, the fixed tempo (one point) in tempo track mode. Switching
  swaps them, so each mode keeps its tempo while the other is in use. `Project::tempoTrack()` and `fixedTempo()` read
  either. `project.set` `tempo` sets the first point of the tempo the project plays: the fixed tempo, or the tempo
  track's first event.
- **Tempo events** are the tempo track's points (`time::TempoPoint`): a position in ticks, a tempo of 1 to 999 BPM and
  whether the tempo ramps to it from the event before (linearly per beat) or jumps there. The first is at the project
  start and stays. `tempo.add` adds one at a position (ticks or `"cursor"`, Insert Tempo Event at Cursor) or changes
  the one there; without `ramp` the user setting `editing.tempoType` decides: `ramp`, `step`, or `auto`, the type of
  the event before it (Type of New Tempo Points, p. 1448). `tempo.set` changes events named by their positions, all at
  once (to, bpm, ramp); `tempo.remove` removes them. They edit the tempo track in either mode, and each is an undo step
  that emits `tempo.changed` with the tempo the project plays, the mode, the tempo track's events and the fixed tempo
  (the same keys as `project.get`).
- **Musical and linear time** (the acceptance of #155). Events, parts, markers and automation on musical tracks are in
  ticks, so a tempo change moves them in time; linear ones are in samples and stay, with the notes in linear MIDI parts
  ([time-base.md](time-base.md)). Audio events keep their length in samples. Cycle markers that a faster tempo makes
  shorter than a sample get longer (`tempoChanged`).
- **Tap tempo** (pp. 1455-1458): `tempo.tap` takes the tapped tempo and applies it by the Tap Tempo Mode, the user
  setting `transport.tapTempoMode` unless `mode` is given: `selection` adds a tempo event at `selection` (the start of
  the selected events or range, which only the client knows), `playbackStart` at the last playback start position
  (`transport.get` `start`), `cursor` at the cursor; `project` sets the project tempo (the first tempo event in tempo
  track mode, the fixed tempo otherwise); `display` changes nothing; `auto` sets the project tempo in fixed tempo mode,
  else adds an event at the selection, at the playback start while playing, or at the cursor.
- **The tempo track** is a track type (`tempo`): a project has one at most, without a channel, at the top level. It
  only shows the tempo, which is the project's: removing it keeps the tempo, and its events wait in the project.
- **Files**: GNUbase's entry keeps the mode and both tempos (`tempoTrackActive`, `tempoTrack`, `fixedTempo`), and the
  tempo track as a track. `project.xml` has the tempo the project plays, so other applications play it the same; a
  project read from `project.xml` alone follows its tempo changes if it has any. Older GNUbase files without the mode
  do too. A MIDI file's tempo changes go on the tempo track and make it active.
- **Tempo track files** (#160, p. 1462): `tempo.export` writes the tempo track's events, also in fixed tempo mode,
  and the time signature events into an XML file, which `tempo.import` reads into another project: its events replace
  the tempo track's and the time signature events, as one undo step. A file with tempo changes makes the tempo track
  active, as a MIDI file's does; the fixed tempo stays. The file says what `project.xml` says about them, in the same
  DAWproject elements (`src/engine/dawproject/dawproject.h`, `saveTempoTrack` and `loadTempoTrack`): a `Transport`
  with the first tempo and signature and an `Arrangement` with the `TempoAutomation` and `TimeSignatureAutomation`,
  under a `TempoTrack` element instead of a `Project`. So both share one writer and reader, and the tempo track
  survives the round trip exactly: positions are whole ticks as beats, tempos written at full precision. The file
  ends in `.xml`: the reference's own tempo track files have a format of their own, which GNUbase doesn't read.

## UI

- **The tempo track's lane** (`ui::TempoLane`, `src/ui/tempotrack.h`): a `ui::TempoCurve` over the tempo track's lane
  in the event display, following the project and the view: the tempo track's events as a `<curve class="tempo-line">`
  on the timeline of the tempo the project plays (ramps as straight pieces, jumps as steps) and a `.tempo-point` per
  event, `.inactive` in fixed tempo mode. In fixed tempo mode `<curve class="tempo-line fixed-tempo">` draws the fixed
  tempo as a horizontal line over the grayed-out events (pp. 1447, 1456). With the display's object selection tool a
  click on an event selects it, Ctrl or Shift adds it and a click on a selected one with them takes it out, as on
  automation tracks, and a drag moves the selected ones in time and tempo (`tempo.set`); a drag elsewhere selects the
  events in a rectangle (`.tool-rectangle`, with Ctrl or Shift added to the selection), and a click there adds an event
  with the tempo where the mouse is (`tempo.add`) (p. 1450). The draw tool adds events, the erase tool removes them but
  the first. The curve shows the tempos from 20 BPM below the slowest event to 20 above the fastest, in tens.
- **Current Tempo** (pp. 162, 1446, 1449): the transport bar's tempo field, the tempo track's control and the tempo
  track editor's field show the same tempo, `ui::currentTempo`: the fixed tempo, or in tempo track mode the tempo the
  project plays at the project cursor, which all three follow during playback. The track list's field reads the time
  context the event display keeps per project answer, and the list watches the cursor only while playing and while it
  has the field (#1338). It keeps its tempo fields from when it shows the rows' controls, so that a frame only fills
  them, without searching the list for them (#1406). A tempo typed (`ui::currentTempoCommand`) sets the fixed tempo
  (`project.set` `tempo`), or changes the tempo event at or before the cursor (`tempo.set`), the first one in a project
  without tempo changes.
- **The tempo track's controls** (p. 162), in the track list (the user setting `ui.trackControls`, all three by
  default): Activate Tempo Track (`tempoTrack`), Current Tempo (`currentTempo`, a text field that takes a tempo with
  Enter) and Type of New Tempo Points (`tempoType`, a drop-down list for `editing.tempoType`).
- **Select All Events** (p. 1450): the UI command `tempo.selectAll`, in the track list's context menu, selects every
  tempo event on the lane while the tempo track is selected.
- **Insert Tempo Event at Cursor** (pp. 1452-1453): the UI command `tempo.insertAtCursor`, without a key until the user
  binds one, opens `resources/ui/insert-tempo.rml` with the tempo at the cursor; OK adds a tempo event with the tempo
  typed at the cursor (`tempo.add` `position: "cursor"`). Text that isn't a tempo, or a tempo the engine refuses, shows
  `#insert-tempo-message` and keeps the dialog open.
- **Activate Tempo Track** (p. 162): the tempo track's control `tempoTrack` in the track list (the user setting
  `ui.trackControls`) and the transport bar's `#tempo-track` button switch the mode, checked in tempo track mode; also
  the UI command `tempo.toggleTrack` in Project > Tempo Track.
- **Tap Tempo** (pp. 1455-1458): the transport bar's `#tap-tempo` button. Clicks on it within two seconds of each other
  are a run; meanwhile the tempo field shows the tempo they beat (`ui::tappedTempo`: the average time between them),
  and two seconds after the last one it goes to `tempo.tap`, by the user setting `transport.tapTempoMode`, with the
  start of the project window's selection range or selected events and parts as `selection`
  (`EventDisplay::selectionStart`). The times are RmlUi's clock, which tests stop and move on. While tapping the button
  is `.tapping` and each tap shows where the tempo goes (`ui::tapTempoMessage`, p. 1455) in `#tap-tempo-message`, a
  notification over the event display (over the transport panel's right end in the panel). A right click on it opens
  the Tap Tempo Mode pop-up menu (p. 1458), `menuitem[tap-mode]`, which sets `transport.tapTempoMode` and is checked by
  it.
- **File > Export > Tempo Track...** (`file.exportTempo`) and **File > Import > Tempo Track...** (`file.importTempo`)
  choose the file in the OS's file dialog; an exported file without an extension gets `.xml`.
- The track list's Duplicate Tracks, Move Selected Tracks to New Folder and drags into folders leave the tempo track
  out: the project has one, at the top level. Remove Empty Tracks, the engine command `track.removeEmpty`, never
  removes it.
- **The tempo track editor** (`ui::TempoEditor`, `resources/ui/tempo-editor.rml`, pp. 1447-1449, 1455): Mod+T (the UI
  command `window.tempoTrack`, also Project > Tempo Track > Open Tempo Track Editor) opens it in a window of its own and
  closes it. The toolbar has Activate Tempo Track, the object selection, draw and erase tools, the type of new tempo
  events (`editing.tempoType`) and Current Tempo: the fixed tempo, or in tempo track mode the tempo at the project
  cursor, where a tempo typed changes the event at or before the cursor; Enter ends the typing, so that the field goes
  on following the cursor, and what isn't a tempo goes. Below them the info line shows the first selected event's
  position, tempo and type, which change it (`tempo.set`); then a bars and beats ruler, the tempo scale and the tempo
  curve of the whole project. Backspace and Delete remove the selected events.
- Project > Add Track has a Tempo page.
- The tempo track editor's time signature display, under its ruler, has the time signature events
  ([signature-track.md](signature-track.md)).

## Not yet

The tempo event editor's smart controls for tilting, compressing and scaling a selection of events (p. 1450), drawing
several events in one drag with the draw tool, snapping new and moved events to the grid, the editor's zoom and
auto-scroll, visible tempo limits of the user's own, Lock, tempo recording (p. 1453) and reading the reference's own
tempo track files. Process Tempo, Process Bars and time warp are [time-warp.md](time-warp.md)'s. Tempo detection and
tempo from MIDI are [tempo-detection.md](tempo-detection.md)'s. The selected events on the tempo track's lane are
removed with the erase tool, not with Backspace, which deletes the selected events and parts. The key command for Tap
Tempo (Shift and Space) is not bound.

## Tests

`tests/tempo_tests.cpp`: the tempo track's limits, both modes keeping their tempo and undo, musical events moving and
linear ones staying when tempo events are added, moved and removed, ramps, the first event, simultaneous moves, events
at the cursor, `editing.tempoType`, every tap tempo mode, saving and loading, and tempo track files: the round trip with
time signature events from fixed tempo mode, the tempo modes after an import, undo, the same file exported again, and
files that aren't tempo track files. `tests/transport_tests.cpp`: the playback start in `transport.get`.
`tests/tempotrack_ui_tests.cpp`: the visible tempos, the tapped tempo, the curve on the tempo track's lane with clicks
that add, drag and erase events, the tempo track editor with Mod+T, its toolbar, info line and tools, its Current Tempo
following the cursor again after a tempo was typed in it, and its rectangle selecting events, Shift-click taking a
selected one out, Activate Tempo Track in the track list and the transport bar, Tap Tempo, also at a selected part, and
File > Export and File > Import > Tempo Track through the file dialog. `tests/tracklist_tests.cpp`: the track list's
commands leaving the tempo track out. `tests/tempocontrols_ui_tests.cpp` (#680, #831, #1338, #1406): Current Tempo and
the command that changes it, in the transport bar, on the tempo track and in the tempo track editor, during playback as
well as at the cursor, the Tap Tempo message for every mode, in the project window and in the transport panel, the fixed
tempo line, selecting events with Shift, Ctrl and a rectangle, also taking a selected event out with either, the Tap
Tempo Mode menu with the message and `.tapping` while tapping, also with the user setting changed after the bar opened,
the tempo track's Current Tempo and Type of New Tempo Points, the cursor watch that comes and goes with the tempo
track's Current Tempo field and the frames of a playback reading the time context the display keeps without building
one, the field following the cursor after tracks are added and removed during playback, Insert Tempo Event at Cursor
with the dialog's message for a tempo it can't take, and Select All Events.
`tests/transportbar_tests.cpp`: a drag and a wheel gesture on the tempo field, in fixed tempo mode and in tempo track
mode, each one undo step.
