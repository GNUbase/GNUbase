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
  while the editor shows audio parts, or a `content` `audioPart` given with them says it will.
- **Selection.** `eventSelection` in `editor.get`, `editor.set` and `editor.changed`: the selected audio events, of the
  editable parts. Removed events and those the part editing mode no longer reaches leave it (p. 702), closing the editor
  clears it.
- `editor.input` takes no notes into audio parts, and `editor.set` refuses a `selection` or `controllerSelection` in
  them.

Edits are the `event.*` commands on the events in the parts ([parts-and-events.md](parts-and-events.md)); the events
keep their own positions on the track, and `lane` is the lane they are on (`event.set`). The lanes of the events in an
audio part are the part's own, not lanes of the track: the project window's Show Lanes doesn't count them
([comping.md](comping.md)), and an event moved onto another lane in the editor adds no lane to its track. Events to
Part and Dissolve Part keep the events' lane numbers: lanes of the track become the part's and back.

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
                                    part editing mode, .outside where its part hides it entirely)
      .ape-waveforms                a waveform per event (ui::EventWaveforms, peaks.md)
```

The display fits the shown parts' range into its width; each event shows what its part lets play of it, at its time on
its lane. An event moved out of its part, which the part hides entirely, shows whole, and the display reaches it, so
that it can be selected and moved back. Code places the parts and the events in percent of the width and the lanes and
events at their lane, 48 dp each. While the project's snap is on (`snap.set`), moves and splits snap to the project's
quantize grid; with it off they go where the mouse is.

A gesture (a move or an audition) follows the drag events of an RmlUi drag element: the editable events with the
object selection tool and the whole display with the play and scrub tools (`drag: drag` in the style sheet, by the
`tool` attribute). Nothing else in the display is one, so that a finger pan scrolls it where a press starts no gesture.
The gesture's mouseup in the editor's whole document or the drag element's dragend ends it, so it ends wherever the
button goes up, also outside the window, with its effect there. A press anywhere in the document while one goes on,
whose end never came, ends it without its effect: a move moves nothing, an audition stops, and the events show where
they are again. While a move drags, the events it moves show where it takes them, also after the editor shows the
project again, as when the selection of the press comes back. The pressed event's element stays when the events are
drawn again during the drag, as when a command adds one to the part: RmlUi drops a drag without a dragend when its
element goes. A command that removes the pressed event, or takes its part out of the part editing mode, ends the move
without its effect. A click shows its selection at once, and a view that comes before the engine has answered it,
which may be older than it, leaves that selection as it is.

| Tool | Gesture | Reference |
|---|---|---|
| Object Selection | A click selects an event, Shift and a click adds or removes it, a click in the empty display deselects; a drag moves the selected events in time and onto other lanes; with Ctrl a drag moves them across lanes only (an event that isn't selected alone) and a click adds or removes the event; a double click opens the event in the sample editor ([sample-editor.md](sample-editor.md)) | pp. 694, 699 |
| Erase | A click deletes the event | p. 694 |
| Split | A click splits the event at the snapped position; on a selected event, every selected event there | p. 695 |
| Glue | A click glues the event to the one on its lane that continues it in the clip, as after a split; a click on an event that nothing continues does nothing, as the engine glues no other audio events in a part | |
| Mute | A click mutes or unmutes the event | p. 695 |
| Play | Auditions the event's track (or the active part's) from the mouse until the release, up to the end of the event's part (or the active part) (`audition.start` with `end`, [audition.md](audition.md)) | pp. 695, 700 |
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

The setup panes of the toolbar and the info line (p. 1575), which only the project window has so far
([docking.md](docking.md)); Solo Editor, Audition and Audition Loop, auto-scroll and Link Project and Lower Zone Editor Cursors; the Combine
Selection, Range Selection, Draw (volume curves), Zoom, Comp and Time Warp tools; the selection rectangle and resizing
events with the mouse; Show Part Borders, Activate Next Part and Previous Part and the search field of the Activate
Part for Editing menu; nudging, a Snap switch of its own, the snap and grid types and Snap to Zero Crossing; quantizing,
event colors and the independent track loop; the ruler and its time format, Show/Hide Info and the toolbar's context
menu; horizontal zoom and scrolling; the preference for where the editor opens;
the toolbox on a right click; shared copies of audio parts (audio parts have none yet). The waveforms follow a change
of the window's size, not yet one of the lower zone's.

## Checks

- `tests/audioparteditor_tests.cpp`: audio parts opening in the audio part editor in the track list's order, parts
  and contents of the other kind refused, MIDI parts going back to the key editor, the Visibility tab's audio tracks,
  removed parts leaving and coming back with undo; the selected audio events of the editable parts, refused outside
  them, leaving with the part editing mode and with removed events, no undo step, cleared by closing; notes and
  controller events of audio parts refused, and `showTracks` showing the parts of a `content` given with it; Events
  to Part and Dissolve Part keeping the events' lanes.
- `tests/audioparteditor_ui_tests.cpp` drives the app with `ui::Automation`: Open/Close Editor with an audio part
  selected, the events at their time and lane, the extra lane, a waveform each, the part row and the Visibility tab's
  audio track; Open Audio Part Editor and the lower zone showing the editor; following the project window's
  selection, and not with Keep Editor Contents; the object selection tool selecting (the info line's start, end and
  length), deselecting, moving on the grid, onto another lane (which adds no lane to the track in the project
  window), across lanes only with Ctrl, Ctrl and a click, and a double click opening the sample editor; a Ctrl drag
  of an event that isn't selected showing and moving only it; moves and splits off the grid with snap off; the split,
  glue (nothing for an event that nothing continues), mute and erase tools with undo; the play tool auditioning up to
  the part's end and the scrub tool; typing a start and a volume on the info line; a move and an audition released
  over the window's toolbar and outside the window, and a press ending a gesture whose mouseup never came; the play
  tool from an empty spot and after a part's end; an event out of its part shown, selected and moved back; a finger
  pan over an empty spot and over an event outside the part editing mode scrolling the lanes; a render during a move
  keeping it shown; a move going on when a command adds an event to the part during the drag, released outside the
  window, and when the display's range changes, a Ctrl drag going on with its event's new lane and its element in
  order, and a move ending without its effect when its event is removed or leaves the part editing mode; a view the
  engine answered before a click leaving the click's selection, and a selection from the
  engine showing once the click's is answered.
- `tests/eventediting_tests.cpp`: a double click on an audio part opens it in the audio part editor.
