# MIDI editors

The MIDI editors' common framework (#126, reference manual pp. 1132-1156): the editor in the project window's lower
zone and the editor window, which parts they show, the part active for editing, the part editing mode, the Visibility
tab, the global tracks and Keep Editor Contents. The key editor ([key-editor.md](key-editor.md), #127) and the list
editor ([list-editor.md](list-editor.md), #130) are the editors' contents for MIDI parts, the audio part editor
([audio-part-editor.md](audio-part-editor.md), #146) for audio parts; the drum editor ([drum-editor.md](drum-editor.md),
#129) is the key editor with a track's drum map.

## In the engine

`src/engine/document/editors.cpp` keeps a view per editor: `lower`, the lower zone's, and `window`, the editor
window's, and the in-place editors' `inPlace:<track>` ([in-place-editor.md](in-place-editor.md), #131). Views are view state like the event display's selection: they are no undo steps, don't make the project
modified and aren't saved, and a new or loaded project closes the editors.

| Command | What it does | Reference |
|---|---|---|
| `editor.get` | The view: `open`, `parts`, `activePart`, `activeTrack`, `editingMode`, `editable`, `keepContents`, `globalTracks`, `content` (`key`, `list` ([list-editor.md](list-editor.md)), `drum` ([drum-editor.md](drum-editor.md)), `pattern` ([pattern-editor.md](pattern-editor.md)) or `audioPart`), the key editor's selection and step and MIDI input ([key-editor.md](key-editor.md)), `controllerSelection`, the selected controller events of the list editor and the controller lanes ([controller-lanes.md](controller-lanes.md)), and the audio part editor's `eventSelection` ([audio-part-editor.md](audio-part-editor.md)) | |
| `editor.open` | Opens the editor with MIDI parts or audio parts, such as those selected in the project window; `activePart`, by default the first | pp. 692, 1189 |
| `editor.set` | Changes an open editor's parts and active part or track, and any editor's editing mode, Keep Editor Contents and global tracks | pp. 1142-1156 |
| `editor.close` | Closes it: no parts, Keep Editor Contents off | p. 1153 |

Each emits `editor.changed` with the view, and so does a change of the project that changes an open editor's parts or
closes it: an edit, an undo or redo that removes parts, brings them back, adds them to an in-place editor's track or
orders them anew (`track.move`, a part move), or a new or loaded project (#625), for clients that follow events.

- **Parts** are in the track list's order and on each track by position, as Activate Part for Editing lists them
  (p. 1143). Parts that are removed leave the view, and come back with an undo; the active part then moves to the first
  one left.
- **Part editing mode** (pp. 1142-1143): `allParts`, `activePart` or `activeTrack`, the active part's track. `editable`
  lists the parts that selecting and editing reach, for the editors' contents to use.
- **Visibility** (pp. 1148-1152): `showTracks` shows every MIDI part of the tracks, of all tracks in a folder, and
  activates the last; `hideTracks` hides theirs; `exclusive` hides every other track first. At least one part stays
  shown, and a track without parts can't show, nor can a track hidden in the project window
  ([track-visibility.md](track-visibility.md)) or a track or folder that holds locked events or parts (p. 1151); a
  folder leaves out its hidden tracks. `activeTrack` activates a track's first part shown.
- **Global tracks** (pp. 1144-1146): the tempo, signature, chord, ruler, arranger, marker and transpose tracks the editor
  window shows above its events, in the track
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
  .key-editor.drum-editor           instead, while the view's content is drum: the drum editor (drum-editor.md)
  .pattern-editor                   instead, while the view's content is pattern: the pattern editor (pattern-editor.md)
.editor-visibility                  an .editor-visibility-track[track][level] per MIDI or instrument track or folder with
                                    them, level the folders around it; .active for the active track, .unavailable for
                                    one that can't show, .hidden inside a collapsed folder; .closed while the view
                                    isn't open
  .tree-indent                      on every row, sized from its level
  .tree-toggle                      on every row, shown as a folder's triangle; the folder's row is .parent, and
                                    .expanded while its tracks are listed
  button.editor-visibility-check    .checked while all its parts show, .partial while some do; disabled while it can't
                                    change what shows
  .editor-visibility-name           a click activates a shown track, or expands or collapses a folder, a double click
                                    shows it exclusively
.editor-global-tracks-section       button.editor-global-toggle[track] per global track, .checked while shown
.editor-chord-editing               the inspector's Chord Editing section (key-editor.md)
```

- **Activate Part for Editing** lists the parts under their tracks' names (disabled items `.editor-menu-track`), the
  active one checked.
- **Visibility tab**: a click on the check shows or hides the track's parts, with Shift exclusively; on a rectangle it
  shows them all (p. 1151). The tracks in a folder are indented under it, and a click on the folder's name or its
  triangle collapses or expands it, in this tab only, without rebuilding the rows, so a double click on its name still
  shows it exclusively. An `editor.changed` updates the rows in place too while they list the same tracks with the
  same names, so a double click on a shown track's name, whose first click activated the track, still reaches the row
  it started on and shows the track exclusively (#2031). A folder counts the parts of a track hidden in the project window only while they show, as
  `showTracks` leaves them out, and isn't listed once none of its parts can show. A track that can't show
  (`editor.set`'s `showTracks` refuses it: hidden in the project window, or it or a track in the folder holds locked
  events or parts) is `.unavailable`: its check only hides what it still shows, and Shift or a double click do nothing.
  While the view isn't open, such as the project window's Visibility tab before the lower zone's editor opens, the tab
  is `.closed` and its checks are disabled, as `editor.set` couldn't change the parts.
- **Windows and zones** (`app::AppUi`). The editor window (`resources/ui/editor.rml`, docking window `editorWindow`)
  opens and closes as the view `window` does, whichever client opens it; its left zone has the Visibility tab
  (`editorVisibility`) and the Inspector with the Global Tracks section (`editorInspector`). The view `lower` shows the
  project window's Editor panel in the lower zone, and the project window's Visibility tab has the lower zone editor's
  on its Editor page ([track-visibility.md](track-visibility.md)).
  Closing the editor window, or hiding the lower zone while the Editor panel is docked there, closes the view, so Keep
  Editor Contents goes off (p. 1153); another tab of the lower zone in front of it doesn't. A new project closes both.
  Until the engine answers that `editor.close`, an `editor.changed` with the view open that was already on its way,
  for example from following the selection, doesn't open the window or show the lower zone again. The Editor tab
  coming to the front while the view `lower` is closed, as when the user picks it from the tab bar or shows the lower
  zone with it in front, opens the view with the project window's selected parts (p. 1153, #625). The app asks the
  engine first, so that an `editor.open` on its way, such as the editor window's Open in Lower Zone, keeps its parts.
- **Project reads** (#625). Each `ui::MidiEditor` (up to five: the lower zone's editor, both Visibility tabs, the
  editor window's editor and its inspector) needs the project after every change, but those that ask while the binding
  hands on the same answer or event share one `project.get` (`ui::ProjectReads`, which `app::AppUi` owns). One that
  asks later while that read is out gets the next read, which goes out once the first is answered.
- **Following the selection** (p. 1153): when the project window's selection changes, its MIDI parts, or else its
  audio parts, open in every open editor that doesn't keep its contents.
- **Commands**: `editor.openClose` (Mod+E and Return in the project window, Edit > Open/Close Editor, p. 1189) opens
  the selected MIDI parts, or else the selected audio parts in the audio part editor, where a double click opens them,
  or closes that editor when none is selected; with audio events but no parts selected it opens the first in the
  sample editor window ([sample-editor.md](sample-editor.md)), and without either it closes that too.
  `window.lowerZoneEditor` (Edit > Open Editor in Lower Zone) does the same for parts in the lower zone.
- **Where a double click opens them** (pp. 87, 1603): a double click on a MIDI part or an audio part in the project
  window and `editor.openClose` open it in the lower zone's editor, or in the editor window with the preference
  Double-click opens Editor in a Window (`editing.doubleClickOpensEditorIn`, Editors page,
  [preferences.md](preferences.md)). Opened in the lower zone, the editor brings its tab forward and takes the keyboard
  focus (p. 100), as with `window.lowerZoneEditor` ([docking.md](docking.md)). A double click on an audio event opens
  the sample editor window, which the lower zone doesn't have yet.
  The MIDI menu's `editor.openKey` (Open Key Editor, p. 1189), `editor.openDrum` (Open Drum Editor, p. 1221) and
  `editor.openList` (Open List Editor, Mod+G, p. 1243) open the selected MIDI parts in the editor window's key, drum or
  list editor (#719).

`resources/styles/midieditor.rcss` lays the editors out; the themes color the states (`tools/skin/generate.py`), the
Visibility tab's checks with the check box skins.

## Not yet

The list editor's setup panes of the toolbar and the info line (p. 1575), which the key, drum and in-place editors
have ([docking.md](docking.md)); event colors other than Part and Sound Slot, and the list editor's, the Part Editing Mode's effect on a selection rectangle and Show Part Borders,
Independent Track Loop, the global tracks' events and editing them, Find Tracks, track visibility configurations and
agents, the Track Display and its settings, selecting tracks in the Visibility tab, the preference Editor Content
Follows Event Selection, Open Editor Commands open Editors in a Window/in Lower Zone (p. 1603: the Audio and MIDI menus'
open commands, such as Mod+E, which here is Open/Close Editor), and saving the global track setup with the project. The
lower zone's Editor tab that stays in front while its view closes (after Open in Separate Window, an `editor.close` from
another client or a new project) shows an empty editor until it comes to the front again or Edit > Open Editor in Lower
Zone (#625).

## Checks

- `tests/editors_tests.cpp`: an editor opened with parts of two tracks in the track list's order, each editor with its
  own view, parts refused that aren't parts, no undo step and no change of the project, closing that lets go of
  the parts and Keep Editor Contents; the active part and track and the editable parts in each editing mode; removed
  parts leaving and coming back with undo; showing, hiding and exclusively showing tracks and folders, the last track
  kept and tracks without parts, hidden tracks and tracks with locked parts refused; global tracks in the track list's order, refused for the lower zone and for
  other tracks, removed tracks leaving; a new project closing the editors; `editor.changed` for a removed part leaving,
  its undo, an in-place editor closing with its track and opening again with its undo, and a new project, and none for
  a change that leaves the parts.
- `tests/midieditor_tests.cpp` drives the app with `ui::Automation`: Open/Close Editor for three parts on two tracks
  opens the editor window with them in order, the Activate Part for Editing menu lists them under their tracks and
  activates one, the part editing mode marks the editable parts, a click activates a part; the Visibility tab's checks
  hide and show tracks, a double click shows one exclusively, also when its first click's `editor.changed` (the
  track activated) reaches the tab before the second click, a rectangle for a track not all of whose parts show; the
  Global Tracks section shows a marker track above the events; Open/Close Editor without a selection, closing the
  window, an engine client and a new project open and close it, and an `editor.changed` on its way after the user closed
  the window doesn't open it again. The Visibility tab indents a folder's tracks, folds them with a click on its name
  and on its triangle, shows a folder exclusively with a double click on its name, marks tracks with locked parts or
  hidden in the project window, whose checks only hide what shows, counts a hidden track's parts in its folder only
  while they show, and is closed in the project window while the lower zone's editor isn't open. The lower zone's editor
  opens for selected parts, follows the selection, stops following with Keep Editor Contents, moves to the window and
  back, and closes when the lower zone hides. Return and a double click on a part open it in the lower zone's editor,
  which takes the keyboard focus, Return an audio part too, and in the editor window with Double-click opens Editor in a
  Window, which leaves the focus in the main zone.
