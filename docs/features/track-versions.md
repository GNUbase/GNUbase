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
  parts that go and come, and `track.changed`.
- **`project.get` and the project file.** Each such track has `versions` (`[{id, name}]`) and `activeVersion`;
  `versionContents` lists what the inactive versions hold (`[{track, version, audioEvents, audioParts, midiParts}]`),
  which GNUbase's entry of the DAWproject file keeps. Other applications see the active versions only. Ids loaded go
  on after the highest one, inactive versions' too.
- **What else knows about inactive versions.** Their audio events use their clips: the pool counts them
  (`pool.removeUnused`, a clip's uses, `clip.remove` refuses), `media.removeMissing` removes them with their clips,
  and backups keep and minimize their media (`forEveryAudioEvent`). `track.duplicate` copies every version with ids of
  its own, `track.removeEmpty` keeps a track with more than one version, and the track list's warning before Remove
  Selected Tracks counts them too. A new time base or sample rate moves inactive versions' events and parts as it
  moves the active ones. MIDI parts that go into an inactive version stop being shared copies of the parts that play.

Tests: `tests/trackversions_tests.cpp` renders a track whose new version is empty and whose first one plays again
once activated, across two tracks with one ID, and checks duplicating, names, deleting, common IDs, undo, the project
file, `track.duplicate`, a new time base and the pool.

## UI

- **Project > Track Versions** works on the selected audio, instrument and MIDI tracks: New Version, Duplicate
  Version, Delete Version, Select Tracks with Same Version ID (the tracks that have a version with the ID of the first
  selected track's active one), Assign Common Version ID, and Activate Version, which `ui::TrackList` fills with the
  first selected track's versions, the active one checked; a click activates that version's ID on them all. The UI
  commands are `track.newVersion`, `track.duplicateVersion`, `track.deleteVersion`, `track.selectSameVersionId` and
  `track.assignCommonVersionId`.
- **The track list** shows the active version's name after the track's name (`.track-version`) while the track has
  more than one version (p. 236).

Tests: `tests/trackversions_ui_tests.cpp` makes a version on two selected tracks through the menu, sees its name in
the list, activates the first version on both through Activate Version, selects by version ID and deletes versions.

## Not yet

The Track Versions pop-up menu at the track name and the inspector's Track Versions section (pp. 231-232), with
Rename Version's dialog (the engine has `trackVersion.set`) and Show Version Name in Track List; versions of the tempo
and signature tracks (p. 230), and of chord, sampler and video tracks, which GNUbase doesn't have yet; Create Lanes
from Versions and Create Versions from Lanes (p. 237); the Track Versions key command category; track archives.
Pasting between versions works as copy and paste do, since only the active version's events are there. Pointing a
media file at a file at another sample rate rescales only the active versions' events.
