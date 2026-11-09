# Routing and sends

Where a channel's audio goes besides its output (#108, reference manual pp. 35–53, 149–152, 541–557): sends to
group and effect channels, before or after the fader, and routing that never closes a loop. Outputs and groups are
in [busses.md](busses.md) and [folder-tracks.md](folder-tracks.md), the fader in [mixer.md](mixer.md).

## In the engine

- **Sends** (pp. 552–557). Every audio channel (audio, instrument, group and effect channels, folders' group
  channels) has eight sends, `Channel::sends` in `src/engine/document/document.h`, in `project.get` as its `sends`
  array; MIDI channels have none (an empty array). Each has a `destination` (a group or effect channel, 0 for none),
  `on`, a `level` in dB from -144 (silence) to the project's maximum volume, and `preFader`. A send starts off, at
  0 dB, after the fader and without a destination, as in the reference, where a send is switched on after choosing
  its destination (p. 555).
- **`channel.set` `sends`** changes them: a list of objects, each with its `send` number (1 to 8) and only what it
  changes, like the equalizer's bands. The level stops at its ends, like the fader; a lower maximum volume
  (`project.set` `maxVolume`) brings send levels above it down in the same undo step. Every change is an undo step.
- **Outputs to effect channels.** A channel's output can be an output bus, a group channel or an effect channel
  (`channel.set` `output`), so an effect channel can play into another one, as in the reference (p. 553); DAWproject
  files keep such outputs.
- **Group Track and Effect Track to Selected Tracks** (pp. 546, 554). `track.add` with `tracks` for a group track
  routes those tracks' outputs to the new group channel; for an effect track, each of them sends to the new effect
  channel with the send given by `send` (Add Effect Track to Send, p. 553), or else its first send without a
  destination, switched on, after the fader, at the user setting `audio.defaultSendLevel`. All in one undo step.
  Tracks that can't be routed are left out, as the reference routes those that can: tracks without an audio channel
  (MIDI, marker and ruler tracks, VCA tracks, folders without a group channel) and, without `send`, tracks whose eight
  sends all have a destination. A selected folder stands for its group channel, not for the tracks inside it, which
  are routed only when they are selected themselves. Routing that would close a loop refuses the whole command.
- **The project's Group and FX folders** (pp. 150-152). `track.add` `projectFolder` puts a group or effect track into
  the first folder at the top level named Group or FX, which it adds at the end of the project if there is none, in
  the same undo step. The folder is found by its name, so after it is renamed the next such track gets a new one.
- **Default send level** (p. 552). The user setting `audio.defaultSendLevel` (0 dB) is the level of the sends that
  `track.add` makes, up to the project's maximum volume, and the level Ctrl-click gives a send's knob in the mixer.
- **Mute Pre-Send when Mute** (p. 1623). With the user setting `audio.mutePreSends` (off by default), muting a
  channel also mutes its pre-fader sends, with its mute button or its mute automation.
- **Feedback prevention.** Neither an output nor a send may lead back to its channel through other outputs and
  sends, on or off (`routingLoops` in `document.h`): `channel.set` refuses them with "would make a loop", so the
  processing graph never has a cycle, however groups nest and effect channels send on to others. Projects read from
  a file drop sends whose destination isn't a group or effect channel or would close a loop.
- **Removing** a destination's track, or turning a folder's group channel off, clears the destination of the sends
  to it, which keep their other settings. Duplicating a folder sends the copies of the tracks inside to the copies of
  the group channels they sent to, as their outputs do.
- **Solo** treats a send like an output: a soloed channel keeps the channels it sends to heard, and a soloed effect
  channel the channels that send to it ([solo-mute-listen.md](solo-mute-listen.md)).
- **DAWproject** has sends: each send with a destination is a `Send` in its channel's `Sends`, with `destination`,
  `type` `pre` or `post`, a `Volume` as a linear gain and `Enable`. A project read from `project.xml` alone gets its
  first eight sends as sends 1 to 8, in order; GNUbase's extra entry keeps their numbers.

## In the audio graph

- `graph::describe` adds a node per send with a destination (`NodeKind::Send`), after the channels and their slots,
  by channel and send number, with the channel's id as its object. A post-fader send receives its channel's output,
  in the channel's output format (stereo for a panned mono channel), so it follows the fader, the panner and the mute
  button. A pre-fader send has the channel's own format and only an event connection from the channel, which orders
  it after it: the channel's strip copies its audio into a `PreFaderTap` (`src/engine/audio/automation.h`) after the
  strip's sections, before the fader, and the send node reads it there. Every send node sends to its destination's
  first node, where it adds up in node order with the other inputs, so the mix stays bit-identical for any number of
  threads.
- **Gains.** `SendStrip` scales the audio by the send's gain from the render snapshot (0 while it is off), gliding
  from the gain of the last block over one block, so switching a send on or changing its level doesn't click; the
  channel's state keeps the gain across graph rebuilds. A pre-fader send doesn't follow the mute button, as with the
  reference's default (Mute Pre-Send when Mute off), unless `audio.mutePreSends` is on, but it is silent while its
  channel isn't heard because another is soloed or its track is disabled. With the setting, the mute button sets the
  send's gain to 0 when the graph is built, and the setting changing rebuilds it; mute automation, which plays on the
  audio thread, mutes the tap itself: the fader scales it by its mute, frame by frame, so the pre-fader sends glide
  out and in with the channel from the automation's sample on.
- **Wider sends.** A send in a wider format than its destination is converted while the destination sums its inputs,
  like an output ([channel-formats.md](channel-formats.md)): a stereo channel's send into a mono effect channel merges
  L and R at -3.01 dB each.
- **Real time.** The tap is made and sized when the graph compiles; the audio thread only copies and scales samples.

## In the mixer

The mixer's strips have a Sends section (`.mixer-sends`, docking set `channel`, hidden by default and shown with Set
up Window Layout), with a row per send: its on switch, its destination (a drop-down list of No Destination and the
other group and effect channels), Pre for a pre-fader send and a knob for its level, whose drag is one undo step
(#716). They send `channel.set` and show the engine's values after every undo step, as the strip's other sections do
([channel-strip.md](channel-strip.md)). Ctrl-click on a send's knob sets the default send level. The knob goes from
-60 dB, or the default send level if that is lower, to the project's maximum volume, as the fader does, so Ctrl-click
gives a send the level `track.add` gives it. The destination list
ends with Add Effect Track to Send (p. 554), which opens the add track dialog's effect page for that send, as in the
channel settings window ([channel-settings.md](channel-settings.md)). Output lists in the inspector and the add track
dialog offer effect channels too.

The track list's context menu has Group Track to Selected Tracks and Effect Track to Selected Tracks
(`track.addGroupToSelected`, `track.addEffectToSelected`): they open the add track dialog on the group or effect page,
and its Add Track passes the selected tracks as `tracks`. The group and effect pages have Add to Project 'Group' or
'FX' Folder (`projectFolder`).

## Not yet

The audio connections window (pp. 35–53), whose commands are the `bus.*` ones ([busses.md](busses.md)); direct
routing, an output with several destinations of which one or a sum plays (pp. 492–494); send panners and Link
Panners (p. 556); automation of the sends; the sends in the inspector, and Add Effect Track to Send in a send slot's
context menu, where the reference has it.

## Checks

- `tests/routing_tests.cpp`: `channel.set` sets, clamps, refuses and undoes sends; outputs and sends through nested
  groups and effect channels never close a loop; removing a destination and duplicating a folder; the send nodes of
  `graph.get`; rendered through the dummy device, post-fader sends follow the fader, pre-fader ones don't and play
  while the channel is muted, sends glide over a block when they come on or change level, and solo keeps the effect
  channels a soloed track sends to; outputs into effect channels; Mute Pre-Send when Mute with the mute button, with
  mute automation at two block sizes, and switched while the channel is muted; a stereo send into a mono effect
  channel; `track.add`'s `tracks` and `send` for group and effect tracks, the tracks it leaves out, its undo, and
  `projectFolder`.
- `tests/dawproject_tests.cpp` saves and loads sends, and reads them and outputs into effect channels from
  `project.xml` alone.
- `tests/mixerwindow_tests.cpp`: the Sends section picks a destination, switches a send on and before the fader,
  turns its level with the wheel, gives it the default send level with Ctrl-click, above +6 dB and below -60 dB too,
  and shows another client's change.
- `tests/addtrack_tests.cpp`: Group and Effect Track to Selected Tracks through the dialog, and Group Track to
  Selected Tracks with a Count of 2 and Keep Dialog Open.
