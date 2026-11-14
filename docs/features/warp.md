# Tempo matching and warping

Musical mode, a clip's definition grid (Auto Adjust and Manual Adjust), free warp markers, flattening and unstretching
(#144, reference manual pp. 648-658). The stretching itself is the time-stretch engine's
([time-stretch.md](../time-stretch.md)).

## In the document

A clip's `warp` (`ClipWarp` in `document.h`), saved with the clip in `gnubase.json`:

| Field | What it is |
|---|---|
| `musical` | Musical mode: the clip's beats follow the project's tempo |
| `tempo` | The clip's tempo, in BPM of quarter notes; 0 until one is set or musical mode is turned on |
| `beatsPerBar` | Quarters per bar, for bars and beats and Auto Adjust |
| `gridStart` | The clip frame of beat 0 |
| `beats` | Beats (quarters from the grid start) that Auto Adjust or Manual Adjust put elsewhere than the tempo would: `beat` and `frame` |
| `markers` | Free warp markers: the audio at clip frame `position` plays where clip frame `target` would without it |
| `preset` | The time stretch algorithm preset of real-time warping |

A clip is warped while it is in musical mode or has warp markers. Where an event of a warped clip plays its clip frame
f (`warpedOffset` in `src/engine/document/warp.cpp`): the markers map f to a clip frame w, linearly between them and
the clip's ends; without musical mode, w plays at the event's start plus (w - w at the event's offset) at the clip's
sample rate; in musical mode the definition grid maps w to beats (linearly between the beats edited, at the clip's
tempo beyond them), and the beats after the event's start play where the project's tempo puts them. An event's
length stays in clip frames; its end on the timeline (`eventSpan`) follows the warp, so overlaps, crossfades and
playback see where it really ends. Every conversion between timeline positions and clip frames goes through
`eventSamples` and `eventFrames` (`fadeedits.cpp`), which follow the warp: splitting, sizing (sizing that moves the
contents keeps the edge that stays put, searching for the offset that ends the event there), snap points and snapping
to them and to zero crossings, drawn volume curves, hitpoint slicing, the audio functions (Detect Silence's splits
among them), Crossfade's closed gaps and overlaps in audio quantizing, and the sample editor's Select Locators; only
the sample editor view's `clipStart` and `ratio` don't (see Not yet). An event's own fades play in frames at the
clip's sample rate from its edges, so together they fit into what it plays for at that rate (`fadeRoom`), which
`audioEvent.setFades`, the fades to the range and Strip Silence's Apply Fades (each at most half of it) use; the fades
to the range measure the range's distance from the event's edges at that rate too (`fadesToRange`). As the room
depends on the tempo in musical mode and on the warp markers, a tempo change or a marker move can leave the fades
longer than it: they stay, and `audioEvent.setFades` takes any change that doesn't make them longer together.

`project.get` gives each event of a warped clip `playedLength`, the project samples from its start to its end through
the warp, and `fadeRoom` (`audioEventJson` in `warp.cpp`), which the project window and the fade editor go by. They
are derived, so not in the project file. After every edit, undo and redo, `ProjectModel` announces `audioEvent.changed`
with them for each event whose `playedLength` or `fadeRoom` changed, as after a tempo change, a time signature or warp
edit, and for each warped event that changed, whose own announcement lacks them; an event that stops being warped is
announced without them (`announceWarpedEvents`). An edit that changes none of the tempo, the media, the clips and the
events skips the comparison, and it compares the two lengths, not the events' JSON; `project.get` adds them to the
event JSON it has (`addWarpedLengths`).

## DAWproject

`project.xml` gives a warped event's clip a `Warps` element: its `Audio` and `Warp` points from its warp map where the
map bends, each the second of the file (`contentTime`, `contentTimeUnit="seconds"`) that plays at a time from the
event's start (`time`). In musical mode the times are in beats (`timeUnit="beats"`, the clip's `contentTimeUnit` too),
so they follow the tempo in another application as they do here, else in seconds; the clip's `playStart` is 0 and its
`duration` where the event ends through the warp. `gnubase.json` still has the clip's warp, which wins when GNUbase
opens its own file. A clip in musical mode without a tempo there, which only a file edited elsewhere holds, takes the
project's tempo at its first event when the project loads, as `clip.setWarp` gives it one, so that its length in beats
stays when the tempo changes later, as a MIDI file import may change it ([file-import.md](file-import.md)).

A project from another application, or without `gnubase.json`, gets the points of a `Warps` element in a clip of the
event's own (`readWarps` in `dawproject.cpp`): in beats musical mode, with beat 0 of the definition at the content's
first whole beat in the audio, the tempo of that beat and edited beats where the content's tempo bends; in seconds free
warp markers, which keep the first point's audio where it is. Points GNUbase can't play, such as a stretch of the whole
clip in seconds (the clip's ends stay put with markers), sections beyond the real-time range or a `playStart` and
`duration` whose audio isn't within the file, leave the event unwarped, as before. The event's fades keep what they
played for, within its `fadeRoom`. Events of the same file whose warps come out the same share one clip, and when no
event plays the file unwarped, that is the file's own clip, so the pool gets no unused clips.

## Engine commands

Each is one undo step and emits `clip.changed`, except `event.flatten`, which emits `media.added`, `clip.added` and
`audioEvent.changed`.

| Command | What it does | Reference |
|---|---|---|
| `clip.setWarp` | Musical mode on or off (a clip without a tempo takes the project's where its first event starts, also one on an inactive track version, so nothing changes until the tempo does), `tempo`, length in `bars` and `beats` (sets the tempo), `beatsPerBar`, `gridStart` and `preset` | pp. 616, 648-650 |
| `clip.autoAdjust` | The tempo and grid start from a section (`start`, `end`; default the clip) that starts and ends at a barline and is `bars` bars long (default: the nearest whole bars), and every beat with a hitpoint that counts within a quarter beat of it edited onto that hitpoint; the snap point of `event` (the sample editor's) moves to the section's start | pp. 651-652 |
| `clip.adjustGrid` | Manual Adjust: `mode` `start` (Set Grid Start), `stretch` (Stretch Grid: beat `beat`, default the second bar, at `frame` by the tempo), `bar` (Set Bar Position: the beats edited after it move with it), `beat` (Set Beat Position), `remove` (Remove Beat Edit) | pp. 652-654 |
| `clip.addWarpMarker`, `clip.setWarpMarker` | Inserts a warp marker at `position` playing at `target`, moves one; markers can't pass each other, and no section between two markers, or a marker and the clip's ends, may play more than 4 times or less than a quarter as long, the real-time stretcher's range (`checkMarkers`, which warp quantizing and `event.pasteWarpMarkers` keep to as well). A section the edit leaves playing as it did, give or take the half frame a marker inserted on it is rounded by, may stay beyond the range, so a clip from a project saved before the range was checked still takes edits elsewhere | pp. 654-656 |
| `clip.removeWarpMarkers` | Removes those at `positions`, or all (Reset: musical mode and the definition stay) | p. 656 |
| `event.unstretch` | Unstretch Audio: musical mode off and no markers in the events' clips | pp. 657-658 |
| `event.flatten` | Flatten Real Time Processing: renders each event of a warped clip as it plays now (with `preset`, default the clip's) with its clip's fades into a 32-bit float WAV file in the project's media folder, and makes the event play a new clip of it; its own fades keep their lengths. When an event fails, the files rendered for the others go | p. 657 |

MCP and Lua get them from the command registry ([Lua API reference](../api-reference.md), [mcp.md](../mcp.md)).

## Playback

The render snapshot gives each event of a warped clip a media file of its own, with the event's id
(`RenderMedia::warp`, `warpEvent` in `rendersnapshot.cpp`): the clip's file, a warp map from its clip frames to frames
from the event's start (a point every 4096 clip frames, at every marker and at every beat edited, so tempo ramps and
edits are followed closely), and the preset. Playback then treats it as any other file, crossfades and fades included.
The clip's fades play where the warp puts the clip's ends (`warpedClipEdges`, `RenderAudioEvent::clipStart` and
`clipEnd`), also for an event that starts or ends inside them. These entries carry the event's id but aren't media
files: the peak cache and `audition.start` skip them.

`Playback` opens such a file through `dsp::WarpReader` (`src/engine/dsp/warp.h`): a real-time `TimeStretcher` whose
ratio is the slope of the map, fed and read by the disk thread that fills the voice's disk stream, so the audio thread
reads finished frames as before and the real-time rules hold. A ratio change takes effect on the input that leaves the
stretcher next, a start pad behind what it takes, so each segment's ratio is set that much early; clicks land within a
few milliseconds of where the map puts them (tested). A seek resets the stretcher, feeds the start pad from before the
position and drops the latency. A segment of the map that plays more than 4 times or less than a quarter as long as
its audio, as musical mode does far from the clip's tempo or across beats edited that far, is beyond the stretcher's
range: its clamped ratio plays the segment's audio too short or too long, so the reader starts over at the segment's
end as after a seek, and the audio after it is back where the map puts it (tested with a quarter at 8 times its
length). Within such a segment the audio still plays at the clamped ratio. Across a long stretch beyond the range, as
musical mode far from the clip's tempo makes, that is a reset every map segment (4096 clip frames); resetting only at
the end of the stretch, or at the definition's beats, would reset less often, but the clamped ratio would carry the
audio up to half the stretch away from the map (at 8 times, the end of the clip's audio half way through it), so the
reader keeps to the segments.

The disk thread that fills every voice's stream does the stretching. Measured with events of a loop that musical mode
stretches from 90 to 120 BPM on the quality preset, playing in real time on the dummy device while other builds kept the
machine busy: a Debug build keeps up with 4 such events and drops out with 8 (about a second of 2). The `[realtime]`
test plays 4; a Release build, which CI runs the tests in, is several times faster. Spreading the voices over more disk
threads is a follow-up if real projects need it. A changed map, as after a tempo change, opens new voices; the stretcher
allocates when one opens, never on the disk thread's reads.

Flattening reads through the same reader and applies the clip's fades as playback does, so a flattened event sounds
exactly as it played (the render tests compare them, with and without fades); offline mode's better quality for it is
a follow-up.

## In the UI

The sample editor's inspector ([sample-editor.md](sample-editor.md)) has a Warp section and Flatten in its
Process section:

```
#se-musical        Musical Mode, .checked while on (clip.setWarp)
#se-audio-tempo    valuefield: the clip's tempo (clip.setWarp tempo)
#se-auto-adjust    Auto Adjust over the selection range, or else the event, moving its snap point (clip.autoAdjust)
#se-free-warp      Free Warp, .checked while on; the editor gets .free-warp
#se-warp-reset     Reset (clip.removeWarpMarkers)
#se-flatten        Flatten (event.flatten of the event)
.se-warp-markers   .se-warp-marker[marker][target] each, at its target: .se-warp-marker-line, .se-warp-marker-handle
```

With Free Warp on, a drag in the display inserts a marker where it starts and moves it to where it ends, a drag of a
marker moves it, and a click on one with a modifier key removes it. The Audio menu's Realtime Processing has Flatten
Realtime Processing (`audio.flatten`) and Unstretch Audio (`audio.unstretch`) for the selected events.

The project window ([event-display.md](event-display.md)) draws a warped event from its start to its end through the
warp (`playedLength`), its waveform stretched evenly over it, and its fade handles and the fade editor dialog
([fades.md](fades.md)) keep its fades within `fadeRoom`.

## Not yet

The Definition section's rulers, Grid Resolution and the bars and beats fields, and Manual Adjust's gestures in the
display (the engine has them: `clip.adjustGrid`); the Warping Algorithm pop-up and the Flatten dialog's preset (the
engine takes `preset`); Swing; Stretch to Project Tempo and Sizing Applies Time Stretch; Free Warp snapping to
hitpoints, warp markers and zero crossings, several markers removed by a rectangle, and Free Warp for several clips at
once (the project window's time warp tool has Free Warp, [time-warp.md](time-warp.md)); the note and warp icons on
events and the pool's Musical Mode column; the tempo of loops with embedded tempo on import; the sample editor's
positions following the warp (they show the unwarped clip), and the project window's waveform of a warped event
following its warp within the event (it is stretched evenly), with the event's snap point, which it draws where the even
stretch puts it while the engine snaps to it through the warp; event fades keep their length in clip frames; auditioning
and scrubbing play warped events unstretched; flattening in offline mode; the audio within a map segment beyond the
stretcher's range playing as long as the segment (the reader gets back onto the map at its end); a `Warps` element in
seconds that stretches a whole clip, and one whose audio is in beats, on import, and in beats the points that bend
between beats, as musical mode with free warp markers exports them (the import keeps the whole beats, so the markers are
lost); more disk threads for many warped events.

## Checks

`tests/warp_tests.cpp`: the warp reader placing clicks where a map with a ratio change puts them, also after a seek; the
render test of musical mode following a fixed tempo and a tempo track change, with undo, and off again; bars and beats,
Auto Adjust from hitpoints moving the snap point and every Manual Adjust mode with their errors; an edited beat playing
on the project's beat; free warp markers moving clicks, moved, refused (also beyond the real-time range, 1:8 both ways),
removed and reset; warp quantizing keeping a marker within the range, and leaving the markers after it room for their
targets; a clip loaded with a section beyond the range still taking warp quantizing, pasted markers and inserted ones
elsewhere, and a marker rounded onto it; a marker next to a section at the limit of the range refused a frame off the
map and taken when only rounded; Crossfade closing a gap and going into the next event through the warp; clip fades of a
warped event that starts and ends inside them; unstretching, and flattening playing exactly as before (also with event
and clip fades) and no longer following the tempo, with undo, and leaving no files when it fails; fades to the range,
drawn volume curves and hitpoint cuts through the warp; sizing with the contents keeping the other edge; zero crossings,
the snap point and Detect Silence's splits through the warp; the sample editor's Select Locators; fades that stop
fitting after a tempo change still changing; the warped events' entries refused as media files; the warp saved and
loaded; a clip in musical mode loaded without a tempo taking the project's at its first event; one whose first event is
on an inactive track version taking the tempo there; a quarter in musical mode at 8 times its length leaving the clicks
after it where the map puts them; a warped event's `playedLength` and `fadeRoom` in `project.get` and announced after a
tempo change, an undo and an edit of the event, and gone out of musical mode; musical mode and warp markers through
`project.xml`'s `Warps` alone, playing as before and musical mode still following the tempo, `Warps` whose audio isn't
within the file, and two events of a file sharing its clip with a fade longer than the clip; 4 warped events on the
quality preset playing in real time without dropouts (`[realtime]`).
`tests/sampleeditor_ui_tests.cpp`: Musical Mode, the Audio Tempo field, Auto Adjust, Free Warp's drags and removal,
Reset, Unstretch Audio from the Audio menu and Flatten.
