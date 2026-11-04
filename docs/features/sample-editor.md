# Sample editor

The sample editor (#140, reference manual pp. 613-635): one audio event's clip in a window of its own, with the
toolbar's tools, the info line, the overview, the waveform display with the event's edges, the selection range and the
snap point, the inspector's Range and Process sections, and the regions list.

## In the engine

`src/engine/document/sampleeditor.cpp` keeps the editor's view on the command thread, like the MIDI editors' views
([midi-editors.md](midi-editors.md)): view state, so no undo steps, the project isn't modified, nothing is saved, and a
new or loaded project closes it.

| Command | What it does | Reference |
|---|---|---|
| `sampleEditor.get` | The view: `open`, `event` with its `clip`, `track` and `media` (0 while none, or while the event is gone: an undo brings it back), the clip's `length`, the event's `offset` and `eventLength`, `clipStart` and `ratio` (clip frame f plays at project sample `clipStart + f * ratio`), and the selection range `selectionStart` and `selectionEnd` in clip frames | |
| `sampleEditor.open` | Opens it with an audio event; another event drops the selection | p. 613 |
| `sampleEditor.set` | The selection range: `select` `all`, `none`, `event` or `loop` (between the locators, within the clip), or `selectionStart` and `selectionEnd`; `zeroCrossing` moves both ends to the nearest zero crossing within 50 ms | pp. 618, 635 |
| `sampleEditor.close` | Closes it | |

Each emits `sampleEditor.changed` with the view. Positions in the editor are clip frames, as the reference counts them
from the clip's start (p. 626).

**Regions** (pp. 624-626) are in the project: a clip's `regions`, each with an id, a name and its start and end in
clip frames, by start. They are saved in `gnubase.json`; DAWproject has no place for them. Each command is one undo
step and emits `clip.changed`.

| Command | What it does | Reference |
|---|---|---|
| `clip.addRegion` | A region from `start` to `end`, named `name` or Region and its number | pp. 620, 624-625 |
| `clip.setRegion` | Renames it or moves its start or end | p. 625 |
| `clip.removeRegion` | Removes it | p. 626 |

Snap to zero crossing reads the file on the command thread with `zeroCrossingFrame` (`snap.h`), which `event.split` and
`event.resize` use too ([snap.md](snap.md)). The snap point is the event's (`event.setSnapPoint`, snap.md).

## In the UI

`ui::SampleEditor` (`src/ui/sampleeditor.h`) fills the sample editor window (`resources/ui/sample-editor.rml`), which
`app::AppUi` opens and closes as the view does, whichever client opens it; closing the window closes the view. The
window opens with a double click on an audio event in the project window (outside a crossfade, which opens the
crossfade editor), and with Open/Close Editor (Mod+E, Return) when audio events but no MIDI parts are selected: the
first of them. Without either selected, Open/Close Editor closes it.

```
.sample-editor[tool=range|zoom|play|scrub]   .empty without an event; .no-info, .no-overview, .no-regions
  .toolbar.sample-editor-toolbar
    button.se-tool[se-tool]       the tools; .checked for the current one
    #se-zero-crossing             Snap to Zero Crossing (the project's snap setting); .checked while on
    button.se-layout[layout]      Set up Window Layout: info, overview, regions; .checked while shown
  .info-line.se-info              [field=format|length|selectionStart|selectionEnd|selectionLength|zoom]
  .se-overview                    waveform.se-overview-wave of the whole clip, .se-overview-event,
                                  .se-overview-selection, .se-overview-snap and .se-overview-view (what the display shows)
  .se-body
    .se-inspector                 Range: button.se-select[select], #se-set-locators, #se-locate;
                                  Process: #se-range-region
    .se-main
      .se-display                 waveform.se-wave, .se-outside (.se-before, .se-after the event), .se-region-shown,
                                  .se-selection (.off without one), .se-snap-point
      .se-regions                 #se-add-region, #se-remove-region, #se-select-region and .se-region-list with a
                                  .se-region[region] per region (.se-region-name, -start, -end; .selected)
```

The display's time axis is the clip's frames: `start` and `scale` (frames per dp) on the waveform, as for every canvas
element ([canvas.md](canvas.md)). It first shows the whole clip, as Zoom Full does with clip-based zoom (p. 623).
Waveforms come from `media.peaks` ([peaks.md](peaks.md)), one question for each of the two in flight.

| Tool | Gesture | Reference |
|---|---|---|
| Range Selection | A drag selects a range, with Snap to Zero Crossing at zero crossings; Shift and a click moves its nearer end; a click deselects | p. 635 |
| Zoom | A click zooms in around the frame clicked, with Alt out, at most to the whole clip | p. 630 |
| Play | Auditions the event's track from the frame pressed until the release (`audition.start`, [audition.md](audition.md)) | p. 630 |
| Scrub | A drag scrubs the track (`audition.scrub`) | p. 634 |

- **Snap point** (pp. 626-628): a line at the event's snap point, at its start without one of its own; a drag with any
  tool sets it within the event (`event.setSnapPoint`).
- **Info line** (p. 614): the clip's sample rate and channels, its length, the selection range and the zoom in samples
  per pixel. Positions are sample frames from the clip's start.
- **Overview** (p. 615): a click or drag shows that part of the clip in the display.
- **Range section** (pp. 618-619): Select All, Select None, Select in Loop and Select Event; Set Locators to Selection
  Range and Locate Selection, of the selection range or else of the event.
- **Process section** (p. 620): Event or Range as Region.
- **Regions list** (pp. 624-626): Add Region makes one of the selection range or else of the event, Remove Region
  removes the one selected in the list, Select Region (or a double click on it) selects its section and zooms to it;
  the display shows the selected region.

`resources/styles/sampleeditor.rcss` lays it out; the themes color it (`tools/skin/generate.py`).

## Not yet

The editor in the lower zone and its inspector in the left zone; Keep Editor Contents and following the project
window's selection; several events at once (Clip Editing Mode, Clip Display Mode, Activate Clip for Editing); the
ruler, the grid and snapping to it, the level scale and vertical zoom, and horizontal scrolling and zoom other than the
zoom tool and the overview; the Zoom submenu; Solo Editor, Audition, auto-scroll, the independent track loop and the
toolbar's context menu; positions in the project's time format, and typing them; the Draw tool's volume curve and
sample editing; renaming regions and moving their edges with the mouse, Play Region and dragging regions into the
project window; the clip's snap point when opened from the pool. Hooks for other tickets: the Process section's Cut,
Copy, Paste, Delete, Insert Silence and offline processing (#141), the Hitpoints section (#143), the Definition,
warping, musical mode and the Time Warp tool (#144), and making a sampler track of the range.

## Checks

- `tests/sampleeditor_tests.cpp`: the view as view state with its event and selection, Select All, None, Event and in
  Loop, a range of its own and its errors, an event that goes and comes back with undo, opening and closing, a new
  project closing it; zero crossings of a sine; regions added, sorted, changed and removed with undo, their errors,
  and saved with the project.
- `tests/sampleeditor_ui_tests.cpp` drives the app with `ui::Automation`: Open/Close Editor opening the selected event,
  the info line, the event's edges in the display and the overview, both waveforms' columns, closing the window; the
  range tool's drag, Shift and a click, Snap to Zero Crossing, the Select menu, Set Locators to Selection Range and
  Locate Selection; Event or Range as Region, Add Region, the list, Select Region and Remove Region with undo; dragging
  the snap point, the zoom tool in and out, a click in the overview, the play and scrub tools auditioning the track and
  the window layout's switches; Open/Close Editor without a selection closing it.
- `tests/eventediting_tests.cpp`: a double click on an audio event opens it in the sample editor.
