# Transpose track and project root key

Global transposition (#168, reference manual pp. 165, 414-421): the Project Root Key, the transpose track with its
transpose events and controls, and MIDI parts and audio events that follow it or are excluded from it.

## Engine

- **Project Root Key**: `project.set` `rootKey`, `""` for none or `C`, `C#` and so on to `B`; in `project.get` and
  `project.changed`. An undo step, saved with the project.
- **A part's root key and Global Transpose**: `midiPart.set` `rootKey` (the same names) and `globalTranspose`,
  `follow` (the default) or `independent`; in the part's JSON, saved with the project, kept by splits and copies.
  `transpose.assignRootKey` (Set Root Key for Unassigned Events, p. 416) gives every part without a root key the
  project's, including the parts stashed in inactive track versions, so that activating a version shows them with it;
  refused without one. `project.get` gives each part `rootKeyTranspose` too, the step that the project root key adds
  to it (see Playback); it is derived, so the project file doesn't keep it. `event.glue` refuses MIDI parts that
  follow global transpose differently, by their root key or Global Transpose: the glued part keeps the first one's
  setting, so the others' notes would play at another pitch.
- **An audio event's root key and Global Transpose** (#1368, pp. 415-416, 421): `audioEvent.set` `rootKey` and
  `globalTranspose`, as a part's, in the event's JSON, saved with the project, kept by splits and copies; the audio file
  stays as it is. `transpose.assignRootKey` gives audio events without a root key the project's too, also those in
  inactive track versions. For the stashed parts and audio events it announces one `track.changed` for each track whose
  stashed events or parts got the root key, and no `midiPart.changed` or `audioEvent.changed`, since nothing shows them.
  `event.glue` refuses audio events that continue each other in their clip but follow global transpose differently, as
  it refuses such parts.
- **An audio event's own Transpose** (#1555, p. 420): `audioEvent.set` `transpose`, -24 to 24 semitones (the time
  stretcher's real-time range), in the event's JSON, saved with the project, kept by splits and copies. It adds to
  global transposition as a MIDI part's transpose does, also while the event is Independent, and Keep Transpose in
  Octave Range folds it with the rest. `project.get` gives each audio event `rootKeyTranspose` too, as each part.
  `event.glue` refuses audio events that transpose differently by their own Transpose.
- **The pool's Key column** (#1555, p. 417): `clip.set` `rootKey` gives every audio event that plays the clip that
  root key, also those in inactive track versions; `pool.get` gives each clip `rootKey`, the root key its audio events
  share (`""` when they have none or differ), `rootKeysDiffer` (they differ) and `audioEvents`, how many play it
  (#1644). The audio file stays as it is. `clip.set` refuses a `rootKey` for a clip that no audio event plays, which
  has nowhere to keep it (#1719).
- **Audio parts** (#1555, p. 417) have no root key or Global Transpose of their own: their events play by theirs.
  The info line shows those of a selected audio part's first event by position, or an empty value where its events
  differ (#1644), and sets them on all its events.
- **The transpose track** (`TrackType::Transpose`, `"transpose"`) has no channel, stays at the top level and is there
  at most once. Its events and controls are `Project::transpose`; removing the track removes them. Switching its time
  base and changing the sample rate keep the events where they are, as for markers.
- **Transpose events**: a position in the track's time base and a value from -24 to 24 semitones, in timeline order,
  at most one per position. Each plays from its position to the next one; before the first one nothing is
  transposed. `transpose.add` (default: the cursor, value 0; adds the transpose track if there is none),
  `transpose.set` (moves or changes events) and `transpose.remove` are undo steps; `transpose.changed` announces
  every change with what `transpose.get` returns, undo and redo included. Delete Time (`range.delete` `time`) and
  Insert Silence (`range.insertSilence`) move the events with the music, over the whole project or the tracks they
  name, unless the transpose track is locked; Delete Time drops the events in its range, and the transposition in
  force at the range's end goes on from the cut with an event there (#1367), as Process Bars carries the signature in
  force, so that the music after the range keeps its pitch.
- **Controls** (`transpose.setTrack`, an undo step): `mute` (Mute Transpose Events: the events don't play),
  `keepInOctave` (Keep Transpose in Octave Range) and `lock` (the events can't be added, changed or removed).
- **Recording** (p. 420): a recorded part or audio event follows global transpose with the project root key, or, if
  the project has transpose events, is independent without a root key. So is an event that render in place adds,
  as the render already holds the transposition.

## Playback

`globalTranspose` (`document.h`) is the transposition a part plays its notes with at a position: an independent part's
own transpose as it is; a part that follows global transposition plays its own transpose plus the step from its root key
to the project root key the nearer way (-5 to 6 semitones, nothing unless both have one), plus the value of the
transpose event playing there unless the track is muted. With Keep Transpose in Octave Range the whole sum stays within
7 semitones either way (7 up plays 7 up, 8 up plays 4 down): the option keeps even a part's own transpose in the range,
which the reference manual's p. 421 recommends it for. So with the option on, a following part's own transpose of an
octave or more is folded away (12 plays 0); a part that should keep such a transpose has to be Independent. The render
snapshot adds it to each note's MIDI modifiers' transpose at the note's start, with the track's modifiers, before their
ranges.

An audio event plays the same sum with its own Transpose (#1368, #1555), shifted in real time by the time stretcher
([time-stretch.md](../time-stretch.md)) on the disk thread: the render snapshot gives an event that some of it shifts a
media entry of its own, as a warped event has ([warp.md](warp.md)), with a pitch map (`RenderMedia::pitch`): the
semitones from the event's start and from each transpose event within it, in the event's frames. `dsp::WarpReader` plays
it through the warp map of a warped event or an identity map of a plain one, with the clip's warp preset, clamped to the
real-time range of 24 semitones. The stretcher's latency depends on the pitch, so the reader starts over at each point
with the pitch from there on, as [warp.md](warp.md) says, and crossfades the run it ends into the new one over 5 ms, so
that sustained audio doesn't click there (#1564). An event that nothing can shift, without a Transpose of its own and
Independent or without a root key step and transpose events, gets no warp or pitch map at all (#1555), so a project
without global transposition pays nothing for it; it plays its file as before.

What following the root key costs, `gnubase-bench --case transposedEvents` ([performance.md](performance.md)): 16 and
64 mono audio events in C on tracks of their own, played as they are and in a project in D, each render in a process of
its own. One Release run on Windows with 32 hardware threads, on a machine that other builds kept busy (#1644): as they
are 2.8 times faster than real time for 16 and 64 events, following the root key 0.99 times for 16 and 0.33 times for
64, and 2.4 and 2.3 MB more per shifted event. A project that shifts many events at once is bound by their stretchers
on the disk threads, which offline rendering waits for; a measurement on an idle machine, and cheaper stretchers if it
holds, are #1735.

## UI

- **The toolbar** has the Project Root Key (`#root-key` with `#root-key-menu`), which the event display shows and
  sets.
- **The info line** has a MIDI part's and an audio event's Root Key and Global Transpose, and those of a selected
  audio part's events ([inspector.md](inspector.md)). Its Transpose field, a MIDI part's or an audio event's own
  (-24 to 24 for audio events), shows what the project root key adds in brackets after it (p. 420), such as `(+2)`
  for a part in C in a project in D, from `rootKeyTranspose`. Its Global Transpose button reads the value it holds
  (p. 421): Follow, or Independent while it is checked; its title says so too, and only "Global Transpose" while an
  audio part's events differ (#1719). The values it shows aren't edits: changes that its lists send while it shows
  them are left out, so that the first selected item's key doesn't reach the others (#1719).
- **Independent parts and audio events** show an icon (`part-independent.svg`) in their upper right corner in the
  event display (p. 421): a `.part-mark.independent` with attribute `part` in the display's `.part-marks`. An audio
  part shows it when all its events are Independent (#1644).
- **The transpose track's lane** (`ui::GlobalEventLane`, `src/ui/transposetrack.h`, shared with the chord track's
  lanes) shows each event's value from its position: the draw tool adds an event at the nearest bar and opens its
  value for typing, a double click opens it too, a drag moves it to the nearest bar, following the mouse while the
  button is down, and the erase tool removes it. The lanes read the project the event display reads, once per answer,
  and the track's own `transpose.changed`, `chord.changed` or `arranger.changed`, which carry its events, instead of a
  `project.get` of their own; an edit the engine refuses shows its reason in `#edit-refused`.
- **The track list** has the transpose track's controls by default (`ui.trackControls` `transpose`): Mute Transpose
  Events (`transposeMute`), Keep Transpose in Octave Range (`keepInOctave`), Switch Time Base and Lock
  (`transposeLock`).
- **Indicate Transpositions** in the key editor's and the in-place editor's toolbar (#1766, pp. 419, 1260) shows the
  notes at the pitches `globalTranspose` gives them, from the editor view's `transpositions`
  ([key-editor.md](key-editor.md)).
- **Add Track** has Transpose, once per project.
- **Refused edits**: the event display shows why the engine refused an edit, such as a glue of parts that follow global
  transpose differently, in its notification `#edit-refused` (#1367, [event-display.md](event-display.md)).
- **The pool's Key column** (`select[field=rootKey]` in each clip's row, [pool.md](pool.md)) shows and sets the root
  key of the clip's audio events (`clip.set` `rootKey`): empty when they differ, where "-" clears them, and disabled
  for a clip that no audio event plays (#1644). Each list holds only the key it shows until it gets the focus. As in
  the reference, the column shows once Root Key in the pool's View/Attributes menu turns it on (p. 417, #1735).
- **The track list's context menu** has Set Root Key for Unassigned Events (`transpose.assignRootKey`, p. 416),
  enabled while the transpose track is selected, as a right click on it selects it.

## Checks

`tests/transpose_tests.cpp`: the pitches the render snapshot plays for the root key (both ways, with the part's own
transpose, Independent, undo), transpose events (adding the track, timeline order, Keep Transpose in Octave Range, mute,
moving, the value range, lock, the track's removal), the octave-range option folding the whole sum a part plays, its own
transpose included (7 staying 7 and 8 folding, a muted track, an Independent part left as it is), `transpose.changed`
and `arranger.changed` announced by undo and redo, glue refusing parts that follow global transpose differently and
keeping the sound of parts the option folds differently, Set Root Key for Unassigned Events (also in inactive track
versions), Delete Time and Insert Silence moving the events, Delete Time carrying the transposition at the range's end
to the cut and nothing when an event is at the end or none in the range, recorded parts with and without transpose
events, and saving and loading. For audio events: the pitch map of the render snapshot for the root key, transpose
events, a muted track, the octave range and Independent, with undo and splits; Set Root Key for Unassigned Events, also
in inactive track versions with the tracks it announces, and glue; recorded events with and without transpose events;
saving and loading. An audio event's own Transpose with the root key step, Independent and the octave range, its range
and `rootKeyTranspose`; no media entry of its own for an event that nothing can shift; glue refusing events with
different Transposes; the pool's Key column (`clip.set` `rootKey`) over the events of a clip, inactive track versions
included, and what `pool.get` shows when they differ and for a clip without events, whose root key `clip.set` refuses.
`tests/renderinplace_tests.cpp`: a rendered event under a transpose event Independent with an empty pitch map, and one
without transpose events in the project root key, following it, with a pitch map that a later root key shifts.
`tests/warp_tests.cpp`: `dsp::WarpReader` shifting a sine an octave up from a point of its pitch map, also after a seek,
clicks after a pitch point an octave up and down where its map puts them, read through and after a seek, no step in a
sine across a pitch point up and down or the end of a segment beyond the real-time range, and an audio event in C
playing 4 semitones up in a project in E and 8 down after a transpose event, measured in an offline render, and
unshifted when Independent. `tests/transpose_ui_tests.cpp`: the toolbar's root key, the info line's Root Key and Global
Transpose (its button reading Follow or Independent) for parts and audio events and a part's root key step, the mark of
Independent parts and audio events, the reason of a refused glue in `#edit-refused`, drawing, typing, dragging,
double-clicking and erasing transpose events on the lane, the track list's controls and its context menu's Set Root Key
for Unassigned Events, an audio event's own Transpose with the root key step in brackets, and an audio part's events'
Root Key and Global Transpose, empty when they differ (the button's title too), with the mark when all of them are
Independent; a MIDI part and an audio event in different keys selected together keeping their keys, also when the
key shown changes and after its undo.
`tests/poolpanel_tests.cpp`: the pool's Key column, its options filled once it is used, differing keys cleared with
"-", and disabled for a clip without audio events; View/Attributes' Root Key showing it.

## Not yet

Ready-made events tagged as drums or effects set to Independent (p. 421), which waits for media files to have tags (the
media browser, #196, then #1735); live
MIDI input through the transpose track; the transpose track's controls in the
track controls dialog; selecting several transpose events and the Delete key.
