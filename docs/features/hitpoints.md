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

## In the UI

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
| A click between two | Play back Slice: plays the slice, from the hitpoint that counts before to the one after |

`resources/styles/sampleeditor.rcss` lays them out; the themes color them (`tools/skin/generate.py`).

## Not yet

Automatic detection when audio is imported or recorded (the Editing - Audio preference, which can start the same job)
and the hitpoints of the selected event in the project window; the Beats filter and warp markers, which need the
clip's musical definition and warping (#144); Tab to the next slice; Alt+N and Alt+B to locate to hitpoints; Divide
Audio Events at Hitpoints for folder tracks; the MIDI notes' destinations First Selected Track in the UI (the engine
takes `track`), which replaces the parts of earlier conversions, and Project Clipboard; Close Gaps and Delete Overlaps
for slices. The detection is checked on synthesized loops only; recorded drum loops with hand-annotated onsets (room
sound, bleed, flams) are yet to come.

## Checks

- `tests/hitpoints_tests.cpp`: detection accuracy on two synthesized drum loops whose hits are known (straight eighths
  with a ghost note and an open hi-hat at 48 kHz; swung sixteenths with soft attacks under a noise floor, in stereo
  at 44.1 kHz): every hit found within 5 ms and nothing else, and none in silence or steady tones; measuring at each
  onset gives what the detection found; detecting as a job, cancelled, with the clip removed meanwhile and without
  its file; filtering, locking, disabling, resetting, inserting and moving hitpoints with their errors, undo and the
  project file, inserted and moved ones measured where they go; `clip.setHitpoints` and its errors; slices, events,
  regions, markers, the groove and MIDI notes.
- `tests/playback_tests.cpp`: auditioning up to an end, as Play back Slice does.
- `tests/hitpoints_ui_tests.cpp` drives the sample editor with `ui::Automation`: Edit Hitpoints showing the detected
  hitpoints at their frames, the threshold slider's drag as one undo step, Remove All; the handle, Shift, Ctrl and
  Alt, Alt and the drag, playing a slice without stopping on release; MIDI Notes with its options and Slices.
