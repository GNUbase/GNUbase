# Folder tracks

Folder tracks hold other tracks, folders too, and can sum their audio in a group channel of their own (#82, reference
manual pp. 160–161, 221–226). Adding, nesting, moving and folding folders is in [add-track.md](add-track.md) and
[track-list.md](track-list.md); this file has what folders do with the tracks inside them.

## In the engine

- **Where the tracks inside play** (p. 222). The audio of a track in a folder goes to the group channel of the
  nearest folder around it that has one, or to the main mix if none has (the *folder output*). A folder with a group
  channel inside another one plays through the outer one's; the tracks inside it stay with it. A MIDI track's output
  is its instrument and doesn't follow folders.
- **Moving** (`track.move`, and `track.add` with `tracks`): where a moved track's folder output changes, its channel
  and those of the tracks inside it are routed to the new one: into a folder with a group channel they play through
  it, out of it through the main mix. Between places with the same folder output a track keeps its routing, so one
  routed elsewhere by hand stays so. A route that would make a loop is left as it was. The move and the routing are
  one undo step.
- **Turning the group channel on and off** (p. 224): `track.set` `groupChannel`. On, the folder gets a stereo channel
  on its own folder output, and the tracks inside whose folder output it now is play through it. Off, the channel
  goes with its automation lanes, the tracks inside play through their new folder output, and channels routed to it
  from outside are disconnected. `track.add` `groupChannel` adds a folder with one, as before.
- **Move Selected Tracks to New Folder** (p. 223): `track.add` with `type` folder and `tracks` adds the folder at
  `parent` and `index` and moves the tracks into it, in that order, all in one undo step; with `groupChannel` they
  play through the new group channel.
- **Folder colors** (p. 221): with the user setting `editing.applyFolderColor` (off by default), a folder's new color
  goes to every track inside it, and tracks moved into a folder with a color, or added to it without one of their
  own, take its color.
- **Events:** `channel.added` and `channel.removed` for the group channel, `channel.changed` for each rerouted
  channel, `track.changed` for recolored tracks, besides `track.moved` and `track.changed`.
- **DAWproject:** a folder is a track with child tracks; one with a group channel has a channel that the tracks
  inside are routed to, as before (`dawproject.cpp`).

## In the UI

- **Context menu of the track list:** Enable Group Channel and Disable Group Channel for the selected folders
  (`track.enableSelectedGroupChannels`, `track.disableSelectedGroupChannels`, one `history.transaction`), and Show
  Data on Folder Tracks: Always Show Data, Never Show Data and Hide Data When Expanded, menu items that set the user
  setting `ui.folderData` with `settings.set`.
- **Project > Track Folding:** Move Selected Tracks to New Folder and Move Selected Tracks to New Folder with Group
  Channel (`track.moveSelectedToNewFolder`, `track.moveSelectedToNewFolderWithGroupChannel`): one `track.add` with
  the selected tracks, without those inside selected folders and marker tracks, at the place of the first of them.
- **Data blocks** (p. 226): a folder's lane in the event display shows the events and parts of every track inside
  it as blocks (`event-lanes.folder-data`, under the events; `EventDisplay::folderData`), always, never, or only while
  the folder is folded, by `ui.folderData`.
- A folder with a group channel shows the track controls of its channel (mute, solo, read), as a group track does.

## Not yet

Event details instead of data blocks on folders, and automation instead of data on folders with a group channel
(p. 222); mute and solo on folders without a group channel; group editing mode (p. 274); the modified folder icon;
the folder's channel in the mixer, and moving channels into folders there; Deep Track Folding; and the color picker,
with the track colors ticket (#94).

## Checks

- `tests/document_tests.cpp`: moving tracks into and out of folders with and without a group channel, nested ones,
  MIDI tracks and undo; turning a group channel on and off, with nested folders, automation and outside routes;
  `track.add` with `tracks`; folder colors with and without `editing.applyFolderColor`.
- `tests/tracklist_tests.cpp` drives the project window with `ui::Automation`: Move Selected Tracks to New Folder,
  with and without a group channel, the context menu's Enable and Disable Group Channel, and the data blocks by
  `ui.folderData` while the folder is folded and unfolded.
