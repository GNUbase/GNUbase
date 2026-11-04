# MIDI editors

The MIDI editors' common framework (#126, reference manual pp. 1132-1156): the editor in the project window's lower
zone and the editor window, which parts they show, the part active for editing, the part editing mode, the Visibility
tab, the global tracks and Keep Editor Contents. The key editor ([key-editor.md](key-editor.md), #127) and the list
editor ([list-editor.md](list-editor.md), #130) are the editors' contents for MIDI parts, the audio part editor
([audio-part-editor.md](audio-part-editor.md), #146) for audio parts; the drum editor will build on the framework too.

## In the engine

`src/engine/document/editors.cpp` keeps a view per editor: `lower`, the lower zone's, and `window`, the editor
window's. Views are view state like the event display's selection: they are no undo steps, don't make the project
modified and aren't saved, and a new or loaded project closes the editors.

| Command | What it does | Reference |
|---|---|---|
| `editor.get` | The view: `open`, `parts`, `activePart`, `activeTrack`, `editingMode`, `editable`, `keepContents`, `globalTracks`, `content` (`key` or `list`, [list-editor.md](list-editor.md), or `audioPart`), the key editor's selection and step and MIDI input ([key-editor.md](key-editor.md)), the list editor's `controllerSelection` and the audio part editor's `eventSelection` ([audio-part-editor.md](audio-part-editor.md)) | |
| `editor.open` | Opens the editor with MIDI parts or audio parts, such as those selected in the project window; `activePart`, by default the first | pp. 692, 1189 |
| `editor.set` | Changes an open editor's parts and active part or track, and any editor's editing mode, Keep Editor Contents and global tracks | pp. 1142-1156 |
| `editor.close` | Closes it: no parts, Keep Editor Contents off | p. 1153 |

Each emits `editor.changed` with the view.

- **Parts** are in the track list's order and on each track by position, as Activate Part for Editing lists them
  (p. 1143). Parts that are removed leave the view, and come back with an undo; the active part then moves to the first
  one left.
- **Part editing mode** (pp. 1142-1143): `allParts`, `activePart` or `activeTrack`, the active part's track. `editable`
  lists the parts that selecting and editing reach, for the editors' contents to use.
- **Visibility** (pp. 1148-1152): `showTracks` shows every MIDI part of the tracks, of all tracks in a folder, and
  activates the last; `hideTracks` hides theirs; `exclusive` hides every other track first. At least one part stays
  shown, and a track without parts can't show. `activeTrack` activates a track's first part shown.
- **Global tracks** (pp. 1144-1146): the marker and ruler tracks the editor window shows above its events, in the track
  list's order. The lower zone's editor has none, as in the reference.

## In the UI

`ui::MidiEditor` (`src/ui/midieditor.h`) fills an element with a part of an editor, as one more client of the engine:
the editor itself, its Visibility tab or its Global Tracks section. Each follows `editor.changed` and the project, and
sends `editor.*` commands.

```
.midi-editor[editor=lower|window]   .empty without parts
  .toolbar.midi-editor-toolbar
    #editor-keep-contents           Keep Editor Contents; .checked while on
    #editor-editing-mode            Part Editing Mode, menu #editor-editing-mode-menu
    #editor-active-part             Activate Part for Editing: the active part's name, menu #editor-active-part-menu
    #editor-move                    Open in Separate Window, or in the window Open in Lower Zone (p. 1196)
  .editor-global-tracks             the window's: an .editor-global-track[track] per global track shown
  .key-editor                       the key editor (key-editor.md), with an .editor-part[part] per part: .active,
                                    .editable; a click activates it
  .list-editor                      instead, while the view's content is list: the list editor (list-editor.md)
.editor-visibility                  an .editor-visibility-track[track] per MIDI or instrument track or folder with them;
                                    .active for the active track
  button.editor-visibility-check    .checked while all its parts show, .partial while some do
  .editor-visibility-name           a click activates a shown track, a double click shows it exclusively
.editor-global-tracks-section       button.editor-global-toggle[track] per global track, .checked while shown
.editor-chord-editing               the inspector's Chord Editing section (key-editor.md)
```

- **Activate Part for Editing** lists the parts under their tracks' names (disabled items `.editor-menu-track`), the
  active one checked.
- **Visibility tab**: a click on the check shows or hides the track's parts, with Shift exclusively; on a rectangle it
  shows them all (p. 1151).
- **Windows and zones** (`app::AppUi`). The editor window (`resources/ui/editor.rml`, docking window `editorWindow`)
  opens and closes as the view `window` does, whichever client opens it; its left zone has the Visibility tab
  (`editorVisibility`) and the Inspector with the Global Tracks section (`editorInspector`). The view `lower` shows the
  project window's Editor panel in the lower zone, and the project window's Visibility tab is the lower zone editor's.
  Closing the editor window, or hiding the lower zone while the Editor panel is docked there, closes the view, so Keep
  Editor Contents goes off (p. 1153); another tab of the lower zone in front of it doesn't. A new project closes both.
- **Following the selection** (p. 1153): when the project window's selection changes, its MIDI parts, or else its
  audio parts, open in every open editor that doesn't keep its contents.
- **Commands**: `editor.openClose` (Mod+E and Return in the project window, Edit > Open/Close Editor, p. 1189) opens
  the selected MIDI parts in the editor window, or else the selected audio parts in the audio part editor, or closes
  it when none is selected; with audio events but no parts selected it opens the first in the sample editor
  ([sample-editor.md](sample-editor.md)), and without either it closes that too. `window.lowerZoneEditor` (Edit > Open
  Editor in Lower Zone) does the same for parts in the lower zone.
  `editor.openList` (Mod+G, Edit > Open List Editor, p. 1243) opens them in the editor window's list editor.

`resources/styles/midieditor.rcss` lays the editors out; the themes color the states (`tools/skin/generate.py`), the
Visibility tab's checks with the check box skins.

## Not yet

Event colors, the Part Editing Mode's effect on a
selection rectangle and Show Part Borders, Independent Track Loop, the global tracks' events and editing them, Find
Tracks, track visibility configurations and agents, the Track Display and its settings, selecting tracks in the
Visibility tab, the Project/Editor switch of the project window's Visibility tab, the preference Editor Content Follows
Event Selection and where editors open by default, double-clicking a part to open it, and saving the global track
setup with the project.

## Checks

- `tests/editors_tests.cpp`: an editor opened with parts of two tracks in the track list's order, each editor with its
  own view, parts refused that aren't parts, no undo step and no change of the project, closing that lets go of
  the parts and Keep Editor Contents; the active part and track and the editable parts in each editing mode; removed
  parts leaving and coming back with undo; showing, hiding and exclusively showing tracks and folders, the last track
  kept and tracks without parts refused; global tracks in the track list's order, refused for the lower zone and for
  other tracks, removed tracks leaving; a new project closing the editors.
- `tests/midieditor_tests.cpp` drives the app with `ui::Automation`: Open/Close Editor for three parts on two tracks
  opens the editor window with them in order, the Activate Part for Editing menu lists them under their tracks and
  activates one, the part editing mode marks the editable parts, a click activates a part; the Visibility tab's checks
  hide and show tracks, a double click shows one exclusively, a rectangle for a track not all of whose parts show;
  the Global Tracks section shows a marker track above the events; Open/Close Editor without a selection, closing the
  window, an engine client and a new project open and close it. The lower zone's editor opens for selected parts,
  follows the selection, stops following with Keep Editor Contents, moves to the window and back, and closes when the
  lower zone hides.
