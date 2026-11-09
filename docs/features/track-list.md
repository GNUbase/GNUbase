# Track list

The project window's track list and its track controls (#81, reference manual pp. 66–67, 189–196, 213–219, 225): a
row per track beside the event display's lanes, with the track's name and controls, track selection, renaming,
reordering, heights, folding, duplicating, disabling and removing tracks, and the track controls settings dialog. The
divider beside the event display sizes the track list, and the track name and controls area widths size its rows
(#493).
In the engine: mute and solo, disabled tracks, `track.duplicate` and the track heights and folded folders saved with
the project (#491).

## In the engine

- **Mute and solo** are the channel's switches `mute` and `solo` (`channel.set`, every channel type; solo defeat,
  exclusive solo and listen are in [solo-mute-listen.md](solo-mute-listen.md)). A muted channel
  is silent. While any channel is soloed, only the soloed channels are heard, with the channels they play through
  (their groups and folders' group channels, a MIDI track's instrument) and the channels that play into them (the
  tracks of a soloed group, the MIDI tracks of a soloed instrument track); a muted channel stays silent, soloed or
  not. A mute automation lane that reads and has points overrides the mute button while it plays, also in its gaps
  where the button's state plays ([automation.md](automation.md)). The render snapshot marks the others `silent`
  (`markSilent` in `src/engine/document/rendersnapshot.cpp`), and each silent channel's strip (`ChannelStrip`) glides
  its output to silence over one audio block, and back, so switching doesn't click; the mute button mutes through
  the fader's mute instead, gliding over 5 ms like mute automation, so that switching between the two is seamless. A
  silent MIDI track's notes and controllers are left out of the snapshot, as a muted part's are, so they don't reach
  its instrument, and its mute ends the notes that sound. Nor do its controller, pitch bend and channel pressure
  automation, its MIDI volume and pan (CC 7 and CC 10) and its program: `channelAutomation` in
  `src/engine/audio/automation.cpp` leaves them out of what its strip plays, so a muted or disabled MIDI track doesn't
  change the instrument that other tracks play. Once it is heard again, it sends them at once. Silent channels don't
  monitor their input directly either.
- **Disabled tracks** (p. 219): `track.set` `enabled` false disables an audio, instrument or MIDI track. It keeps its
  events, parts and channel settings, but the snapshot leaves its events and parts out (no disk streaming) and its
  channel is silent. Other track types refuse it.
- **`track.duplicate`** (p. 219) copies a track below itself, with its channel and every setting, its audio events
  (on the same clips, crossfades between copies kept), its MIDI parts (notes and controllers with new ids; shared
  copies stay shared copies of the same group), its markers and its automation lanes and points. A folder comes with
  the tracks inside it, whose channels play through the copies of the group channels they played through. It
  announces each copy (`track.added`, `audioEvent.added` and so on) and returns the copy of the track; a marker track
  beyond the project's ten is refused.
- **Track heights and folded folders** (pp. 217, 225, #491): `trackLayout.get` and `trackLayout.set`
  (`src/engine/document/trackvisibility.cpp`) keep `Project::trackLayout`: `heights`, each track's height in dp before
  vertical zoom (20 to 400), by track, and `folded`, the folded folder tracks. `heights` sets the given tracks' and
  leaves the others; `folded` replaces the list, and refuses tracks that aren't folders. Like the track visibility they
  are settings, not undo steps, and saved with the project in GNUbase's extra entry (`trackLayout` in `project.get`),
  empty for files from before; tracks that are gone are left out of what `trackLayout.get` returns. Every change
  emits `trackLayout.changed`.
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
        .track-color                      the track's color strip, attribute color (colors.md)
        .track-name, input.track-name-field
        .track-controls > button.toggle[control]   .checked while on, disabled without anything to do
    .drop-line                    where dragged tracks go, .shown while dragging
```

- **Width.** The track list starts 280 dp wide (`size` in `project.rml`). Dragging the divider between it and the
  event display (`splitter for="trackList"`) sizes it, between its minimum of 120 dp and what leaves the event display
  its own 72 dp; the event display takes the rest of the main zone. Both minimums and the divider fill the 200 dp that
  the zones' column keeps when side zones take the rest of the window. The docking system keeps the width in the user
  setting `ui.layout` (`windows.project.sizes.trackList`, [docking.md](docking.md)), so it comes back after a restart,
  and any client changes it with `settings.set`. A double click on the divider leaves it alone. A window too narrow for
  both takes the room from the track list once the event display is at its minimum.
- **Name and controls widths** (p. 191). The user setting `ui.trackWidths` has the track name's width for every track
  type (`name`, 20 to 400 dp) and each type's controls area width (`controls`, 0 to 800 dp), 120 dp each by default (the
  tempo track's 144 dp, so its three controls fit on one line) and where it has none. The name and the controls area are
  as wide as that, and the controls go onto more lines when they don't fit, as many as the track's height shows. In a
  row too narrow for both, such as deep in folders or in a narrow track list, the name gives way; what still doesn't fit
  is hidden at the row's right. The dialog keeps what the setting has that it doesn't know, such as another type's width
  from a later GNUbase.
- **Rows and lanes.** The event display has a lane per track that isn't hidden, each as high as its track
  (`EventDisplay::trackHeight`, `g_trackHeight` (40 dp) until set, 20 to 400 dp, times vertical zoom), and tells the
  track list when its lanes or view change (`onLayout`). The list sets each row's height from its lane and moves the
  list view up by the display's vertical scroll, so rows and lanes stay level; the header is as high as the overview
  line and the ruler, by style sheet, also without the overview line. The wheel over the list scrolls the display.
- **Controls** (pp. 191–195). The user setting `ui.trackControls` chooses each track type's controls and their order:
  `mute` (M) and `solo` (S), which tracks with a channel always show, `record` and `monitor` (audio tracks), `read`
  (every automation lane of the track reads, or with a click none does; disabled without lanes) and `timeBase`
  (musical or linear, `.checked` when linear). A click sends `channel.set`, `automationLane.set` in one transaction,
  or `track.set`. Folders without a group channel, marker and signature tracks show none. The tempo track's controls are
  `tempoTrack`, Activate Tempo Track (`project.set` `tempoTrackActive`), `currentTempo`, Current Tempo, a text field
  with the tempo at the project cursor that takes a tempo typed with Enter, and `tempoType`, Type of New Tempo Points,
  a drop-down list for `editing.tempoType` ([tempo-track.md](tempo-track.md)); ruler tracks have `timeFormat`,
  `timeOffsetOn`, `timeOffset` and `setTimeOffset` ([ruler-tracks.md](ruler-tracks.md)). The defaults: mute, solo,
  record, monitor and read on audio tracks; mute, solo and read on the others with a channel; all three on the tempo
  track; all four on ruler tracks.
- **Selection** (pp. 218–219) is the list view's: a click selects a track, Ctrl-click adds or removes one,
  Shift-click selects a range, and Shift-click on a selected track deselects it (the list view's `shiftdeselects`,
  [widgets.md](widgets.md)). Up and Down step through the shown tracks, in the list and wherever the project window
  has the focus (`track.selectPrevious`, `track.selectNext`), and scroll the selected track into view; in the list,
  Shift+Up and Shift+Down extend the selection. While an event or part is selected, they also select the first one on
  the new track, by position ([parts-and-events.md](parts-and-events.md)), unless the user setting
  `ui.upDownSelectsTracksOnly` (Use Up/Down Navigation Commands for Selecting Tracks Only, off by default) says they
  only select tracks. A right click on a track that isn't selected selects it for the context menu. The selection is the UI's, not the
  project's (`TrackList::selection`), like the event display's.
- **Renaming** (p. 214): a double click on a name, or `track.rename` (Alt+F2) for the first selected track, shows a
  text field with the name selected; Enter or leaving it sends `track.set` `name`, Escape keeps the name.
- **Adding tracks** (p. 198): a double click in the empty part of the list, below the last track, adds a track like
  the selected one below it, or a MIDI track ([add-track.md](add-track.md#double-click)).
- **Moving** (pp. 213–214): dragging a row moves the selected tracks (or the dragged one, if it isn't selected) to
  where the line shows: before the track in the upper half of a row, after it in the lower half, and into a folder,
  last, on the middle of the folder's row. The moves are `track.move` commands in one `history.transaction`, "Move
  Tracks", so one undo restores the order.
- **Heights** (p. 217): dragging a row's lower edge (its last 4 dp) sets its track's height, with Ctrl (Cmd) every
  track's; releasing the mouse sends them with `trackLayout.set`. Heights are the project's, saved with it, but not in
  its undo history.
- **Folding** (p. 225): a folder's toggle, or Right and Left on the selected folder, folds and unfolds it; the tracks
  inside a folded folder leave the list and the event display (`EventDisplay::setHiddenTracks`) and still play.
  Project > Track Folding: Toggle Selected Track, Fold Tracks, Unfold Tracks and Flip Fold States
  (`track.toggleFolding`, `track.foldAll`, `track.unfoldAll`, `track.flipFolding`). Each sends the folded folders
  with `trackLayout.set`: they are the project's, like the heights.
- **Another project** (#491): the list follows `trackLayout.changed` and, on `project.reset` (`project.new`,
  `project.load`), takes the new project's heights and folded folders from `trackLayout.get` and drops the old ones,
  and the selection: the new project's tracks can have the old ones' ids.
- **Track handling** (pp. 213, 219): Project > Remove Selected Tracks, Remove Empty Tracks, Duplicate Tracks, and in
  the context menu also Disable and Enable Selected Tracks (`track.removeSelected`, `track.removeEmpty`,
  `track.duplicateSelected`, `track.disableSelected`, `track.enableSelected`), each one undo step: Remove Empty
  Tracks is the engine command `track.removeEmpty` (#630), which the menus and key commands run like any engine
  command, the others are UI commands that send a `history.transaction`.
  The engine finds the empty tracks in its current project, so a Lua script or MCP client gets the same result without the UI, and the list's copy of the
  project, which can be behind a change from another client, doesn't decide. Empty tracks are
  audio, instrument, MIDI and marker tracks without events, parts, markers or automation, and folders without tracks,
  unless another track's channel outputs or sends to theirs; group and effect tracks are where others play through,
  VCA tracks control channels, and ruler, tempo and signature tracks hold no events: they stay. What it removes can
  leave other tracks empty, such as an instrument track that only empty MIDI tracks played or a folder (with its group
  channel, nested or not) that held only empty tracks, so it looks again until nothing more goes: one run leaves no
  empty tracks, in one undo step, and none if nothing is empty. It removes a folder with one `track.remove`, which
  takes the tracks inside along, and returns every track removed. Plugins in a channel's slots and an instrument
  track's instrument don't keep a track. It removes the active marker track last, so the marker track that becomes
  active is one that stays (#797).
- **Group Track and Effect Track to Selected Tracks** (pp. 546, 554): the context menu's commands
  `track.addGroupToSelected` and `track.addEffectToSelected` open the add track dialog for a group or effect track
  that the selected tracks play or send into ([add-track.md](add-track.md), [routing.md](routing.md)).
  Remove Selected Tracks asks first when a track it removes holds something (`resources/ui/remove-tracks.rml`): the
  list decides from its copy of the project, by content alone, so a track with events, parts, markers or automation,
  or a folder with such a track inside, warns, whatever plays into it. Remove removes them, Cancel or Escape keeps
  them, and Remove with "Don't show this warning again" checked also turns the user setting `ui.removeTracksWarning`
  (Display Warning before Deleting Non-Empty Tracks, on by default) off, which any client can turn on again with
  `settings.set`. Remove Selected Tracks also removes the active marker track last, as Remove Empty Tracks does, so
  clients only hear of a marker track that stays becoming active (#820).
- **Track presets** (pp. 199, 238-243): the context menu's Add Track from Preset, Save Track Preset and Load Track
  Preset open the track preset dialog for the selected tracks ([track-presets.md](track-presets.md)).
- **Freeze and render in place** (pp. 219-220, 1471-1477): the context menu's Freeze/Unfreeze Selected Tracks
  (`track.freezeSelected`) freezes the selected audio and instrument tracks that aren't frozen, or unfreezes them when
  every one is; a frozen track's row has `.frozen`, which the themes show like a disabled one. Edit > Render in Place
  > Render (with Current Settings) (`track.renderSelected`) renders them in place
  ([render-in-place.md](render-in-place.md)).
- **Key commands.** `AppUi` adds the track list's commands (`TrackList::actions`) as key commands in the project
  window's focus zone, enabled while the list exists and, for those of selected tracks, while tracks are selected,
  so the menus disable them otherwise.

- **Track visibility** (#166): the tracks the Visibility tab hides leave the list and the event display
  ([track-visibility.md](track-visibility.md)).
- **Automation tracks** (#121) have rows below their track's own, which reaches down over them; Show/Hide
  Automation is in the lower left corner of each track with a channel ([automation-tracks.md](automation-tracks.md)).

## Track controls settings

`resources/ui/track-controls.rml` (pp. 189–191), opened by `#track-controls-settings`, the context menu or the UI
command `track.controlsSettings`: the track types with controls on the left (audio, instrument, MIDI, group, effect,
folder and tempo), the hidden and the visible controls of the selected
type, Add, Remove (not for mute and solo), Move Up, Move Down and Reset (the type's controls and controls area width,
from `settings.list`), and below them Controls Area Width (`#controls-area-width`, the selected type's) and Track Name
Width (`#track-name-width`, global), held at the setting's limits. OK saves every type with `settings.set`
`ui.trackControls` and `ui.trackWidths`, which the track list follows; Cancel and Escape drop the changes.

## Not yet

The Show Lanes control and the lanes' headers ([comping.md](comping.md)), write automation (#122),
dividing the track list, track visibility agents and the track type
filter, track presets, track pictures, the lock control, edit channel settings, volume and pan in the track list,
the settings dialog's groups, controls area preview, presets, Reset All and the MIDI tracks' Output Group and
Channel Width (p. 191), the track height presets menu, snap track heights, the preferences dialog's
page for `ui.removeTracksWarning` and `ui.upDownSelectsTracksOnly` (#162), and the ruler and marker tracks' own
controls (#447, #91). Adding a track below the selected one is #448. Folders' group channels, data blocks and Move Selected Tracks to
New Folder are in [folder-tracks.md](folder-tracks.md).

## Checks

- `tests/document_tests.cpp`: which channels mute and solo silence, also through groups and from MIDI tracks to
  instruments, with undo; disabled tracks in the snapshot and which types refuse it; `track.duplicate` with a
  channel, events, automation, MIDI notes, a folder with its routing, undo and the marker track limit.
- `tests/mixer_tests.cpp` renders muted and soloed tracks and the glide; `tests/sequencing_tests.cpp` plays a muted, a
  disabled and a soloed MIDI track into an instrument, and checks that muted, soloed-out and disabled ones send it no
  automation, MIDI volume, pan or program; `tests/dawproject_tests.cpp` saves and loads mute, solo and
  disabled tracks, also from `project.xml` alone.
- `tests/tracklayout_tests.cpp`: `trackLayout.set` setting heights and replacing the folded folders, refusing unknown
  tracks, folded tracks that aren't folders and heights out of range, not an undo step, without tracks that are gone,
  and saved and loaded with the project; a new project starts without.
- `tests/tracklist_tests.cpp` drives the app's project window with `ui::Automation`: rows level with the lanes and
  each type's controls; every control's button and the engine's state both ways; selection with clicks, modifiers,
  Shift-click deselecting, the arrow keys and the key commands; Up, Down and Alt+F2 pressed with the focus in the
  event display and in the list, selecting the first part on the new track or keeping the parts selected on an
  empty one, Shift+Up in the list, and `ui.upDownSelectsTracksOnly`; the warning before removing tracks that aren't
  empty, for a track per kind of content (audio events, audio parts, MIDI parts, markers, automation) and folders for
  the tracks inside, with Cancel, Escape, Remove, one undo step, "Don't show this warning
  again" and `ui.removeTracksWarning` from another client; renaming with Enter and Escape; moving by a drag and undoing it; heights by a
  drag, with Ctrl for all; the wheel; folding from the toggle and the commands, in the event display too; duplicating,
  disabling, enabling and removing from the context menu and the commands; the settings dialog's Add, Remove,
  Move Up and Reset, saved in the user setting and shown in the rows; the divider dragged both ways and to both
  limits, with the rows, the event display and its lanes following, its double click, the width in `ui.layout`, from
  another client and `Docking::setSize`, a size too large for the window, a narrow window, a side zone squeezing the
  main zone, a larger UI scale and the dragged width after a restart; and the track name and each type's controls area
  width from the dialog, its limits, Ctrl-click to the default and Reset, from another client and with defaults for
  what the setting leaves out, the setting's unknown keys kept, the controls on more lines in a narrow controls area at
  any track list width, and the name giving way deep in folders and in a narrow track list; heights, folded folders and
  the selection not taken into a new project's tracks with the same ids, and heights and folded folders back after
  saving and loading the project.
