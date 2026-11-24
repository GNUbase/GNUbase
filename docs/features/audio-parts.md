# Audio parts

Containers of audio events on audio tracks (#471, reference manual p. 250): several events that the project window
treats as one. An audio part is an `AudioPart` in the project (`src/engine/document/document.h`): track, name, position
and length in the track's time base, mute, lock and color, like a MIDI part. Its events are the audio events whose
`part` is its id; they stay audio events on the same track with their own positions there, and the part decides where
they play.

## Engine commands

| Command | What it does |
|---|---|
| `audioPart.add` | An empty part on an audio track, at `position` and `length` in the track's time base |
| `event.toPart` | Events to Part: puts audio events into a new part on their track, from the first start to the last end; one part per track |
| `event.dissolvePart` | Dissolve Part: removes parts and makes their events events of their own, cut at the part's edges so that they play as they did, and muted if the part was; events the part hid entirely go |

The `event.*` commands ([parts-and-events.md](parts-and-events.md)) take audio parts like MIDI parts, by id:

- **Moving and copying** take the part's events along, also onto another audio track; a copy has copies of the events,
  and crossfades between them join the copies. An event in a part can't move to another track by itself. Shared
  copies (`shared`, reference manual p. 272) keep their events alike ([parts-and-events.md](parts-and-events.md)).
- **Resizing.** Normal sizing reveals or hides events: they keep their time. Sizing moves contents moves them with the
  edge.
- **Splitting** cuts the events across the position too; what lies after it goes into the new part. Without `events`,
  `event.split` splits parts with their events, not the events on their own. The events that move into the new part
  are announced as `audioEvent.changed`, only the pieces the split cuts off as `audioEvent.added`.
- **Gluing** audio events that continue each other in a clip, as after a split, makes one event; any other audio events
  and parts of one track become one part from the first start to the last end (reference manual p. 269), the first of
  the parts or a new one, with all their events.
- **Deleting** a part deletes its events. `event.set` mutes, locks, colors and renames parts; `event.toRealCopy` gives
  each of a part's events a clip of its own; `event.get` has `kind` `audioPart`.
- **Ranges** (`range.*`) cut parts at their edges like events and copy and paste them with their events.

## Playback

The render snapshot cuts each event of a part at the part's edges (`rendersnapshot.cpp`), and leaves out the events
of a muted part and those it hides. Where the part cuts an event, the part only hides the audio outside it: the event's
fade there plays on from where the cut leaves it (`RenderAudioEvent::cutStart` and `cutEnd`), and the event's
crossfade from an event before it rests.

Snapping to events (`snap.position`) takes the start, end and snap point of an event in a part only where the part shows
them, besides the part's own edges.

## Project file

`gnubase.json` has the parts in `audioParts` and each event's `part`. `project.xml` has a part as a clip whose content is
a `Clips` timeline of its events' clips, which count from the part's start in the lane's time unit; a muted part is a
disabled clip. Read from another application, such a clip of clips on an audio track becomes an audio part again; its
color is the part's, not its events'.

## The event display

Parts show like MIDI parts, with their events' waveforms inside, cut at the part's edges; the events themselves can't
be selected there; the audio part editor edits them inside the part ([audio-part-editor.md](audio-part-editor.md)),
which a double click on a part opens. Empty parts come from:

- the draw tool on an audio track ([tools.md](tools.md)), a drag or a click for the bar there;
- the object selection tool with the tool modifier Split Event (Alt) and a drag or click on an empty spot of an audio,
  MIDI or instrument track;
- a double click on an audio track between the locators (elsewhere on an audio track it does nothing).

The info line renames the selected parts, audio and MIDI ones alike, with `event.set`.

The Audio menu's Events to Part (`audio.eventsToPart`) and Dissolve Part (`audio.dissolvePart`) act on the selected
audio events and parts and select what they make.

## Checks

- `tests/audiopart_tests.cpp`: `audioPart.add`; Events to Part and Dissolve Part with undo, also of a muted part;
  moving, copying, splitting (with what it announces), gluing, muting, locking, removing and duplicating parts with
  their events; shared copies, an edit of one's events reaching the others and undone with them, and the copies that
  leave their group; playback cut at the part's edges and silent while muted; the DAWproject file with and without
  GNUbase's entry.
- `tests/inspector_tests.cpp`: the info line renames an audio part and a MIDI part selected with it, as one undo step.
- `tests/fade_tests.cpp`: fades that a part cuts play on up to its edges, sample by sample and against the golden file
  `fades-cut-by-part`.
- `tests/snap_tests.cpp`: an event's magnets that its part hides don't attract.
- `tests/eventediting_tests.cpp`: drawing audio parts with the draw tool and with Alt and the object selection tool, the
  double click between the locators on an audio track, the Audio menu's two commands, and Snap Point to Cursor, which
  leaves selected parts out.
