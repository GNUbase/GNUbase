# Time warp, Process Tempo and Process Bars

The time warp tool, the Process Tempo and Process Bars dialogs, Set Definition from Tempo and copying warp markers
(#159, reference manual pp. 1463-1468). They edit the tempo track ([tempo-track.md](tempo-track.md)), the time
signature events ([signature-track.md](signature-track.md)) and the clips' warp ([warp.md](warp.md)).

## Engine commands

`src/engine/document/timewarp.cpp`. Each is one undo step and changes nothing if it fails.

| Command | What it does | Reference |
|---|---|---|
| `tempo.warp` | Warp Grid: the musical `position` (ticks) plays at the time `to` (samples): the tempo event before it gets the tempo, 1 to 360 BPM, found by bisection; with later tempo events a tempo event at `position` keeps the tempo after it. Musical tracks keep the time of what they hold, or with `follow` (musical events follow) its position. Needs tempo track mode | pp. 1465-1466 |
| `tempo.process` | Process Tempo: the tempo from `start` to `end` (ticks, default the locators) times the factor that makes the range last `newLength` seconds or end at `newEnd`; the tempo after it stays and musical tracks follow. Returns the factor (Equivalent Tempo Scaling). Needs tempo track mode | pp. 1463-1464 |
| `tempo.processBars` | Process Bars from `bar` (zero-based) for `bars` bars with `numerator`/`denominator`: `insert`, `delete`, `replace`, `reinterpret` | p. 1464 |
| `event.setDefinitionFromTempo` | Set Definition from Tempo for the audio events' clips: musical mode, the grid on each quarter where the event plays it now; the events' tracks, or with `allTracks` every track, become musical first | pp. 1467-1468 |
| `event.pasteWarpMarkers` | Copy and Paste Warp Markers: the markers of `from`'s clip into the clips of `events`, at the same times, but none that would pass a marker there or make a section play beyond the real-time stretcher's range | p. 1467 |

- **Keeping time.** Where what musical tracks hold keeps its time while the tempo changes (Warp Grid, Reinterpret
  Bars), the musical tracks are linear while it changes and musical again after it (`track.set`, as tempo detection
  does), so their positions in ticks change.
- **Replacing a stretch of the tempo track** (`replaceTempo`): Process Tempo and Process Bars replace the ticks of a
  range by other ticks at a factor of their tempo, and move the tempo events after it by the difference. A ramp into
  the range's start or end ends a tick before it, so that the tempo jumps there and the time before and after the
  range stays.
- **Process Bars** edits only the time signature events and the tempo track, as the manual says ("by automatic
  adjustment of the signature and tempo events"): events, parts, markers and automation keep their positions, as for
  any time signature change. `insert` puts the bars in at the tempo there and moves the later time signature and tempo
  events by them; `delete` removes the bars and moves those after back; `replace` gives the bars the time signature
  at the same tempo; `reinterpret` too, with the tempo scaled so that they last as long as before, and what musical
  tracks hold keeps its time. `range.insertSilence` and `range.delete` with `time` move the events too.
- **Set Definition from Tempo** makes a clip's definition grid the project's quarters from the one at or before the
  clip's start to its end, each edited onto the clip frame where the event plays it now (`ClipWarp::beats`), and its
  tempo the project's after the last one. In musical mode the event then plays as before, and follows the tempo from
  then on, including an event that starts between quarters. Beat 0 can precede the clip's first frame. Selected
  events sharing a clip get independent clips, so each placement gets its own definition and unselected events
  keep theirs. A clip in musical mode already is refused; where an event plays a clip frame is taken unwarped.
- **Pasted warp markers** go where the source event plays the marker's frames unwarped, into the frames that the
  destination event plays then; those outside the destination event or passing one of its clip's markers are left
  out, and its markers stay. Each destination clip is visited once, using its first event in the request.

## UI

- **The time warp tool** (`Tool::TimeWarp`, `timeWarp`, `src/ui/eventtimewarp.cpp`): the toolbar button, toolbox and
  context menu's Tools, or `tool.timeWarp`, choose it. A second click opens its modes (`#time-warp-toolbar-menu`);
  the context menu's Tools > Time Warp Mode (`#time-warp-menu`) chooses them too: Warp Grid, Warp Grid (musical
  events follow) and Free Warp (`EventDisplay::setWarpMode`), checked as chosen. The toolbar hides sections that no
  longer fit in a narrow window. In the grid modes a drag sends `tempo.warp` on release, from the position pressed,
  snapped, to the time released, with `follow` in the second mode. In Free Warp a press on an audio event inserts a
  warp marker at the clip frame pressed that plays at the time released (`audioEvent.addWarpMarker`). The engine
  maps the project sample positions through musical mode and existing markers, so the marker moves the audio
  under the pointer even in an already warped event. It anchors a trimmed event's offset if inserting the marker
  would otherwise change its mapping. This command returns the clip's id and warp, as `clip.addWarpMarker` does,
  and is one undo step.
- **Project > Tempo Track > Process Tempo...** (`tempo.openProcessTempo`) opens `resources/ui/process-tempo.rml`: the
  locator range in seconds, New End Position `#pt-new-end` and New Length `#pt-new-length`, each following the other,
  and the Equivalent Tempo Scaling `#pt-scaling`; Process sends `tempo.process`.
- **Project > Tempo Track > Process Bars...** (`tempo.openProcessBars`) opens `resources/ui/process-bars.rml`: Start
  at Bar `#pb-start` (from 1), Length in Bars `#pb-length`, Process Type `#pb-type`, Time Signature `#pb-numerator`
  and `#pb-denominator`; Process sends `tempo.processBars`.
- **Audio > Advanced > Set Definition from Tempo...** (`audio.setDefinitionFromTempo`) opens
  `resources/ui/definition-from-tempo.rml` for the selected audio events: Set all Tracks to Musical Time Base
  `#dt-all-tracks`; OK sends `event.setDefinitionFromTempo`.
- **Audio > Advanced > Copy Warp Markers from Selected Event** (`audio.copyWarpMarkers`) remembers the first selected
  audio event, **Paste Warp Markers to Selected Events** (`audio.pasteWarpMarkers`) sends `event.pasteWarpMarkers`
  for the selected ones.

## Not yet

The ruler's and the events' tempo flags while the time warp tool is chosen, the ruler switching to bars and beats,
showing the warp while dragging and snapping the release to events; Free Warp snapping to hitpoints and magnetic
markers; the Process Tempo and Process Bars buttons of the tempo track editor, the dialogs' bars and beats and other
time formats; Write Definition to Audio Files.

## Tests

`tests/timewarp_tests.cpp`: time warp in both grid modes, with a later tempo event and its limits; Process Tempo by
length and end, from the locators and with a ramp across the range's end; every Process Bars type and its errors;
Set Definition from Tempo playing as before and following a tempo change, for the events' tracks or all, with off-beat
starts and shared clips, including inactive track versions; pasting warp markers once per clip, keeping those there,
and undo; Free Warp through musical mode, existing markers and trimmed events. `tests/timewarp_ui_tests.cpp`:
the tool's toolbar button and second-click modes,
Warp Grid and Free Warp drags on plain and warped clips, its modes in the context menu, the Process Tempo and
Process Bars dialogs, and Audio > Advanced's Set Definition from Tempo and Copy and Paste Warp Markers.
