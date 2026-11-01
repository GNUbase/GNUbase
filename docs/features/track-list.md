# Track list

The project window's track list and its track controls (#81, reference manual pp. 66–67, 189–196, 213–219, 225): a
row per track beside the event display's lanes, with the track's name and controls, track selection, renaming,
reordering, heights, folding, duplicating, disabling and removing tracks, and the track controls settings dialog.
In the engine: mute and solo, disabled tracks and `track.duplicate`.

## In the engine

- **Mute and solo** are the channel's switches `mute` and `solo` (`channel.set`, every channel type). A muted channel
  is silent. While any channel is soloed, only the soloed channels are heard, with the channels they play through
  (their groups and folders' group channels, a MIDI track's instrument) and the channels that play into them (the
  tracks of a soloed group, the MIDI tracks of a soloed instrument track); a muted channel stays silent, soloed or
  not. The render snapshot marks the others `silent` (`markSilent` in `src/engine/document/rendersnapshot.cpp`), and
  each silent channel's strip (`ChannelStrip`) glides its output to silence over one audio block, and back, so
  switching doesn't click. A silent MIDI track's notes and controllers are left out of the snapshot, as a muted part's
  are, so they don't reach its instrument. Silent channels don't monitor their input directly either.
- **Disabled tracks** (p. 219): `track.set` `enabled` false disables an audio, instrument or MIDI track. It keeps its
  events, parts and channel settings, but the snapshot leaves its events and parts out (no disk streaming) and its
  channel is silent. Other track types refuse it.
- **`track.duplicate`** (p. 219) copies a track below itself, with its channel and every setting, its audio events
  (on the same clips, crossfades between copies kept), its MIDI parts (notes and controllers with new ids; shared
  copies stay shared copies of the same group), its markers and its automation lanes and points. A folder comes with
  the tracks inside it, whose channels play through the copies of the group channels they played through. It
  announces each copy (`track.added`, `audioEvent.added` and so on) and returns the copy of the track; a marker track
  beyond the project's ten is refused.
- **DAWproject:** a channel's mute is its `Mute` parameter, solo its `solo` attribute, and a disabled track has
  `loaded="false"`; all three come back from `project.xml` without GNUbase's entry.

## The track list

`ui::TrackList` (`src/ui/tracklist.h`) fills `#track-list` in the project window's main zone, left of the event
display (`resources/ui/project.rml`, `AppUi::showProject`). Like the event display it is one more client of the
engine: it shows the project the event display last read (`EventDisplay::project`, one `project.get` for both) and
sends engine commands, so whatever another client changes shows at once.

```
#track-list                       the element the document gives it, with the context menu #track-list-menu
  .track-list-header              the global track controls: #add-track, #track-controls-settings (document)
  .track-rows                     clips the rows, under the overview line and ruler's height
    listview.tracks[multiple]     the list view (widgets.md), scrolled with the event display
      listitem[value=<id>][level][type]   a tree row per track; .selected, .disabled, .renaming
        .tree-indent, .tree-toggle        from the list view, by level
        .track-name, input.track-name-field
        .track-controls > button.toggle[control]   .checked while on, disabled without anything to do
    .drop-line                    where dragged tracks go, .shown while dragging
```

- **Rows and lanes.** The event display has a lane per track that isn't hidden, each as high as its track
  (`EventDisplay::trackHeight`, `g_trackHeight` (40 dp) until set, 20 to 400 dp, times vertical zoom), and tells the
  track list when its lanes or view change (`onLayout`). The list sets each row's height from its lane and moves the
  list view up by the display's vertical scroll, so rows and lanes stay level; the header is as high as the overview
  line and the ruler, by style sheet, also without the overview line. The wheel over the list scrolls the display.
- **Controls** (pp. 191–195). The user setting `ui.trackControls` chooses each track type's controls and their order:
  `mute` (M) and `solo` (S), which tracks with a channel always show, `record` and `monitor` (audio tracks), `read`
  (every automation lane of the track reads, or with a click none does; disabled without lanes) and `timeBase`
  (musical or linear, `.checked` when linear). A click sends `channel.set`, `automationLane.set` in one transaction,
  or `track.set`. Folders without a group channel, marker and ruler tracks show none. The defaults: mute, solo,
  record, monitor and read on audio tracks; mute, solo and read on the others with a channel.
- **Selection** (p. 218) is the list view's: a click selects a track, Ctrl-click adds or removes one, Shift-click
  selects a range; Up and Down step through the shown tracks, in the list and wherever the project window has the
  focus (`track.selectPrevious`, `track.selectNext`), and scroll the selected track into view. A right click on a
  track that isn't selected selects it for the context menu. The selection is the UI's, not the project's
  (`TrackList::selection`), like the event display's.
- **Renaming** (p. 214): a double click on a name, or `track.rename` (Alt+F2) for the first selected track, shows a
  text field with the name selected; Enter or leaving it sends `track.set` `name`, Escape keeps the name.
- **Moving** (pp. 213–214): dragging a row moves the selected tracks (or the dragged one, if it isn't selected) to
  where the line shows: before the track in the upper half of a row, after it in the lower half, and into a folder,
  last, on the middle of the folder's row. The moves are `track.move` commands in one `history.transaction`, "Move
  Tracks", so one undo restores the order.
- **Heights** (p. 217): dragging a row's lower edge (its last 4 dp) sets its track's height, with Ctrl (Cmd) every
  track's. Heights are view state, like zoom: not in the project or its undo history.
- **Folding** (p. 225): a folder's toggle, or Right and Left on the selected folder, folds and unfolds it; the tracks
  inside a folded folder leave the list and the event display (`EventDisplay::setHiddenTracks`) and still play.
  Project > Track Folding: Toggle Selected Track, Fold Tracks, Unfold Tracks and Flip Fold States
  (`track.toggleFolding`, `track.foldAll`, `track.unfoldAll`, `track.flipFolding`). Folding is view state too.
- **Track handling** (pp. 213, 219): Project > Remove Selected Tracks, Remove Empty Tracks, Duplicate Tracks, and in
  the context menu also Disable and Enable Selected Tracks (`track.removeSelected`, `track.removeEmpty`,
  `track.duplicateSelected`, `track.disableSelected`, `track.enableSelected`), each one `history.transaction`. Empty
  tracks are audio, instrument, MIDI and marker tracks without events, parts, markers or automation, and folders
  without tracks, unless other tracks' channels output to theirs; group and effect tracks are where others play
  through and stay, ruler tracks never hold anything.
- **Key commands.** `AppUi` adds the track list's commands (`TrackList::actions`) as key commands in the project
  window's focus zone, enabled while the list exists and, for those of selected tracks, while tracks are selected,
  so the menus disable them otherwise.

## Track controls settings

`resources/ui/track-controls.rml` (pp. 189–191), opened by `#track-controls-settings`, the context menu or the UI
command `track.controlsSettings`: the track types on the left, the hidden and the visible controls of the selected
type, Add, Remove (not for mute and solo), Move Up, Move Down and Reset (the type's defaults from `settings.list`).
OK saves every type with `settings.set` `ui.trackControls`, which the track list follows; Cancel and Escape drop the
changes.

## Not yet

Lanes and comping (#99), write automation (#122), track colors and the color strip (#94), the inspector (#89),
recording from instrument and MIDI tracks (#97), dividing the track list, track visibility agents and the track type
filter, track presets, track pictures, the lock control, edit channel settings, volume and pan in the track list,
the settings dialog's groups, widths and presets, the track height presets menu, snap track heights, Shift-click to
deselect, the warning before removing tracks that aren't empty, Move Selected Tracks to New Folder (with folders,
#82), and the ruler and marker tracks' own controls (#447, #91). Adding a track below the selected one is #448.

## Checks

- `tests/document_tests.cpp`: which channels mute and solo silence, also through groups and from MIDI tracks to
  instruments, with undo; disabled tracks in the snapshot and which types refuse it; `track.duplicate` with a
  channel, events, automation, MIDI notes, a folder with its routing, undo and the marker track limit.
- `tests/mixer_tests.cpp` renders muted and soloed tracks and the glide; `tests/sequencing_tests.cpp` plays a muted, a
  disabled and a soloed MIDI track into an instrument; `tests/dawproject_tests.cpp` saves and loads mute, solo and
  disabled tracks, also from `project.xml` alone.
- `tests/tracklist_tests.cpp` drives the app's project window with `ui::Automation`: rows level with the lanes and
  each type's controls; every control's button and the engine's state both ways; selection with clicks, modifiers,
  the arrow keys and the key commands; renaming with Enter and Escape; moving by a drag and undoing it; heights by a
  drag, with Ctrl for all; the wheel; folding from the toggle and the commands, in the event display too; duplicating,
  disabling, enabling and removing from the context menu and the commands; and the settings dialog's Add, Remove,
  Move Up and Reset, saved in the user setting and shown in the rows.
