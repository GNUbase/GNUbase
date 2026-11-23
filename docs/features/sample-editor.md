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
clip frames, by start. They are saved in `gnubase.json`; DAWproject has no place for them, and loading leaves out
regions that aren't sections of the clip's media file. Each command is one undo step and emits `clip.changed`.

| Command | What it does | Reference |
|---|---|---|
| `clip.addRegion` | A region from `start` to `end`, named `name` or Region and a number no other region of the clip has | pp. 620, 624-625 |
| `clip.setRegion` | Renames it or moves its start or end | p. 625 |
| `clip.removeRegion` | Removes it | p. 626 |

Snap to zero crossing reads the file on the command thread with `zeroCrossingFrame` (`snap.h`), which `event.split` and
`event.resize` use too ([snap.md](snap.md)). The snap point is the event's (`event.setSnapPoint`, snap.md).

## Process commands

`sampleEditor.process` implements the Basic Commands on an audio event's clip range (reference manual pp. 619-620). It
takes `event`, `start` and `end` in clip frames and `operation`: `cut`, `copy`, `paste`, `delete`, `insertSilence` or
`draw`. Cut and Delete close the gap; Paste replaces the range with the copied samples; Insert Silence inserts the
range's length at its start without replacing any audio. Copy and Cut replace an engine audio clipboard, independent of
the system text clipboard and of undo. Paste converts the clipboard to the destination sample rate with the engine
resampler, then mixes its default speaker arrangement with the engine channel conversion gains. Mono duplicates at unity
into stereo; stereo folds to mono at -3.01 dB per channel. Uncommon channel counts map matching indices and leave extra
channels silent. The inserted length is rounded at the destination rate. Conversion streams on the job thread and leaves
the clipboard unchanged.

File reads and writes run as cancellable jobs, streaming blocks to new float WAV files in the project's media folder
(the recording folder before saving). The result is `job`; its result contains `clip`, `media` and `length` for an edit,
or `copied` for Copy. Original files stay intact. Failed, cancelled and stale jobs discard their files and leave the
clipboard alone. An edit fails if the project's revision changed during the job; selection changes and saving do not
invalidate it. These asynchronous commands cannot be used in `history.transaction`.

`sharedCopies` must be chosen when another event, sampler track or drum pad uses the clip. Without another user,
omitting it edits the clip in place and keeps the clip and region ids. `newVersion` makes an independent clip for this
event; `all` edits the existing clip and adjusts its other events, including inactive track versions. Locked events and
locked audio parts refuse edits. Each edit is one undo step, restoring the original clip, media and events. Clip
regions, hitpoints, event offsets, lengths, fades, volume points and snap points follow the changed range; empty regions
and events disappear. Warped clips and edited definition grids are supported by version 3. Musical mode, tempo and
surviving beat numbers stay. Grid points inside replaced audio are removed; later points move by the inserted length
minus the removed length. Warp anchors follow the same rule in both source and target coordinates; an anchor is removed
if either coordinate is inside replaced audio. Insert Silence moves anchors at its start with the audio to the right.
Surviving segments outside the splice keep their stretch. A removed grid start collapses to the splice, bounded by the
new clip end; beat edits that collide with it and warp anchors at clip endpoints disappear. Event fades map their
played-time boundaries back to source frames, through the edit, and into the new warp, within the new fade room. Clip
fades remain in source frames. Copy keeps the definition and warp intact and copies the source samples. Draw preserves
timing. The internal completion commands `sampleEditor.applyProcess` and `sampleEditor.finishProcess` accept only
engine-posted work; clients use `sampleEditor.process`.

The Process section's Basic Commands menu and the sample editor's Cut, Copy, Paste, Delete and Insert Silence key
commands send these operations for the selected range. Without a selection they are disabled. The keys are Mod+X, Mod+C,
Mod+V, Delete or Backspace, and Mod+Shift+E; the project window keeps its own commands. Engine refusals and job errors
appear in the Process section. Process controls stay disabled while a command or its job is pending. These UI key
commands use `sampleEditor.cut`, `sampleEditor.copy`, `sampleEditor.paste`, `sampleEditor.delete` and
`sampleEditor.insertSilence`, so their assignments are separate from the project window's edit commands.

When the engine requires a shared-copy choice, a modal question offers New Version, Continue (all copies), and Cancel.
Copy never asks. The question keeps the event and range that requested it, and closes without editing if the project or
editor selection changes. Escape cancels too. The engine checks all users, including inactive versions and sampler
instruments, before asking for the choice.

The Draw tool uses `draw`, with a zero-based `channel` and `samples`: one value from -1 to 1 for each frame in the
range, at most 65536. It replaces only that channel, preserving clip timing, regions, fades and the other channels. It
uses the same shared-copy choice, cancellable job and undo as the other edits. Draw requires a clip without active
warping or definition-grid edits.

## In the UI

`ui::SampleEditor` (`src/ui/sampleeditor.h`) fills the sample editor window (`resources/ui/sample-editor.rml`), which
`app::AppUi` opens and closes as the view does, whichever client opens it; closing the window closes the view. The
window opens with a double click on an audio event in the project window (outside a crossfade, which opens the
crossfade editor), and with Open/Close Editor (Mod+E, Return) when audio events but no MIDI parts are selected: the
first of them. Without either selected, Open/Close Editor closes it.

```
.sample-editor[tool=range|draw|zoom|play|scrub]   .empty without an event; .no-info, .no-overview, .no-regions
  .toolbar.sample-editor-toolbar
    button.se-tool[se-tool]       the tools; .checked for the current one
    #se-zero-crossing             Snap to Zero Crossing (the project's snap setting); .checked while on
    #toolbar-quantize             the quantize functions (p. 367) on the event it edits: ui::QuantizePanel's toolbar
                                  section, as in the project window ([audio-quantize.md](audio-quantize.md))
    button.se-layout[layout]      Set up Window Layout: info, overview, regions; .checked while shown
  .info-line.se-info              [field=format|length|selectionStart|selectionEnd|selectionLength|zoom]
  .se-overview                    waveform.se-overview-wave of the whole clip, .se-overview-event,
                                  .se-overview-selection, .se-overview-snap and .se-overview-view (what the display shows)
  .se-body
    .se-inspector                 Range: button.se-select[select], #se-set-locators, #se-locate, #se-loop;
                                  Process: #se-basic-commands, #se-process-message, #se-range-region
    .se-main
      .se-canvas                  .se-timeline with #se-level-menu and ruler.se-ruler; .se-wave-area with .se-level
        .se-display               waveform.se-wave, .se-outside (.se-before, .se-after the event), .se-region-shown,
                                  .se-selection (.off without one), .se-snap-point
      .se-regions                 #se-add-region, #se-remove-region, #se-select-region, #se-play-region and
                                  .se-region-list with a .se-region[region] per region (.selected): .se-region-name,
                                  -start and -end [field], and input.se-region-field while one is typed in
```

The display's time axis is the clip's frames: `start` and `scale` (frames per dp) on the waveform, as for every canvas
element ([canvas.md](canvas.md)). It first shows the whole clip, as Zoom Full does with clip-based zoom (p. 623).
Waveforms come from `media.peaks` ([peaks.md](peaks.md)), one question for each of the two in flight.

| Tool | Gesture | Reference |
|---|---|---|
| Range Selection | A drag selects a range, with Snap to Zero Crossing at zero crossings; Shift and a click moves its nearer end; a click deselects | p. 635 |
| Draw | At one sample per pixel or closer, draws replacement samples in the pressed channel, interpolating between pointer positions; release applies one edit and selects the corrected range | pp. 634-635 |
| Zoom | A click zooms in around the frame clicked, with Alt out, at most to the whole clip | p. 630 |
| Play | Auditions the clip with its fades from the frame pressed until the release, also outside the event and past its end (`audition.start` with the `clip`, [audition.md](audition.md)) | p. 630 |
| Scrub | A drag scrubs the clip (`audition.scrub` with the `clip`) and moves the project cursor to where the frame plays | p. 634 |

While drawing, `curve.se-draw-stroke` overlays the collected samples in the pressed channel's waveform lane, following
horizontal scroll and zoom and vertical waveform zoom. It draws only between the stroke's endpoints, with a handle for
a single sample. The overlay remains while the shared-copy choice or processing job is pending and disappears when the
edit is refused, cancelled or finished, or the editor switches to another event. Its color and line width belong to the
themes.

- **Snap point** (pp. 626-628): a line at the event's snap point, at its start without one of its own; a drag with any
  tool sets it within the event (`event.setSnapPoint`).
- **Info line** (p. 614): the clip's sample rate and channels, its length, the selection range and the zoom in samples
  per pixel. Positions are sample frames from the clip's start.
- **Overview** (p. 615): a click or drag shows that part of the clip in the display.
- **Range section** (pp. 618-619): Select All, Select None, Select in Loop and Select Event; Set Locators to Selection
  Range, Locate Selection and Loop Selection, of the selection range or else of the event. Locate Selection goes to
  its start, and from there to its end; Loop Selection sets the locators to it, turns cycle on and plays from its
  start.
- **Process section** (pp. 619-620): Basic Commands (Cut, Copy, Paste, Delete, Insert Silence), Event or Range as
  Region. Detect Silence, Spectrum Analyzer and Statistics open separate windows on the captured range, or the event
  without one ([audio-functions.md](audio-functions.md)).
- **Layout and axes** (pp. 621-622): the ruler stays above the waveform and follows its horizontal zoom and scroll,
  using the project's time format, tempo, sample rate and the event's clip-to-project mapping. The level scale beside
  each channel shows percentages or dB, chosen from its Level menu, and follows vertical waveform zoom. Its labels use
  the design system's `text-small` class, whose font size belongs to the theme. The regions list occupies the zone to
  the right of the waveform. Show Half Level Axis in the Level menu toggles guides halfway between each channel's center
  and its top and bottom. The bottom sliders scroll and zoom horizontally; the vertical slider zooms in when dragged
  down. The controls follow the zoom menu and overview.
- **Zoom menu** (p. 623): Zoom In/Out, Full, Selection, Selection (Horiz.), Event, vertical zoom and waveform vertical
  zoom, Undo/Redo Zoom. Zoom In and Out center the display on the project cursor, mapped to clip frames and clamped to
  the clip. Full and Selection show the whole clip, as clip-based zoom does without pitch analysis; Selection (Horiz.)
  fits the range and Event fits the event. Vertical zoom magnifies the waveform up to 64 times, independently of the
  overview. Zoom history belongs to the current editor event and adds no project undo steps.
- **Hitpoints section** (pp. 636-647): detecting, filtering and editing hitpoints and making slices and more of them,
  in [hitpoints.md](hitpoints.md).
- **Warp section** (pp. 648-656): Musical Mode, the audio tempo, Auto Adjust, Free Warp and Reset, and Flatten
  in the Process section, in [warp.md](warp.md).
- **Regions list** (pp. 624-626): Add Region makes one of the selection range or else of the event, Remove Region
  removes the one selected in the list, Select Region selects its section and zooms to it, Play Region auditions it
  (`audition.start` up to its end). A click on a region selects it in the list and shows it in the display, scrolled to
  it at the same zoom; the display marks it. The rows are built again only when other regions are listed or their order
  changes, so a double click stays one when its first press takes what was typed in another row. A double click on its
  name, start or end types a new one in a field: Enter or leaving the field takes it (`clip.setRegion`), Escape doesn't,
  and what isn't a whole number or a section of the clip changes nothing, nor does the number shown written another way.
  When the regions change meanwhile, such as by an undo, what was typed goes. The selected region has start and end
  handles in the waveform; dragging either sends one `clip.setRegion` edit on release, bounded by the clip and the other
  edge. A click without dragging leaves it unchanged.

`resources/styles/sampleeditor.rcss` lays it out; the themes color it (`tools/skin/generate.py`).

## Not yet

The editor in the lower zone and its inspector in the left zone; Keep Editor Contents and following the project window's
selection; several events at once (Clip Editing Mode, Clip Display Mode, Activate Clip for Editing); the grid and
snapping to it; Solo Editor, Audition, auto-scroll, the independent track loop and the toolbar's context menu; positions
in the project's time format on the info line, and typing them; the Draw tool's volume curve; dragging regions into the
project window; the clip's snap point when opened from the pool. Hooks for other tickets: offline processing (#141) and
making a sampler track of the range; what warping doesn't have yet is in [warp.md](warp.md).

## Checks

- `tests/sampleeditor_tests.cpp`: Process Cut, Copy, Paste, Delete and Insert Silence with definition edits and warp
  markers, musical mode on and off, surviving beat numbers, moved source and target anchors, insertion boundaries,
  removed grid starts and empty clips; musical timing and event fades through an edited grid, source-frame clip fades,
  warped crossfades bounded by played fade room and undo/redo.
- `tests/sampleeditor_tests.cpp`: Draw replacing one stereo channel with exact samples, unchanged clip regions and event
  timing, source preservation, undo/redo and invalid channel or sample count; Paste resampling and mono/stereo
  conversion in both directions, inserted lengths, unchanged surrounding audio and undo/redo.
- `tests/sampleeditor_ui_tests.cpp`: Draw refusing coarse zoom, drawing and selecting corrected samples with undo; half
  level guides toggled through the Level menu and both zoom sliders and horizontal scroll following the waveform;
  cancelling a held Draw job preserving the selection; a stroke visible during a held drag in the second stereo
  channel at vertical zoom, its frames and values, rendered pixels, and retention until a job succeeds, fails or is
  cancelled.

- `tests/uiapp_tests.cpp`: project and sample-editor Delete/Backspace assignments stay separate;
  `tests/theme_tests.cpp`: all shared style sheets leave new font sizes to the themes.

- `tests/sampleeditor_ui_tests.cpp`: each Basic Commands operation from its menu and keys, the clip's resulting length
  and undo/redo; Copy without undo, unshared edits preserving the clip, the shared-copy question's New Version and
  Continue choices, Cancel, Escape, closing the editor and project or selection changes while it is open; ruler
  alignment and zoom mapping, percentage and dB levels, region start/end drags and undo, Zoom menu fits and zoom
  undo/redo, Zoom In and Out centering on the cursor with a different file sample rate and cursors outside both clip
  edges, vertical zoom changing the waveform's pixels and restoring them.

- `tests/sampleeditor_tests.cpp`: Process samples and lengths for Cut, Delete, Paste and Insert Silence, Copy and
  clipboard replacement, shared-copy choices, regions and fades following edits, stereo samples, undo/redo, errors,
  cancellation, stale jobs and file cleanup; an unshared Cut keeping clip and region ids and the clip count, and sampler
  tracks and drum pads requiring the shared-copy choice.
- `tests/sampleeditor_tests.cpp`: the view as view state with its event and selection, Select All, None, Event and in
  Loop, a range of its own and its errors, an event that goes and comes back with undo, opening and closing, another
  event dropping the selection, a new project closing it; zero crossings of a sine; regions added, sorted, changed
  and removed with undo, their errors, names that stay unique after a removal, saved with the project, and those of
  a hand-edited file that aren't sections of the clip left out on load.
- `tests/sampleeditor_ui_tests.cpp` drives the app with `ui::Automation`: Open/Close Editor opening the selected event,
  the info line, the event's edges in the display and the overview, both waveforms' columns, closing the window; the
  range tool's drag, Shift and a click, Snap to Zero Crossing, the Select menu, Set Locators to Selection Range and
  Locate Selection to the start and the end, Loop Selection; Event or Range as Region, Add Region, the list, Select
  Region and Remove Region with undo, a click on a region scrolling the display to it, typing a region's name, start
  and end, by Enter and by leaving the field, and Escape, wrong and unchanged values and regions changing meanwhile,
  a double click on another region right after typing; dragging the snap point, the zoom tool in and out, a click in
  the overview, the play and scrub tools auditioning the clip past the event's end, `audition.changed` naming the clip
  and its media file, the cursor following the scrub, Play Region and the window layout's switches; Open/Close
  Editor without a selection closing it.
- `tests/playback_tests.cpp`: auditioning and scrubbing a media file and a clip with its fades
  ([audition.md](audition.md)).
- `tests/eventediting_tests.cpp`: a double click on an audio event opens it in the sample editor.
