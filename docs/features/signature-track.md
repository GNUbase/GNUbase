# Signature track

The signature track and its time signature events, with pickup bars (#156, reference manual pp. 163, 1468-1470). The
signature map itself, which converts between ticks and bars and beats, is the time model's (#13,
`engine/time/signaturemap.h`); the tempo next to it is the tempo track's ([tempo-track.md](tempo-track.md)).

## Engine

- **Time signature events** are the project's signature changes (`time::SignatureChange`): a bar, zero-based, and a
  signature of 1 to 99 beats of a whole, half, quarter, eighth or sixteenth note. The first is at the first bar and
  stays; the transport bar's signature (`project.set` `signature`) is its signature. `signature.add` adds one at a bar
  (or `"cursor"`: the bar the project cursor is in, Insert Time Signature Event) or changes the one there; two equal
  signatures in a row are two events. `signature.set` changes events named by their bars, all at once (`to`,
  `numerator`, `denominator`); `signature.remove` removes them. Each is an undo step that emits `signature.changed`
  with the project's signatures, as `project.get` has them; undo and redo emit `project.changed`.
- **Bars move, positions stay.** Events, parts, markers and automation keep their positions in ticks or samples, so
  a signature change moves the bars under them. The bars and beats ruler, the grid, snapping, bar-wise lengths and
  bar positions everywhere follow the signature map.
- **Pickup bars**: a short first bar, such as 1/4 at bar 1 and 4/4 from bar 2. Project Setup's bar offset of -1
  numbers it bar 0, so that the first full bar is bar 1.
- **The signature track** is a track type (`signature`): a project has one at most, without a channel, at the top
  level. It only shows the signatures, which are the project's: removing it keeps them. GNUbase's entry keeps the
  track; `project.xml` has the signatures as its time signature points, as before.

## UI

- **The signature track's lane** (`ui::SignatureLane`, `src/ui/signaturetrack.h`): a `ui::SignatureRow` over the
  signature track's lane in the event display, following the project and the view, with a `.signature-event` per
  event at the start of its bar, its signature as text. With the display's object selection tool a click on an event
  selects it (Ctrl adds or removes it) and a drag moves the selected ones to the nearest bar (`signature.set`); the
  draw tool adds an event at the nearest bar with the signature there (`signature.add`) and opens it for editing, the
  erase tool removes events but the first. A double click opens an event: a `.signature-field` over it, where Enter
  sets the signature typed, such as "7/8"; a press elsewhere in the row closes it.
- **The tempo track editor's time signature display** (pp. 1468-1469): the same row under the editor's ruler, with
  the editor's tools. The info line shows the first selected time signature event instead of a tempo event: its bar
  as the ruler numbers it and its signature, which a signature typed changes for the selected events. Backspace and
  Delete remove the selected events.
- Lengths in bars and beats, such as the info line's, count the bars from their start: two bars from the third beat
  of a 4/4 bar before a 3/4 bar end on the third beat of the bar after it.
- Project > Add Track has a Signature page. The track list leaves the signature track out of Duplicate Tracks, Move
  Selected Tracks to New Folder and drags into folders, and shows no controls for it. Remove Empty Tracks, the engine
  command `track.removeEmpty`, never removes it.

## Not yet

Click patterns and the click itself (the metronome, #101, whose golden test covers signature changes), the signature
track's Lock and options, its Inspector list (p. 183), the Insert Time Signature Event key command and dialog, the bars
in the signature track's background and signature events in the middle of a bar.

## Tests

`tests/signature_tests.cpp`: the signature track's limits, adding, changing, moving and removing events, the first
event, events at the cursor, a pickup bar with events keeping their positions, undo and saving and loading.
`tests/signaturetrack_ui_tests.cpp`: typed signatures, the bars and beats ruler across signature changes, the row on
the signature track's lane with clicks that add, edit, move and erase events, and the tempo track editor's time
signature display with its info line, draw tool and Delete. `tests/timeformat_tests.cpp`: lengths in bars across
signature changes. `tests/tracklist_tests.cpp`: the track list's commands leaving the signature track out.
