# Tempo detection and tempo from MIDI

Making the tempo track follow freely played material: detecting the beats of an audio event or a MIDI part, and
calculating the tempo from MIDI notes tapped along with it (#158, reference manual pp. 1130-1131, 1459-1461). The tempo
track itself is [tempo-track.md](tempo-track.md)'s, the hitpoints that audio detection starts from
[hitpoints.md](hitpoints.md)'s.

## Beat tracking

`analysis::detectBeats` (`src/engine/analysis/tempodetect.h`) takes where sounds start (pulses: seconds and a weight)
and finds the beats, which may drift:

1. The pulses make an onset envelope at 200 frames per second, blurred by a Gaussian of 10 ms and scaled to a standard
   deviation of 1.
2. Its autocorrelation over lags of 40 to 240 BPM, weighted by a log-Gaussian of one octave around 120 BPM, gives the
   beat period, refined between frames by a parabola.
3. Dynamic programming (D. Ellis, "Beat Tracking by Dynamic Programming", 2007) finds the beats: each frame scores its
   envelope plus the best score of a beat half a period to two periods before, less 100 times the squared log of how
   far that gap is from the period. The beats run back from the best frame in the last period.
4. A beat within an eighth of a beat of a pulse moves onto it, so a steady performance gives exact tempos; the beats
   before the first such beat and after the last are left out.

`rescaleBeats` makes beats a factor as fast (a beat between each two for 2, every other one for 0.5, 4/3 and 3/4 by
interpolating between them) and moves them by half a beat (offbeat); `smoothBeats` puts them on the straight line that
fits them best, one steady tempo. Everything runs on the command thread.

## Engine

- **`tempo.detect`** (Tempo Detection, pp. 1460-1461) takes an audio event or a MIDI part (`event`) of 7 seconds or
  more. An audio event's pulses are the onsets of its clip's section (`detectHitpoints`, the hitpoint detection), weighed
  by their peaks; a MIDI part's are its notes, weighed by velocity. First, every musical track that holds events, parts,
  markers or automation becomes linear (`track.set`), so that nothing moves in time while the tempo changes, as the
  reference manual says, and the tempo events inside the event, but the first, go: what an earlier detection made. Then
  the beats, a quarter apart, go on the tempo track (below). `factor` (Multiply by 2, Divide by 2, Multiply by 4/3 and
  by 3/4), `offbeat` (Offbeat Correction) and `smooth` (Smooth Tempo) change the beats first; `reset` (Reset the
  analysis result) only removes the tempo events inside the event. Each call is one undo step, "Detect tempo".
- **`tempo.fromMidi`** (Calculate Tempo from MIDI Events, pp. 1130, 1459-1460) takes MIDI parts (`parts`) and the selected
  notes among them (`notes`, all without), `distance`, the Distance of Notes in ticks (a quarter by default), and
  `beginAtBarStart`. Notes that start within 30 ms of each other are one tap, such as a chord. One undo step, "Calculate
  tempo from MIDI". Notes on linear tracks keep their time, those on musical tracks follow the new tempo: the reference
  manual records the taps on a linear track (p. 1459).
- **Onto the tempo track** (`followBeats` in `tempo.cpp`): the first beat keeps its position under the tempo playing,
  and a step tempo event at each beat but the last, `distance` after the one before with the tempo that reaches the next
  beat in time, replaces the tempo events from the first beat to the last; the last tempo holds after it. Beats before
  the project start don't count. With `beginAtBarStart` the first beat moves to the nearest bar start after the project
  start, and a tempo event at the start of the bar before puts the bar start at the beat's time (an error if that tempo
  would be outside 1-999 BPM). Both commands make the tempo track active, whose events then come from the tempo the
  project played (a fixed tempo's one, in fixed tempo mode, as `tempo.import` replaces them), and report the tempo as
  `tempo.changed` does.

## UI

- **Project > Tempo Track > Tempo Detection...** (`tempo.detection`) opens `resources/ui/tempo-detection.rml` for the
  first selected audio event, or else MIDI part, in the project window: `#tempo-detection-analyze` detects afresh,
  `#tempo-detection-double`, `-halve`, `-four-thirds` and `-three-quarters` multiply the factor, `-offbeat` switches
  Offbeat Correction and `-smooth` Smooth Tempo on, each sending `tempo.detect` again with them
  (`EventDisplay::detectTempo`); `#tempo-detection-reset` resets. Close or Escape closes it.
- **MIDI > Functions > Calculate Tempo from MIDI Events...** (`midi.calculateTempo`, a new MIDI menu) opens
  `resources/ui/calculate-tempo.rml` for the selected MIDI parts: `select#calculate-tempo-distance` (whole to sixteenth
  notes, triplets and dotted values, a quarter first) and `#calculate-tempo-bar-start`; OK sends `tempo.fromMidi`.

## Not yet

The Tempo Detection panel as a panel that stays open, its event name and the direction of reanalysis buttons for
correcting the curve by hand, time signature events from the analysis and resetting the tempo the first event had
before (Edit > Undo does, as the reference manual recommends), detecting the tempo of a range or a whole project,
Set Tempo from Event (p. 1457, which needs the musical definition of clips, #144), the selected notes of a MIDI editor in
Calculate Tempo from MIDI Events (the engine takes `notes`), and ramps between the calculated tempo events.

## Tests

`tests/tempodetection_tests.cpp`: detection accuracy on material whose beats are known: pulses at a steady 97 BPM with
quieter offbeats, drifting from 100 to 125 BPM and at 120 BPM played up to 8 ms off with rests (every beat found, on the
pulses or within 20 ms); a synthesized drum loop at 104 BPM, 48 kHz (every tempo event within 0.5 BPM, the kicks on the
beats within 5 ms) and one speeding up from 92 to 108 BPM after half a second of silence (every beat within 5 ms);
rescaling and smoothing; the factors, offbeat correction, smoothing, detecting again, reset and undo, tracks becoming
linear, MIDI parts and parts shorter than 7 seconds; `tempo.fromMidi` with quarters and eighths, selected notes,
Begin at Bar Start and its errors, notes keeping their time. `tests/tempotrack_ui_tests.cpp` drives both dialogs in the
app.
