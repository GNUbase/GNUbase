# Cycle recording and takes

Recording in a cycle (#98, reference manual pp. 338, 344–345, 352–353, and p. 226 on lanes): as soon as the cursor
reaches the right locator, it jumps back to the left one and recording goes on in a new lap. It builds on audio
recording ([recording.md](recording.md)) and MIDI recording ([midi-recording.md](midi-recording.md)), which already
make each lap a take of its own: a run of consecutive project positions.

- **Lanes.** Audio events, audio parts and MIDI parts have a `lane`, from 0 (the default), which `project.get`,
  `event.get` and the events' announcements report, `event.set` with `lane` changes (as one undo step, up to lane
  1000), splits and copies keep, and the project file keeps in GNUbase's entry (files from before have every event on
  lane 0). A cycle recording puts its takes on lanes of their own, one per take, from the lane after the highest one
  that the track's events and parts where the takes lie are on (from 0 where there are none). Lanes don't change what
  plays: an audio track plays the event added last where events overlap, so the last lap plays; MIDI tracks play
  every part that isn't muted. Showing lanes, bringing takes to the front and comping are #99.
- **Audio** (p. 345). Every lap of an audio cycle recording is a take of the one file, and `recording.add` adds an
  audio event per take (a four-lap cycle recording makes four events). With the audio record modes that keep the
  takes, Keep History and Cycle History + Replace, the takes go onto lanes; Replace keeps only what the last lap
  recorded where laps overlap, so its events stay on lane 0, as do the events of a recording of one take. The
  acceptance test records four laps through the dummy device in both modes and finds four events playing the four
  runs of the file, on lanes 0 to 3, the last one added last (`tests/recording_tests.cpp`).
- **MIDI cycle record modes** (pp. 352–353). `transport.set` with `midiCycleRecordMode`, which `transport.get`
  reports and the Transport menu's MIDI Record Mode submenu sets below the MIDI record modes, decides what becomes of
  a MIDI cycle recording's laps. The recorder marks each note and controller event with the take it was recorded in
  (`take` in `recording.add`'s notes and controllers), and `recording.add` gets the mode as `cycleMode`:
  - `mix` (the default) puts every lap into one part, by the MIDI record mode.
  - `overwrite`: input in a lap overwrites what the laps before it recorded from its first note or controller event to
    the lap's end; a lap without input overwrites nothing. What is left goes into one part, as with mix.
  - `keepLast`: the last complete lap that recorded anything replaces the others. The last take is complete if it
    ends where an earlier one ends (at the right locator), not where recording stopped; if recording stops in a lap,
    or a lap records nothing, the lap before it stays. If only an incomplete lap recorded anything, it stays.
  - `stacked`: each lap that recorded anything becomes a part of its own, over the lap (whole bars with
    `midi.snapPartsToBars`), each on a lane of its own; all but the last are muted. The parts are new parts whatever
    the MIDI record mode, so merge and replace don't apply.
  - `mixStacked` (Mix-Stacked (No Mute)): the same, without muting.

  Tests: `tests/midirecording_tests.cpp` records four laps in the stacked modes and checks the parts, lanes and mutes,
  records Keep Last through the dummy device with a lap without input and one where recording stopped, and checks
  every mode through `recording.add`; `tests/mainmenu_tests.cpp` sets the mode from the menu.
- **Real time.** Nothing changes on the audio thread: the recorder's disk thread numbers the takes as it already
  splits them, and a take that got no frames before the next one started gives its place to it, so the numbers are
  those of the takes `recording.add` gets.

## Not yet

Regions per lap on the recorded clip (the reference divides a cycle recording's audio event into regions, one per
take, p. 225), which needs regions in the pool; showing lanes, choosing takes and comping (#99); lanes in the
DAWproject file's own description (GNUbase's entry keeps them); the MIDI record modes for stacked recordings;
inserting a retrospective recording as a cycle recording (p. 355).
