# Render in place and freeze

Rendering tracks into audio files that play on the timeline (#116, reference manual pp. 219-220, 1471-1477): render in
place puts what a track plays onto a new audio track, freeze plays a rendered file instead of a track's instrument and
plugins. Both render with the offline renderer of the mixdown export ([mixdown.md](mixdown.md)), so a file is
bit-identical to `mixdown.export` of the same channel and range.

## Background jobs and track locks

Freeze and render in place use the same background jobs as backups and media conforming (#1794). Each command validates
its request, takes a render snapshot and locks the affected tracks before returning `{job}`. The private renderer works
on the job thread; the command thread remains available to every client. The lock protects rendered input, including the
source track, its inserts, channel settings, intersecting audio events, clips and media, MIDI parts and tracks routed to
a rendered instrument. Commands that change protected input fail atomically with "being rendered", including
transactions and undo/redo. Other tracks and audio events outside the rendered range remain editable, as do track
version names and channel input, MIDI input and input-transformer settings, which only affect live input. The lock check
reuses the edit or undo step's object difference and compares render input only when a relevant object or project-wide
render setting changes. A track being rendered cannot be deleted or moved, and replacing the project is refused until
its track jobs end. Read-only queries and saving remain available.

On completion the job posts its project edit to the command thread: freezing the sources or adding all rendered tracks
and applying the source-track option is one undo step, named "Freeze" or "Render in place", after any edits made
elsewhere while it ran. The render lock is temporary state, outside the project file and undo history. Completion,
failure and cancellation release it; failure and cancellation also remove the files made by the job. `track.renderLocks`
reports active locks and job ids; `track.renderingChanged` announces `{tracks, job, rendering}`. The UI follows these
through the engine API, styles track rows with `.rendering` in each generated theme's RCSS, and shows nonmodal progress
with Cancel. Closing progress also cancels its job, using the backup progress lifecycle. A track job owns its prepared
work and sample leases until it exits, so engine shutdown joins it and releases those leases before destroying the live
audio engine's sample loader.

`track.freeze` and `track.renderInPlace` are command version 2. Their names and parameters stay, but they return `{job}`
immediately. The completed job's `result` keeps the former synchronous result: `{tracks}` for freeze, `{tracks, media,
events}` for render in place. Lua and MCP callers obtain it through `job.get` or `job.finished`; `job.cancel` requests
cancellation. `track.unfreeze` retains its synchronous result. The job-starting commands cannot be nested in
`history.transaction`; each job applies its own single undo step when it completes.

## Render points

`mixdown.export`'s `point` renders channels from inside their strip instead of after the fader and panner (`output`, the
default). The channel's strip (`ChannelStrip`) copies its audio at that point into a buffer of the render
(`PreFaderTap`) in every block, which the export reads after the block: `AudioEngine::m_renderTaps` names the channels
and points while the export renders, and the graph is built again with them before and without them after. Nothing
changes on the audio thread otherwise, and no graph but the export's has a tap.

- `source`: what plays on the channel (its events or instrument, the automation), before the pre section and the
  plugin slots: in the strip that runs the source, the source node's on a channel with slots.
- `inserts`: after the slots, before the strip's other sections (gate, compressor, EQ, limiter).
- `strip`: after those, before the fader: where pre-fader sends take the audio.
- `panner`: after the panner, without the fader and the mute (a mixdown with Channel Settings): the fader node's
  `ChannelFader` pans a copy of the strip's audio into the tap with the panner's gains alone, frame by frame as it pans
  the channel, so pan automation plays in it too.

Files from a point are in the channel's format, so a mono track gives a mono file; from `panner` and `output`, in the
format the panner makes. `tail` renders up to that many seconds after the range, as quick export does: the file ends after the last
sample a 24-bit file doesn't hold as silence, or at the range's end.

## Freeze

- **`track.freeze`** renders each audio and instrument track from the project's start to the end of its last event or
  note (on an instrument track also those of the MIDI tracks that play it) at `inserts`, with a tail of up to 30 s by
  default, into a 32-bit float WAV file named after the track in the `Freeze` folder next to the project file (p. 220),
  or for a project without a file in the folder its recordings go to. Then the track is frozen: `Track::frozen` holds
  the file, its sample rate, channels and length, saved in the project's GNUbase entry. One undo step.
- **`includeInserts: false`** (Include Inserts for Instrument Tracks off, p. 992) renders an instrument track at
  `source`: the file holds the instrument only, and `frozen.inserts` is false. The frozen track then plays its pre
  section and plugin slots on the file, with their automation, so they stay editable and the track null-tests against
  the live render (tested). An audio track's file holds its slots in any case.
- **A frozen track** (`compile` in `rendersnapshot.cpp`) plays its file from the project's start as an audio track's
  event, with the media file and event ids of its channel and track, instead of its events, parts and instrument,
  without its plugin slots, its pre section (both only if the file holds them) and auto fades, and without the
  automation of its instrument, MIDI and what the file holds. Its strip's other sections, fader, pan, mute and sends
  play as before, with their automation (p. 220: level, panning, EQ and sends can still be changed). So it null-tests
  against the live render: the test renders a project with a frozen audio track and an instrument track, each with an
  insert and strip settings, and compares it with the live render, before freezing and after unfreezing.
- **A frozen track neither monitors nor records** (p. 220: its controls are locked): its input would play past the
  freeze file, and its takes wouldn't play. Nor do the MIDI tracks that play a frozen instrument track, whose notes
  the freeze file plays (`lockedByFreeze` in `renderinplace.h`). Freezing turns their monitor and record enable
  buttons off in the same step, `channel.set` refuses to turn them on or to route a MIDI track with them on to a
  frozen instrument track, the channels of their link groups or VCA faders don't pass them on to them, and a project
  file that has them on loads with them off.
- **`track.unfreeze`** plays the events, instrument and slots again; the freeze files stay, so freezing and
  unfreezing can be undone. The monitor and record enable buttons stay off.
- **Freeze files move with the project.** The project file refers to them as to its media files: `project.save` copies
  them into the file (into `freeze/`, extracted into the media folder when it loads) unless `embedMedia` is false, and
  then refers to them relative to itself if they are in its folder, so the project's folder can move. Each file goes in
  once, under a name no other one has in any case (`freeze/2-ramp.wav` next to `freeze/Ramp.wav`), since entries are
  looked up in any case and extracted into one folder; one that isn't there when the project is saved is referred to by
  its absolute path, and so is a media file (#1567); the save's `missing` lists it after the media files (#1736), and
  one that the file system can't say is there fails the save. A load extracts over no file that is there with other
  content: the entry goes beside it under a numbered name, and a file with the entry's content already is used as it is.
  Names that differ only in the case of a non-ASCII letter (`Übung.wav` and `übung.wav`) are two entries in the file,
  which the case-insensitive lookup tells apart, but a file system that folds the case of such letters, as those of
  Windows and macOS do by default, reads as one name, so the later one would extract over the earlier. This holds for
  every embedded file, not only freeze files: media files go into `audio/` under names made unique in any case of ASCII
  letters (`audio/<id>-kick.wav` next to `audio/Kick.wav`, #871), so `audio/Übung.wav` and `audio/übung.wav` can both be
  in a file too. The load asks the file system which paths are one file and gives the clash a numbered name of its own
  (`freeze/2-übung.wav` next to `freeze/Übung.wav`, `audio/2-übung.wav` next to `audio/Übung.wav`) there, while a file
  system that keeps the two apart, as those of Linux do by default, extracts both as they are. `project.backup` copies
  each into the backup's `Freeze` folder once. `media.prepareArchive` copies the freeze files from outside the project's
  folder into its `Freeze` folder, each once, and the first save of a project frozen without a file copies them there
  from the folder its recordings go to; both point the tracks at the copies with `track.relinkFreeze`, in the undo step
  that relinks the media (#871).
- Refused for a track that is frozen already, disabled or has nothing to play, and when `mixdown.export` refuses
  (no device, a device at another sample rate than the project).
- Scrubbing a frozen track reads its freeze file; when undo brings back an earlier freeze, it reads that one.

## Render in place

**`track.renderInPlace`** renders each audio and instrument track into a WAV file (32-bit float by default) in the
project's media folder, named after the track or `name`, and puts it as one event (As One Event, p. 1472) on a new audio
track below the track, named after it with "(Rendered)", in its folder, time base and output. One undo step.

- The range is `start` to `end` (a range selection, p. 1474), or from the start of the track's first event or note
  to the end of its last; `tail` adds seconds after it (default 0), or `tailBars` bars (Tail Mode in bars and beats,
  p. 1473): of the time signature where the range ends, at the tempo there and after, so 0.5 is two beats of 4/4.
- `folder` (File Path, p. 1473) renders into that folder, an absolute path, instead of the media folder; it is made if
  it isn't there.
- `mixdown` (Mix down to One Audio File, p. 1473) renders several tracks into one file on one new audio track below
  the first, named `name` or "Mixdown" with "(Rendered)", whose file has the name with the characters that file names
  can't have turned into "_" ("AC/DC: live" writes "AC_DC_ live.wav"): each track renders alone from the start of the earliest range
  to the end of the latest, in 32-bit float, and the job sums the files in the tracks' order into the file, in
  `sampleFormat` (integer formats rounded, without dither), as long as the longest, then removes them. The source
  options apply to every track. It needs `oneEvent`, tracks that play through one output, whose format the file has,
  and processing that isn't `dry` (p. 280). With `completeSignalPath` the file is the sum of the tracks' renders, and
  the main mix stays as it was with the sources muted (tested). With `channelSettings` one new track can't take the
  fader and pan of several tracks: each track renders at `panner`, after its panner without its fader and mute, and the
  new track gets no settings of its own, centred at 0 dB without sends (tested: the same as the complete signal paths
  with the faders at 0 dB). While it renders, the tracks' pans are locked too.
- `mode` (pp. 1472, 1475): `oneEvent` (the default) combines the input into one file; `separateEvents` writes a file per
  audio event, audio part or MIDI part; `blockEvents` combines adjacent or overlapping events and parts into one file
  per block. All files of a source go onto one destination track, in time order, each at its source range's start. Parts
  use their whole boundaries, including silence before or after their notes. Each file renders only its own input,
  including during its tail, so another event cannot play into that tail. A source-track option applies once per source,
  and all destination tracks, files and events belong to the same undo step. MIDI parts feeding the instrument through
  either a MIDI output or a MIDI send participate in the grouping, using the same routing check as the render input
  locks.
- `events` optionally selects audio event, audio part and MIDI part ids on the tracks in `tracks`. Only that material
  renders, including where unselected material overlaps it; `start` and `end`, if given, clip the selected ranges. The
  selection must be nonempty, without duplicate or unknown ids, and each requested track must have material in the
  range. Selected events and separate/block modes require unfrozen tracks. Render in the Edit menu passes the event
  display's selection (see In the UI).
- `processing` (p. 1472): `dry` renders at `source`, and the new track gets the channel's plugin slots (copies, with
  their state), strip sections, fader, pan and sends; `channelSettings` (the default) renders at `strip`, and the new
  track gets the fader, pan and sends; `completeSignalPath` renders at `output`, so a mono track panned on a stereo
  bus gives a stereo track, with no settings of its own.
- `sourceTracks` (p. 1473): `keep` (the default), `mute`, `disable` or `remove` the tracks it rendered.
  `hideSourceTracks` (p. 1473) hides them in the project window, unless they are removed, with `trackVisibility.set`,
  whose changes are settings, not undo steps: undoing the render leaves them hidden. `sourceEvents: mute` (p. 1476,
  default `keep`) mutes the selected events and parts in the render's undo step, so it needs `events` (tested).
- The new event holds the global transposition it was rendered with (#1368, [transpose.md](transpose.md)), and gets
  the root key and Global Transpose that a recorded event gets (p. 420): without transpose events the project root
  key, if it has one, and Follow, so that changing the project root key afterwards shifts it by the step from the old
  key; with transpose events no root key and Independent, since following them it would be transposed again (tested).
- Refused for a disabled track; `dry` for a frozen track, whose freeze file is already through the slots that `dry`
  copies onto the new track (unfreeze it first); and `completeSignalPath` for a silent track, muted or silenced by
  another track's solo, since the complete signal path ends after the mute (unmute it, or solo it too). Before the
  fader, mute and solo don't matter. If a step fails after rendering, the rendered files are removed.

## In the UI

- Edit > Render in Place > Render (with Current Settings) (`track.renderSelected`) and Edit > Freeze/Unfreeze >
  Selected Tracks (with Current Settings) (`track.freezeSelected`). Both use the options of the dialogs below, the user
  settings `render.options` (empty, the commands' defaults, at first), `render.freezeTail` (30 s) and
  `render.freezeInserts` (on); freezing works
  on the selected audio and instrument tracks and unfreezes them when every one is frozen. A frozen track's row has
  `.frozen`.
- Edit > Render in Place > Render Settings... (`track.renderSettings`, pp. 1471-1477) opens Render Selection for
  selected events and parts or a selection range, else Render Tracks (`render-settings.rml`), with Mode, Processing,
  Tail Mode (Bars & Beats is `tailBars`, Seconds `tail`, Off neither; Off at first) and Tail Size, Bit Depth, Use
  Custom Name and Name, File Path (`folder`, empty for the media folder; its button chooses one in a folder dialog),
  for several tracks Mix down to One Audio File (`mixdown`), in Render Tracks Source Tracks and Hide Source Tracks,
  and in Render Selection of events and parts Source Events. Render saves them in `render.options` and renders. A
  selection never changes its tracks: Render Selection has no Source Tracks and Hide Source Tracks, and Render (with
  Current Settings) leaves out the saved ones for a selection, and the saved Source Events for anything but selected
  events and parts (tested). A mixdown renders one event, so it renders without the saved Mode, and only several
  tracks that don't render dry mix down (p. 1473): for one track, or with Dry, the saved Mix down to One Audio File is
  left out (tested).
- Edit > Freeze/Unfreeze > Selected Tracks... (`track.freezeSelectedSettings`, p. 992), which is also Freeze/Unfreeze
  Selected Tracks in the track list's context menu (pp. 219-220, [track-list.md](track-list.md)), opens Freeze Channel
  Options (`freeze-options.rml`) with Tail Size and Include Inserts for Instrument Tracks for the selected tracks that
  aren't frozen, saves them in `render.freezeTail` and `render.freezeInserts` and freezes them; when every one is frozen
  it unfreezes them, as there are no unfreeze options yet.
- Render renders what the event display selects (pp. 1474-1477): the selected audio events, audio parts and MIDI parts
  on audio and instrument tracks (`events`, each on its own track); without them, the selection range on its audio and
  instrument tracks (`start` and `end`); without that, the selected tracks. It is enabled while any of them is
  selected. Events selected only on other tracks, such as MIDI tracks, render nothing, so that a selection never falls
  back to whole tracks.

## Not yet

- Mute Source Events for a selection range; the Unfreeze Channel Options dialog;
  Render Tracks for MIDI tracks (pp. 1471, 1474), which Render skips; Freeze Channel in the inspector and the mixer,
  and the lit freeze buttons.
- Unload Instruments When Frozen (p. 993) as an option: a frozen track's channel has no instrument in the render
  snapshot, so its built-in instrument isn't made and lets go of its samples in any case.
- Locking a frozen track's events, parts and controls (p. 220); edits of them play once the track is unfrozen.
- Complete Signal Path + Master FX, and groups and effect returns in the complete signal path; copying the track's
  automation to the new track.
- Freezing external instruments and effects, and removing freeze files when unfreezing (p. 993), which the undo of
  the unfreeze would need again.
- Plugin delay compensation (#71): a freeze file rendered at `inserts` holds its slots' latency, so once latency is
  compensated it has to play earlier by the latency of the slots it replaces.
- A frozen track's pre section and cut filters glide from their old settings over the first block after freezing.

## Tests

`tests/renderinplace_tests.cpp` checks the rendered audio, source options, freeze persistence, unfreeze and undo. Its
background-job tests hold jobs at progress while another client edits: protected edits and transactions are refused,
unrelated edits and events outside the range succeed, completion adds one undo step, and cancellation or failure unlocks
without a project change. The mixdown snapshot test edits and removes the exported source while the job runs and
compares the result with the starting audio. An instrument track frozen without its inserts null-tests against the
live render, plays its slot on the freeze file and keeps `inserts` in the project file; Mute Source Events and Hide
Source Tracks are checked with their undo. A tail in bars is as long as the same tail in seconds in 4/4 and follows a
3/4 signature, a folder of its own that isn't there yet gets the file, and a mixdown is the sum of the tracks'
complete signal paths, on one track, without the tracks' own files, in float32 and in int24 within its rounding, and
named after its name made a file name; with Channel Settings, it holds the pans, not the faders and mutes, and its track
is centred at 0 dB without sends. `tests/tracklist_tests.cpp` renders a selected MIDI part, a selection
range and, with neither, the selected track through the Edit menu's Render, and checks that the render and freeze
dialogs' options reach the commands, are saved for the commands with current settings, and leave a selection's tracks
as they are; Tail Mode in bars, a File Path chosen in the folder dialog and Mix down to One Audio File render one file
there, and one track renders without the mixdown. `tests/mixdown_ui_tests.cpp` uses the real app's automation API to check the rendering state class,
progress and Cancel for all three background render commands.

A shutdown regression in `tests/renderinplace_tests.cpp` stops command posting during an active multi-track freeze or
render in place, checking that the job releases sample leases before the live loader is destroyed and removes its
partial files.

The follow-up regressions in `tests/renderinplace_tests.cpp` check the completed undo names, version renaming and live
input changes during a render, and refusal of undo and redo that would change a locked track's pre gain, preserving both
project and history. They also check that newly routed MIDI and newly inserted plugins remain protected.
`tests/theme_tests.cpp` checks the rendering-state style in every generated theme.

The selection regressions in `tests/renderinplace_tests.cpp` check separate files and adjacent blocks on one destination
track, their positions and audio, distinct file names, source disabling and removal, undo/redo, exclusion of overlapping
unselected audio and MIDI parts, full MIDI part boundaries, later events excluded from tails, and invalid selections
rejected before a job starts. They also null-test separate dry renders with their copied effects and check that a
failure in the second file removes the first without an edit or remaining locks. Outside Windows, a save with a freeze
file in a folder that cannot be searched must fail without changing the existing project file or leaving a `.saving`
file. The UI regression in `tests/projectfiles_tests.cpp` checks that Save, Save As and Save New Version name media and
freeze files in the missing-files notice. A MIDI send regression compares separate files and overlapping blocks with
direct MIDI routing, checking that the send parts get their files and remain audible in a combined block.
