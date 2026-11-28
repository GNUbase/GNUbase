# Hitpoints and slices

Where sounds start in a clip (#143, reference manual pp. 636-647): detecting them, filtering and editing them in the
[sample editor](sample-editor.md), and making slices, events, regions, markers, a groove and MIDI notes of them.

## Detection

`clip.detectHitpoints` runs as a job (`job.progress`, `job.cancel`, [architecture](../architecture.md), "Jobs"),
since a long recording takes seconds: `analysis::OnsetDetector` (`src/engine/analysis/onsets.h`) reads the clip's
file in blocks on the job's thread, and the job then sets the hitpoints with `clip.setHitpoints`, one undo step. A
clip removed meanwhile, another project opened or the clip's file relinked fails the job.

Each channel's peak level is followed by two envelopes, one of the samples and one of their differences (the high
frequencies, so that a hi-hat stands out over a kick drum's decay), which jump up with the signal and fall with a
50 ms time constant. Where either rises by 6 dB or more within 5 ms, from at least -60 dBFS, a sound starts: at the
first frame there more than 1 dB above where the envelope was. The next one can start 20 ms later. Each onset has its
peak, the loudest sample in the 20 ms from there, and its rise; the intensity is the rise relative to the clip's
sharpest.

A hitpoint inserted or moved by hand is measured the same way where it goes (`OnsetDetector::measure`), from half a
second of the file before it, by when anything earlier has decayed below -60 dBFS, so that it gets the peak and rise
the detection would have given it there. Its intensity is relative to the sharpest the detection found: the rise at
the clip's other hitpoint of the highest intensity, preferring detected (unlocked) ones, measured again and divided by
that intensity. So a hit the detection missed and the user added gets a MIDI note's velocity as loud as it is, and the
filters treat it as a detected one once it is unlocked. Inserting or moving one fails if the file can't be read.

## In the engine

The hitpoints are in the project: a clip's `hitpoints`, by position, each with its `position` (clip frames), `peak`
(dBFS), `intensity` (0-1), `locked` and `disabled`, and its `hitpointFilter`: `threshold` (-60 to 0 dBFS),
`intensity` (0-1) and `minimumLength` (clip frames). A hitpoint counts (`active`) when it isn't disabled and is locked
or passes the filters: its peak and intensity reach theirs, and it starts the minimum length or more after the one
before that counts. Clients get `active` with each hitpoint; the project file has them in `gnubase.json`, which
DAWproject has no place for. Each command is one undo step and emits `clip.changed`.

| Command | What it does | Reference |
|---|---|---|
| `clip.detectHitpoints` | Detects the clip's hitpoints, in place of those it has then, as a job | p. 637 |
| `clip.setHitpoints` | Sets a clip's hitpoints as given: what the detection found, or hitpoints from elsewhere | |
| `clip.setHitpointFilter` | Threshold, Intensity and Minimum Length | p. 638 |
| `clip.addHitpoint` | Inserts one, locked, measured where it is | p. 640 |
| `clip.setHitpoint` | Moves (and so locks and measures) one, locks, disables or resets it | p. 640 |
| `clip.removeHitpoints` | Remove All | p. 638 |
| `event.fromHitpoints` | The Create section, with `create`: | pp. 638-647 |

`event.fromHitpoints` uses the hitpoints that count inside the section the event plays, and runs the commands that
make such things as part of it (`ProjectModel::runEdit`), so that it is one undo step with their events:

- `slices` cuts every unlocked event of the clip at them (`event.split`) and puts the slices into an audio part in its
  place (`event.toPart`, [audio-parts.md](audio-parts.md)); slices of an event in a part stay there. Slices move with
  the project tempo on a musical track.
- `events` cuts this event at them.
- `regions` makes regions of the clip ([sample-editor.md](sample-editor.md)) from the event's start to the first, from
  each to the next and from the last to the event's end.
- `markers` adds a position marker at each to the active marker track, adding one without ([markers.md](markers.md)).
- `groove` makes the project's quantize settings a groove of them, as long as the event and starting where it does
  (`quantize.set`, [quantize.md](quantize.md)); Save Preset keeps it.
- `midiNotes` adds a MIDI part as long as the event with a note at each, to `track` or a new MIDI track named after the
  clip: Convert Hitpoints to MIDI Notes' `velocityMode` (`dynamic`: the loudest hitpoint velocity 1, the others less as
  their peaks are quieter; `fixed`: `velocity`), `pitch` (default 36) and `length` (ticks, default a sixteenth).

`runEdit` validates the nested command's parameters and result against its registered schemas and refuses commands that
are not document edits. It synchronizes shared MIDI parts before returning, so another nested edit sees the updated
copies. The outer command keeps the combined events and undo step and rolls back the project on failure.

`event.deleteOverlaps` (reference manual p. 643) trims each selected audio event's end to the next selected start in the
same track, part and lane. It preserves clip offsets and delegates trimming to `event.resize`, including its fade and
warp handling. Equal starts keep the highest id and remove the others. The last event and unselected events stay as they
are; locked events or parts fail the whole edit. It returns trimmed `events` and `removed` ids in one undo step.
Automatic track fades still apply; this command creates no crossfades. It is available through the engine, MCP and Lua.

`event.closeGaps` implements Close Gaps (Crossfade), reference manual p. 643. Within each track, part and lane it
extends each selected slice's end through the gap into the next selected slice, then runs `crossfade.set`. Starts and
clip offsets stay in place. `length` is the crossfade in milliseconds (default 10, from 0.1 to 500), limited to half the
next slice's played length. Touching slices get crossfades too; existing overlaps and the last slice stay as they are.
Timing follows the clips' warps through `event.resize`. Locked events or parts, frozen tracks and insufficient source
audio fail the whole edit, including earlier pairs. It returns changed `events` in one undo step through the engine, MCP
and Lua.

`event.closeGaps` also accepts `mode: "timeStretch"` for Close Gaps (Time Stretch). Each selected slice with a gap is
rendered from its current audio, including its warp and clip fades, then stretched offline without shifting pitch to end
at the next selected start. The result is a new clip named after the source with Flattened appended and a float WAV in
the project media folder, or the recording folder for an unsaved project. Other events of the source clip stay
unchanged. The stretch ratio must be from 0.1 to 10; touching and overlapping slices stay unchanged. Version 2 returns a
cancellable job in time-stretch mode, with progress and the changed `events` in its result. It reads, stretches and
writes the captured slice audio on the job thread, checking cancellation between blocks, including both stretch passes.
The completion command `event.applyClosedGaps` accepts only engine-posted work and applies all renders in one undo step
at the captured project revision. An edit, undo or project replacement meanwhile fails the job. Time-stretch mode cannot
run inside a history transaction; crossfade mode remains a synchronous edit. Failed or cancelled jobs remove their
files, including after a late render-lock refusal; undo keeps successful files for redo. Undo before changing tempo
again, as the reference manual, p. 643 recommends. Moving to the rendered clip clears the event's slice and quantize
origins from the old clip, so Reset Slices cannot restore an offset or length from that source onto the new clip. Undo
restores those origins along with the original clip.

## In the UI

Audio > Hitpoints > Divide Audio Events at Hitpoints is enabled for one selected folder with Group Editing on (reference
manual p. 642). The UI command `audio.divideAtHitpoints` sends `track.divideAtHitpoints`, which gathers audio events in
that folder and its nested folders and delegates to `quantize.slice`: equal track priorities, a 20 ms merge range and
zero slice offset. Every enclosed track is cut at the same project positions in one undo step. Locked events stay whole,
and MIDI parts and tracks outside the folder stay unchanged. Engine refusals appear in the project window's edit-refused
notification. The engine command returns `events` and `positions` through MCP and Lua too.

Audio > Advanced has Close Gaps (Crossfade), Close Gaps (Time Stretch) and Delete Overlaps, the UI commands
`audio.closeGapsCrossfade`, `audio.closeGapsTimeStretch` and `audio.deleteOverlaps`. They run `event.closeGaps` with the
corresponding mode or `event.deleteOverlaps` on selected audio events and all events inside audio parts selected in the
project window, filtering out selected MIDI parts. Without selected audio events or nonempty audio parts they are
disabled. Each edit is one undo step; the selection range does not replace the event selection. The project window
watches the time-stretch job and shows a failed job's engine error in the edit-refused notification, including a ratio
outside 0.1 to 10 or a project changed during rendering. It also checks jobs that finished before their command's result
reached the UI. While a job started in the project window runs, the shared Rendering Audio dialog shows its progress and
detail, titled Close Gaps (Time Stretch), and asks the user to wait before editing. Cancel or closing the dialog sends
`job.cancel`; the job removes its rendered files. Completion, cancellation and failure close the display. Jobs started
by other clients do not open it. Once a `job.progress` has come, the dialog's progress no longer goes back to the older
answer of its `job.get` check (#2172).

The sample editor's inspector has the Hitpoints section, and its display shows the hitpoints that count and the
disabled ones.

```
.sample-editor.edit-hitpoints          while Edit Hitpoints is on
  .se-inspector                        #se-edit-hitpoints (.checked); slider#se-threshold, #se-intensity and
                                       #se-minimum-length (milliseconds) in .se-filter rows; #se-remove-hitpoints;
                                       button.se-create[create] per Create button; .se-midi-options (.off until
                                       MIDI Notes): button.se-velocity-mode[velocity-mode] (.checked),
                                       valuefield#se-midi-velocity (1-127), #se-midi-pitch, #se-midi-length
                                       (sixteenths), #se-midi-ok, #se-midi-cancel
  .se-display .se-hitpoints            .se-hitpoint[hitpoint] per hitpoint (.locked, .disabled) with its
                                       .se-hitpoint-line and .se-hitpoint-handle at the top
```

Edit Hitpoints detects the hitpoints of a clip that has none. A filter's drag is one undo step. While it is on, the
display's gestures edit hitpoints instead of the tool's (pp. 636, 639-640):

| Gesture | What it does |
|---|---|
| A click on a hitpoint's handle | Lock Hitpoint, or unlocks it |
| Shift and a click on its line | Disable Hitpoint, or enables it |
| Ctrl (Cmd on macOS), Alt and a click on its line | Enable/Unlock Hitpoints: resets it |
| A drag of its line | Move Hitpoint, which locks it |
| Alt and a click elsewhere | Insert Hitpoint |
| A click between two | Play back Slice: plays the slice of the clip, as the play tool does (`audition.start` with the `clip`), from the hitpoint that counts before to the one after |

`resources/styles/sampleeditor.rcss` lays them out; the themes color them (`tools/skin/generate.py`).

## Not yet

Automatic detection when audio is imported or recorded (the Editing - Audio preference, which can start the same job)
and the hitpoints of the selected event in the project window; the Beats filter and warp markers, which need the clip's
musical definition and warping (#144); Tab to the next slice; Alt+N and Alt+B to locate to hitpoints; the MIDI notes'
destinations First Selected Track in the UI (the engine takes `track`), which replaces the parts of earlier conversions,
and Project Clipboard. The detection is checked on synthesized loops only; recorded drum loops with hand-annotated
onsets (room sound, bleed, flams) are yet to come.

## Checks

- `tests/hitpoints_tests.cpp`: folder division includes nested audio, preserves outside tracks, rejects non-folders and
  folders without Group Editing, and supports undo/redo. `tests/mainmenu_tests.cpp` drives its Audio > Hitpoints entry,
  disabled without Group Editing, and undoes the split. A folder without hitpoints shows the engine's refusal in
  `#edit-refused` and keeps its events unchanged.

- `tests/mainmenu_tests.cpp`: a time-stretch job needing a ratio above 10 shows its error in `#edit-refused`, both after
  its command's result and when it finished before the UI handled that result; the project and undo history stay
  unchanged.

- `tests/mainmenu_tests.cpp`: Close Gaps jobs held after rendering one slice show nonzero progress and detail. Cancel
  sends `job.cancel` without changing the project or history and removes that rendered file. Completion closes the
  display with one undoable edit, and failed jobs close it without opening a second error dialog.

- `tests/mainmenu_tests.cpp`: Close Gaps progress doesn't go back to what an older `job.get` answer tells. The test uses
  `Engine::setCommandRanHook` to let the job go on between `job.get`'s read and its answer, so a newer `job.progress`
  reaches the UI before that answer every time (#2172).

- `tests/hitpoints_tests.cpp`: held time-stretch jobs keep commands responsive, cancel without files, reject edits, undo
  and project replacement, and refuse transactions. Flatten clears slice and quantize origins with undo/redo and removes
  its renders after an outer transaction failure or a late render-lock refusal. `tests/timestretch_tests.cpp` checks
  cancellation between offline stretch blocks.

- `tests/mainmenu_tests.cpp`: all three Audio > Advanced commands on a selected part created by
  `event.fromHitpoints`, with unselected audio preserved and undo/redo.
- `tests/hitpoints_tests.cpp`: quantize slicing, time-stretch gap closing and Reset Slices keep every event inside its
  clip; rendered events discard all three old-clip origins, and undo/redo restore the recorded states.

- `tests/mainmenu_tests.cpp`: all three Audio > Advanced slice entries, disabled with no selection or MIDI parts alone,
  enabled for selected audio events; crossfade, time stretch and overlap results, untouched unselected audio and MIDI,
  one undo step and undo/redo.

- `tests/hitpoints_tests.cpp`: `runEdit` refusing unknown edits, invalid parameters and malformed results, and shared
  parts synchronized before the caller continues.

- `tests/hitpoints_tests.cpp`: detection accuracy on two synthesized drum loops whose hits are known (straight eighths
  with a ghost note and an open hi-hat at 48 kHz; swung sixteenths with soft attacks under a noise floor, in stereo at
  44.1 kHz): every hit found within 5 ms and nothing else, and none in silence or steady tones; measuring at each onset
  gives what the detection found; detecting as a job, cancelled, with another project opened or the clip removed
  meanwhile, while `test::whileHeld` holds the job at its first `job.progress`, and without its file; filtering,
  locking, disabling, resetting, inserting and moving hitpoints with their errors, undo and the project file, inserted
  and moved ones measured where they go; `clip.setHitpoints` and its errors; slices, events, regions, markers, the
  groove and MIDI notes; Delete Overlaps within a part, unchanged offsets, equal starts, part boundaries, selection
  validation, locked events and undo/redo.
- `tests/hitpoints_tests.cpp`: Close Gaps (Crossfade) preserving starts and offsets, musical warping, selection part and
  lane boundaries, duplicate and empty selections, locked events, insufficient source audio, requested crossfade length,
  its limit to half the next slice's played length, atomic rollback and undo/redo.
- `tests/hitpoints_tests.cpp`: Close Gaps (Time Stretch) preserves pitch, slice starts, source clips and the next event;
  musical warping, rendered clip naming, atomic rollback with file cleanup and undo/redo.
- `tests/playback_tests.cpp`: auditioning a clip, and up to an end, as Play back Slice does.
- `tests/hitpoints_ui_tests.cpp` drives the sample editor with `ui::Automation`: Edit Hitpoints showing the detected
  hitpoints at their frames, the threshold slider's drag as one undo step, Remove All; the handle, Shift, Ctrl and
  Alt, Alt and the drag, playing a slice without stopping on release; MIDI Notes with its options and Slices.
