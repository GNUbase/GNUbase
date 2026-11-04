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
  either. `project.set` `tempo` (the transport's tempo field) sets the first point of the tempo the project plays: the
  fixed tempo, or the tempo track's first event.
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

## UI

- **The tempo track's lane** (`ui::TempoLane`, `src/ui/tempotrack.h`): a `ui::TempoCurve` over the tempo track's lane
  in the event display, following the project and the view: the tempo track's events as a `<curve class="tempo-line">`
  on the timeline of the tempo the project plays (ramps as straight pieces, jumps as steps) and a `.tempo-point` per
  event, `.inactive` in fixed tempo mode. With the display's object selection tool a click on an event selects it (Ctrl
  adds or removes it) and a drag moves the selected ones in time and tempo (`tempo.set`); a click elsewhere adds an event
  with the tempo where the mouse is (`tempo.add`). The draw tool adds events, the erase tool removes them but the first.
  The curve shows the tempos from 20 BPM below the slowest event to 20 above the fastest, in tens.
- **Activate Tempo Track** (p. 162): the tempo track's control `tempoTrack` in the track list (the user setting
  `ui.trackControls`, its only one) and the transport bar's `#tempo-track` button switch the mode, checked in tempo track
  mode; also the UI command `tempo.toggleTrack` in Project > Tempo Track.
- **Tap Tempo** (pp. 1455-1458): the transport bar's `#tap-tempo` button. Clicks on it within two seconds of each other
  are a run; meanwhile the tempo field shows the tempo they beat (`ui::tappedTempo`: the average time between them),
  and two seconds after the last one it goes to `tempo.tap`, by the user setting `transport.tapTempoMode`, with the
  start of the project window's selection range or selected events and parts as `selection`
  (`EventDisplay::selectionStart`). The times are RmlUi's clock, which tests stop and move on.
- The track list's Remove Empty Tracks, Duplicate Tracks, Move Selected Tracks to New Folder and drags into folders
  leave the tempo track out: the project has one, at the top level.
- **The tempo track editor** (`ui::TempoEditor`, `resources/ui/tempo-editor.rml`, pp. 1447-1449, 1455): Mod+T (the
  UI command `window.tempoTrack`, also Project > Tempo Track > Open Tempo Track Editor) opens it in a window of its own
  and closes it. The toolbar has Activate Tempo Track, the object selection, draw and erase tools, the type of new tempo
  events (`editing.tempoType`) and Current Tempo: the fixed tempo, or in tempo track mode the tempo at the project
  cursor, where a tempo typed changes the event at or before the cursor. Below them the info line shows the first
  selected event's position, tempo and type, which change it (`tempo.set`); then a bars and beats ruler, the tempo scale
  and the tempo curve of the whole project. Backspace and Delete remove the selected events.
- Project > Add Track has a Tempo page.
- The tempo track editor's time signature display, under its ruler, has the time signature events
  ([signature-track.md](signature-track.md)).

## Not yet

The tempo event editor's smart controls for tilting, compressing and scaling a selection of events (p. 1450), drawing
several events in one drag with the draw tool, snapping new and moved events to the grid, the editor's zoom and
auto-scroll, visible tempo limits of the user's own, Lock, tempo recording (p. 1453) and the Process Tempo and Process
Bars dialogs (pp. 1463-1464), tempo detection (#157) and time warp. The selected events on the tempo track's lane are
removed with the erase tool, not with Backspace, which deletes the selected events and parts. The key command for Tap
Tempo (Shift and Space) is not bound.

## Tests

`tests/tempo_tests.cpp`: the tempo track's limits, both modes keeping their tempo and undo, musical events moving and
linear ones staying when tempo events are added, moved and removed, ramps, the first event, simultaneous moves, events
at the cursor, `editing.tempoType`, every tap tempo mode and saving and loading. `tests/transport_tests.cpp`: the
playback start in `transport.get`. `tests/tempotrack_ui_tests.cpp`: the visible tempos, the tapped tempo, the curve on
the tempo track's lane with clicks that add, drag and erase events, the tempo track editor with Mod+T, its toolbar, info
line and tools, Activate Tempo Track in the track list and the transport bar, and Tap Tempo, also at a selected part.
`tests/tracklist_tests.cpp`: the track list's commands leaving the tempo track out.
