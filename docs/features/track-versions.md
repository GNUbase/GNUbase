# Track versions

Several versions of the events and parts of a track, of which one is active (#165, reference manual pp. 230-237):
new and duplicated versions, renaming and deleting them, version IDs that tie versions on several tracks together,
and activating versions across tracks.

## Engine

- **The model.** Audio, instrument and MIDI tracks have versions (`TrackVersion` in `document.h`): each has an ID and
  a name, and every track starts with v1, ID 1, which it doesn't store until it gets a second one
  (`Track::versions` empty, `versionsOf`). The active version's events and parts are the project's, as before; an
  inactive version holds its own audio events, audio parts and MIDI parts in the track, as they were when it was last
  active. So playback, the render snapshot, the editors, the event display and every `event.*` command only ever see
  the active version, and nothing else had to learn about versions. Automation, markers and the channel belong to the
  track, not a version (p. 230: automation tracks have no versions).
- **IDs.** Versions made together, by one `trackVersion.add` or `trackVersion.duplicate` on several tracks, share an
  ID one higher than any version's in the project; `trackVersion.assignCommonId` gives the active versions of tracks a
  new common ID (p. 233). `trackVersion.activate` activates the version with an ID on each given track that has one,
  so versions with one ID switch together (p. 234).
- **Commands**, each one undo step named after the reference's menu item, on `tracks` (audio, instrument and MIDI
  tracks; other types fail): `trackVersion.add` (New Version, an empty version, active), `trackVersion.duplicate`
  (Duplicate Version: a copy of the active one's events and parts with new ids, audio events in the copies of their
  parts, joined by copied crossfades, in the same stack order), `trackVersion.set` (Rename Version, of the active
  versions), `trackVersion.remove` (Delete Version: the active version goes with its events and parts and the one
  before it becomes active, the next one for the first; tracks with only one version keep it), `trackVersion.activate`
  and `trackVersion.assignCommonId`. New versions are named vN, N one more than the track's number of versions, or the
  next number no version of the track is named (p. 236). Switching emits the removal and addition of the events and
  parts that go and come, and `track.changed`. A track a command leaves alone keeps storing no version of its own:
  `trackVersion.activate` on a track without that ID and `trackVersion.remove` on a track with a single version skip
  it, so it stays out of the undo step, out of the `hasTrackVersion` rule property
  ([project-rules.md](project-rules.md)) and out of what the commands return; a track whose only version is named v1
  again stores none either.
- **`project.get` and the project file.** Each such track has `versions` (`[{id, name}]`) and `activeVersion`;
  `versionContents` lists what the inactive versions hold (`[{track, version, audioEvents, audioParts, midiParts}]`),
  which GNUbase's entry of the DAWproject file keeps. Other applications see the active versions only. Ids loaded go
  on after the highest one, inactive versions' too.
- **What else knows about inactive versions.** Their audio events use their clips: the pool counts them
  (`pool.removeUnused`, a clip's uses, `clip.remove` refuses), `media.removeMissing` removes them with their clips,
  and backups keep and minimize their media (`forEveryAudioEvent`). `media.relinkResampled`, including when called by
  `media.conform`, rescales their audio events and crossfades. `track.duplicate` copies every version with ids of
  its own, `track.removeEmpty` keeps a track with more than one version, and the track list's warning before Remove
  Selected Tracks counts them too. A new time base or sample rate moves inactive versions' events and parts as it
  moves the active ones. A tempo change retimes notes and controllers in their linear MIDI parts. A MIDI part that
  goes into an inactive version stops being a shared copy of the parts that play, since only the active parts are
  edited together ([parts-and-events.md](parts-and-events.md)); a group every copy of which goes into the version
  stays one, with its group and its note and controller ids, and edits reach all of them again once it is active.
- **One visitor for the rewrites that reach every event and part.** `forEveryEventAndPart` (`document.h`) calls three
  visitors with every audio event, audio part and MIDI part of the project, those that inactive versions hold too,
  and tells each visitor whether its item plays, so that a rewrite announces only what there is to see;
  `forEveryAudioEvent` is its audio events alone. A new time base (`rebaseTrack`), a new sample rate
  (`changeSampleRate`), a tempo change (`tempoChanged`) and resampled media (`rescaleUses`) go through them, so none
  of them can miss an inactive version.

Tests: `tests/trackversions_tests.cpp` renders a track whose new version is empty and whose first one plays again
once activated, across two tracks with one ID, and checks duplicating, names, deleting, common IDs, undo, the project
file, `track.duplicate`, a new time base, the pool, resampled media relinking, tempo changes, the shared copies a
version keeps and the tracks a command skips.

## UI

- **Project > Track Versions** works on the selected audio, instrument and MIDI tracks: New Version, Duplicate
  Version, Rename Version..., Delete Version, Select Tracks with Same Version ID (the tracks that have a version with
  the ID of the first selected track's active one), Assign Common Version ID, and Activate Version, which
  `ui::TrackList` fills with the first selected track's versions, the active one checked; a click activates that
  version's ID on them all. Rename Version opens `resources/ui/rename-version.rml` (p. 236), which shows the first
  selected track's active version's name ready to be typed over, and OK sends `trackVersion.set` for every selected
  track. The UI commands are `trackVersions.new`, `trackVersions.duplicate`, `trackVersions.rename`,
  `trackVersions.remove`, `trackVersions.selectSameId` and `trackVersions.assignCommonId`: their own category in the
  key commands dialog (p. 231, [key-commands.md](key-commands.md)), enabled while an audio, instrument or MIDI track
  is selected.
- **The Track Versions pop-up menu** (p. 231) opens from the arrow `.track-versions-toggle` to the right of the name
  of every track that has versions; the arrow leaves the selection alone, so that the menu's functions work on every
  selected track (p. 234), and selects the track it is on when the selection doesn't have it. `ui::TrackList` fills
  `#track-versions-menu` with that track's versions, the active one checked and a click activating it on them all,
  and below them New Version, Duplicate Version, Rename Version, Delete Version (disabled with only one version),
  Select Tracks with Same Version ID and Show Version Name in Track List.
- **Show Version Name in Track List** is the user setting `ui.showVersionName`, on by default: while it is on, the
  track list shows the active version's name after the track's name (`.track-version`) as long as the track has more
  than one version (p. 236).
- **The inspector's Track Versions section** (pp. 231-232, [inspector.md](inspector.md)) shows the first selected
  track's active version: its name (`textdisplay[control=versionName]`), which a double click edits for every
  selected track, and its ID, an indicator while the track has more than one version, the list of them all, in which
  a click activates one on every selected track, and New, Duplicate and Delete, which work on them all. Tracks
  without versions get the section as unavailable.

Tests: `tests/trackversions_ui_tests.cpp` makes a version on two selected tracks through the menu, sees its name in
the list, activates the first version on both through Activate Version, selects by version ID and deletes versions;
opens the pop-up menu at a track's name, makes a version from it, activates one from its list and switches the version
name in the track list off and on again; renames versions through the dialog and through the inspector's section, and
activates, adds, duplicates and deletes them there; and finds the commands as a key command category of their own.

## Not yet

Versions of the tempo and signature tracks (p. 230), and of chord, sampler and video tracks, which GNUbase doesn't
have yet; Create Lanes from Versions and Create Versions from Lanes (p. 237); track archives. Pasting between versions
works as copy and paste do, since only the active version's events are there.
