# Render in place and freeze

Rendering tracks into audio files that play on the timeline (#116, reference manual pp. 219-220, 1471-1477): render
in place puts what a track plays onto a new audio track, freeze plays a rendered file instead of a track's
instrument and plugins. Both render with the offline renderer of the mixdown export ([mixdown.md](mixdown.md)), so a
file is bit-identical to `mixdown.export` of the same channel and range.

## Render points

`mixdown.export`'s `point` renders channels from inside their strip instead of after the fader and panner
(`output`, the default). The channel's strip (`ChannelStrip`) copies its audio at that point into a buffer of the
render (`PreFaderTap`) in every block, which the export reads after the block: `AudioEngine::m_renderTaps` names
the channels and points while the export renders, and the graph is built again with them before and without them
after. Nothing changes on the audio thread otherwise, and no graph but the export's has a tap.

- `source`: what plays on the channel (its events or instrument, the automation), before the pre section and the
  plugin slots: in the strip that runs the source, the source node's on a channel with slots.
- `inserts`: after the slots, before the strip's other sections (gate, compressor, EQ, limiter).
- `strip`: after those, before the fader: where pre-fader sends take the audio.

Files from a point are in the channel's format, so a mono track gives a mono file; from `output`, in the format the
panner makes. `tail` renders up to that many seconds after the range, as quick export does: the file ends after the
last sample a 24-bit file doesn't hold as silence, or at the range's end.

## Freeze

- **`track.freeze`** renders each audio and instrument track from the project's start to the end of its last event
  or note (on an instrument track also those of the MIDI tracks that play it) at `inserts`, with a tail of up to 30 s
  by default, into a 32-bit float WAV file named after the track in the `Freeze` folder next to the project file
  (p. 220), or for a project without a file in the folder its recordings go to. Then the track is frozen: `Track::frozen`
  holds the file, its sample rate, channels and length, saved in the project's GNUbase entry. One undo step.
- **A frozen track** (`compile` in `rendersnapshot.cpp`) plays its file from the project's start as an audio track's
  event, with the media file and event ids of its channel and track, instead of its events, parts and instrument,
  without its plugin slots, its pre section and auto fades, and without the automation of its instrument, plugins and
  MIDI. Its strip's other sections, fader, pan, mute and sends play as before, with their automation (p. 220: level,
  panning, EQ and sends can still be changed). So it null-tests against the live render: the test renders a project
  with a frozen audio track and an instrument track, each with an insert and strip settings, and compares it with the
  live render, before freezing and after unfreezing.
- **A frozen track neither monitors nor records** (p. 220: its controls are locked): its input would play past the
  freeze file, and its takes wouldn't play. Nor do the MIDI tracks that play a frozen instrument track, whose notes
  the freeze file plays (`lockedByFreeze` in `renderinplace.h`). Freezing turns their monitor and record enable
  buttons off in the same step, `channel.set` refuses to turn them on or to route a MIDI track with them on to a
  frozen instrument track, the channels of their link groups or VCA faders don't pass them on to them, and a project
  file that has them on loads with them off.
- **`track.unfreeze`** plays the events, instrument and slots again; the freeze files stay, so freezing and
  unfreezing can be undone. The monitor and record enable buttons stay off.
- **Freeze files move with the project.** The project file refers to them as to its media files: `project.save`
  copies them into the file (into `freeze/`, extracted into the media folder when it loads) unless `embedMedia` is
  false, and then refers to them relative to itself if they are in its folder, so the project's folder can move.
  Each file goes in once, under a name no other one has in any case (`freeze/2-ramp.wav` next to
  `freeze/Ramp.wav`), since entries are looked up in any case and extracted into one folder; one that isn't there
  when the project is saved is referred to by its absolute path. Names that differ only in the case of a non-ASCII
  letter (`Übung.wav` and `übung.wav`) are two entries in the file, which the case-insensitive lookup tells apart,
  but a file system that folds the case of such letters, as those of Windows and macOS do by default, reads as one
  name, so the later one would extract over the earlier. This holds for every embedded file, not only freeze files:
  media files go into `audio/` under names made unique only in the same case, so `audio/Übung.wav` and
  `audio/übung.wav` can both be in a file too. The load asks the file system which paths are one file and gives the
  clash a numbered name of its own (`freeze/2-übung.wav` next to `freeze/Übung.wav`, `audio/2-übung.wav` next to
  `audio/Übung.wav`) there, while a file system that keeps the two apart, as those of Linux do by default, extracts
  both as they are. `project.backup` copies each into the backup's `Freeze` folder once.
- Refused for a track that is frozen already, disabled or has nothing to play, and when `mixdown.export` refuses
  (no device, a device at another sample rate than the project).
- Scrubbing a frozen track reads its freeze file; when undo brings back an earlier freeze, it reads that one.

## Render in place

**`track.renderInPlace`** renders each audio and instrument track into a WAV file (32-bit float by default) in the
project's media folder, named after the track or `name`, and puts it as one event (As One Event, p. 1472) on a new
audio track below the track, named after it with "(Rendered)", in its folder, time base and output. One undo step.

- The range is `start` to `end` (a range selection, p. 1474), or from the start of the track's first event or note
  to the end of its last; `tail` adds seconds after it (default 0).
- `processing` (p. 1472): `dry` renders at `source`, and the new track gets the channel's plugin slots (copies, with
  their state), strip sections, fader, pan and sends; `channelSettings` (the default) renders at `strip`, and the new
  track gets the fader, pan and sends; `completeSignalPath` renders at `output`, so a mono track panned on a stereo
  bus gives a stereo track, with no settings of its own.
- `sourceTracks` (p. 1473): `keep` (the default), `mute`, `disable` or `remove` the tracks it rendered.
- Refused for a disabled track; `dry` for a frozen track, whose freeze file is already through the slots that `dry`
  copies onto the new track (unfreeze it first); and `completeSignalPath` for a silent track, muted or silenced by
  another track's solo, since the complete signal path ends after the mute (unmute it, or solo it too). Before the
  fader, mute and solo don't matter. If a step fails after rendering, the rendered files are removed.

## In the UI

- Edit > Render in Place > Render (with Current Settings) (`track.renderSelected`) and Edit > Freeze/Unfreeze >
  Selected Tracks (with Current Settings) (`track.freezeSelected`), which is also Freeze/Unfreeze Selected Tracks in
  the track list's context menu ([track-list.md](track-list.md)). Both work on the selected audio and instrument
  tracks with the commands' defaults; freezing unfreezes them when every one is frozen. A frozen track's row has
  `.frozen`.

## Not yet

- The Render Tracks, Render Selection, Freeze Channel Options and Unfreeze Channel Options dialogs, with options that
  are saved for the next time; Freeze Channel in the inspector and the mixer, and the lit freeze buttons.
- Locking a frozen track's events, parts and controls (p. 220); edits of them play once the track is unfrozen.
- As Separate Events and As Block Events, rendering events or parts selected in the event display, Mix down to One
  Audio File and Hide Source Tracks (pp. 1472-1473); Complete Signal Path + Master FX, and groups and effect returns
  in the complete signal path; copying the track's automation to the new track.
- Freezing only the instrument (p. 991) or the inserts before a given slot (p. 548), freezing external instruments
  and effects, and removing freeze files when unfreezing.
- Plugin delay compensation (#71): a freeze file rendered at `inserts` holds its slots' latency, so once latency is
  compensated it has to play earlier by the latency of the slots it replaces.
- A frozen track's pre section and cut filters glide from their old settings over the first block after freezing.
