# Audio part editor

The audio part editor (#146, reference manual pp. 692-702): the editors' content for audio parts
([audio-parts.md](audio-parts.md)), in the editor window and the lower zone, as the key editor is for MIDI parts
([key-editor.md](key-editor.md)). It shows the parts' audio events on lanes with their waveforms, and edits them with
the toolbar's tools and the info line.

## In the engine

The audio part editor is a content of the editors' views (`src/engine/document/editors.cpp`,
[midi-editors.md](midi-editors.md)), so the framework's commands do for it what they do for the key editor: parts in the
track list's order, the active part, the part editing mode, Keep Editor Contents, the Visibility tab and the global
tracks. Like the rest of a view it is view state: no undo steps, not saved, and a new project closes the editors.

- **Parts.** `editor.open` takes MIDI parts or audio parts, not both. Audio parts open with `content` `audioPart`;
  MIDI parts open in the key editor or the list editor, and an editor that showed audio parts shows them in the key
  editor. `editor.set` refuses parts of the other kind and a content that doesn't fit its parts.
- **Visibility.** `showTracks` and `hideTracks` show and hide the audio parts of audio tracks (and of folders of them)
  while the editor shows audio parts.
- **Selection.** `eventSelection` in `editor.get`, `editor.set` and `editor.changed`: the selected audio events, of the
  editable parts. Removed events and those the part editing mode no longer reaches leave it (p. 702), closing the editor
  clears it.
- `editor.input` takes no notes into audio parts.

Edits are the `event.*` commands on the events in the parts ([parts-and-events.md](parts-and-events.md)); the events
keep their own positions on the track, and `lane` is the lane they are on (`event.set`).

## In the UI

`ui::AudioPartEditor` (`src/ui/audioparteditor.h`) fills the editor of a `ui::MidiEditor` while the view's content is
`audioPart`, with the framework's toolbar above it ([midi-editors.md](midi-editors.md)). The Visibility tab lists audio
tracks then, and the Activate Part for Editing menu the audio parts. It shows the project and the view as `MidiEditor`
hands them on and sends `event.*`, `audioEvent.set`, `audition.*` and `editor.set`; several commands go in one
`history.transaction`.

```
.audio-part-editor[tool=objectSelection|erase|split|glue|mute|play|scrub]
  .toolbar.audio-part-editor-toolbar
    button.ape-tool[ape-tool]       the tools; .checked for the current one
  .info-line.ape-info               the first selected event: textdisplay[field=start|end|length|volume]; .empty
                                    without, .multiple with several selected
  .editor-parts                     an .editor-part-row[track] per track, its .editor-part[part] at their time
  .ape-body                         scrolls vertically
    .ape-display
      .ape-lanes                    an .ape-lane[lane] per lane the events are on, and one more below
      .ape-events                   an .ape-event[part][event] per event (.selected, .muted, .inactive outside the
                                    part editing mode)
      .ape-waveforms                a waveform per event (ui::EventWaveforms, peaks.md)
```

The display fits the shown parts' range into its width; each event shows what its part lets play of it, at its time on
its lane. Code places the parts and the events in percent of the width and the lanes and events at their lane, 48 dp
each. Moves, splits and the info line's positions snap to the project's quantize grid.

| Tool | Gesture | Reference |
|---|---|---|
| Object Selection | A click selects an event, Shift and a click adds or removes it, a click in the empty display deselects; a drag moves the selected events in time and onto other lanes; with Ctrl a drag moves them across lanes only and a click adds or removes the event; a double click opens the event in the sample editor ([sample-editor.md](sample-editor.md)) | pp. 694, 699 |
| Erase | A click deletes the event | p. 694 |
| Split | A click splits the event at the snapped position; on a selected event, every selected event there | p. 695 |
| Glue | A click glues the event to the next one on its lane in its part, where it continues it in the clip | |
| Mute | A click mutes or unmutes the event | p. 695 |
| Play | Auditions the event's track (or the active part's) from the mouse until the release (`audition.start`, [audition.md](audition.md)) | pp. 695, 700 |
| Scrub | A drag scrubs the track (`audition.scrub`) | p. 701 |

- **Info line** (p. 698): start, end and length in bars and beats, and the volume in dB. A start typed moves every
  selected event by as much as the first one's changes, an end or length resizes their ends, a volume changes theirs
  by as much.
- **Opening it** (p. 692): a double click on an audio part in the project window, Open/Close Editor (Mod+E, Return)
  with audio parts but no MIDI parts selected, the Audio menu's Open Audio Part Editor (`audio.openPartEditor`), or
  Open Editor in Lower Zone. Open editors that follow the project window's selection take its MIDI parts, or else its
  audio parts.

`resources/styles/audioparteditor.rcss` lays it out; the themes color it (`tools/skin/generate.py`).

## Not yet

Solo Editor, Audition and Audition Loop, auto-scroll and Link Project and Lower Zone Editor Cursors; the Combine
Selection, Range Selection, Draw (volume curves), Zoom, Comp and Time Warp tools; the selection rectangle and resizing
events with the mouse; Show Part Borders, Activate Next Part and Previous Part and the search field of the Activate
Part for Editing menu; nudging, Snap on and off, the snap and grid types and Snap to Zero Crossing; quantizing, event
colors and the independent track loop; the ruler and its time format, Show/Hide Info and the toolbar's context menu;
horizontal zoom and scrolling; the play tool stopping at the part's end; the preference for where the editor opens;
the toolbox on a right click; shared copies of audio parts (audio parts have none yet). The waveforms follow a change
of the window's size, not yet one of the lower zone's.

## Checks

- `tests/audioparteditor_tests.cpp`: audio parts opening in the audio part editor in the track list's order, parts
  and contents of the other kind refused, MIDI parts going back to the key editor, the Visibility tab's audio tracks,
  removed parts leaving and coming back with undo; the selected audio events of the editable parts, refused outside
  them, leaving with the part editing mode and with removed events, no undo step, cleared by closing.
- `tests/audioparteditor_ui_tests.cpp` drives the app with `ui::Automation`: Open/Close Editor with an audio part
  selected, the events at their time and lane, the extra lane, a waveform each and the part row; Open Audio Part
  Editor and the lower zone's editor; the object selection tool selecting, deselecting, moving on the grid, onto
  another lane, across lanes only with Ctrl, Ctrl and a click, and a double click opening the sample editor; the
  split, glue, mute and erase tools with undo; the play and scrub tools auditioning the track; typing a start and a
  volume on the info line.
- `tests/eventediting_tests.cpp`: a double click on an audio part opens it in the audio part editor.
