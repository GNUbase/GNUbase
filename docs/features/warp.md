# Tempo matching and warping

Musical mode, a clip's definition grid (Auto Adjust and Manual Adjust), free warp markers, flattening and unstretching
(#144, reference manual pp. 648-658). The stretching itself is the time-stretch engine's ([time-stretch.md](../time-stretch.md)).

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
to them and to zero crossings, fades to the range, drawn volume curves, hitpoint slicing and the audio functions.

## Engine commands

Each is one undo step and emits `clip.changed`, except `event.flatten`, which emits `media.added`, `clip.added` and
`audioEvent.changed`.

| Command | What it does | Reference |
|---|---|---|
| `clip.setWarp` | Musical mode on or off (a clip without a tempo takes the project's where its first event starts, so nothing changes until the tempo does), `tempo`, length in `bars` and `beats` (sets the tempo), `beatsPerBar`, `gridStart` and `preset` | pp. 616, 648-650 |
| `clip.autoAdjust` | The tempo and grid start from a section (`start`, `end`; default the clip) that starts and ends at a barline and is `bars` bars long (default: the nearest whole bars), and every beat with a hitpoint that counts within a quarter beat of it edited onto that hitpoint; the snap point of `event` (the sample editor's) moves to the section's start | pp. 651-652 |
| `clip.adjustGrid` | Manual Adjust: `mode` `start` (Set Grid Start), `stretch` (Stretch Grid: beat `beat`, default the second bar, at `frame` by the tempo), `bar` (Set Bar Position: the beats edited after it move with it), `beat` (Set Beat Position), `remove` (Remove Beat Edit) | pp. 652-654 |
| `clip.addWarpMarker`, `clip.setWarpMarker` | Inserts a warp marker at `position` playing at `target`, moves one; markers can't pass each other, and no section between two markers, or a marker and the clip's ends, may play more than 4 times or less than a quarter as long, the real-time stretcher's range | pp. 654-656 |
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
position and drops the latency. A changed map, as after a tempo change, opens new voices; the stretcher allocates
when one opens, never on the disk thread's reads.

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

## Not yet

The Definition section's rulers, Grid Resolution and the bars and beats fields, and Manual Adjust's gestures in the
display (the engine has them: `clip.adjustGrid`); the Warping Algorithm pop-up and the Flatten dialog's preset (the
engine takes `preset`); Swing; Stretch to Project Tempo and Sizing Applies Time Stretch; Free Warp snapping to
hitpoints, warp markers and zero crossings, several markers removed by a rectangle, and Free Warp for several clips at once (the
project window's time warp tool has Free Warp, [time-warp.md](time-warp.md)); the note and warp icons on events and the pool's Musical Mode column; the tempo of loops with embedded
tempo on import; the event's length, its waveform in the project window and the sample editor's positions following
the warp (they show the unwarped clip); event fades keep their length in clip frames; auditioning and scrubbing play
warped events unstretched; flattening in offline mode; musical mode at more than 4 times or less than a quarter of the
clip's tempo, and beats edited that far, play beyond the stretcher's range, so the audio after them drifts from the
map; the warp in DAWproject's `Warps` element (it is saved in `gnubase.json` only, and the importer reads a `Warps`
element's audio without its warp points); measuring the disk thread's load with many warped events.

## Checks

`tests/warp_tests.cpp`: the warp reader placing clicks where a map with a ratio change puts them, also after a seek;
the render test of musical mode following a fixed tempo and a tempo track change, with undo, and off again; bars and
beats, Auto Adjust from hitpoints moving the snap point and every Manual Adjust mode with their errors; an edited
beat playing on the project's beat; free warp markers moving clicks, moved, refused (also beyond the real-time range,
1:8 both ways), removed and reset; clip fades of a warped event that starts and ends inside them; unstretching, and
flattening playing exactly as before (also with event and clip fades) and no longer following the tempo, with undo,
and leaving no files when it fails; fades to the range, drawn volume curves and hitpoint cuts through the warp;
sizing with the contents keeping the other edge; the warped events' entries refused as media files; the warp saved
and loaded.
`tests/sampleeditor_ui_tests.cpp`: Musical Mode, the Audio Tempo field, Auto Adjust, Free Warp's drags and removal, Reset, Unstretch Audio
from the Audio menu and Flatten.
