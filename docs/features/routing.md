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
  reference's default (Mute Pre-Send when Mute off), but it is silent while its channel isn't heard because another
  is soloed or its track is disabled.
- **Real time.** The tap is made and sized when the graph compiles; the audio thread only copies and scales samples.

## In the mixer

The mixer's strips have a Sends section (`.mixer-sends`, docking set `channel`, hidden by default and shown with Set
up Window Layout), with a row per send: its on switch, its destination (a drop-down list of No Destination and the
other group and effect channels), Pre for a pre-fader send and a knob for its level, whose drag is one undo step
(#716). They send `channel.set` and show
the engine's values after every undo step, as the strip's other sections do ([channel-strip.md](channel-strip.md)).

## Not yet

The audio connections window (pp. 35–53), whose commands are the `bus.*` ones ([busses.md](busses.md)); direct
routing, an output with several destinations of which one or a sum plays; send panners and Link Panners (p. 556);
the default send level and Ctrl-click to it; Mute Pre-Send when Mute (p. 1622); automation of the sends; Add Effect
Track to Send, Effect Track to Selected Tracks, Group Track to Selected Tracks and the project's Group and FX folders
(pp. 546, 553–554); sends in the inspector and the channel settings window (#112); routing outputs to effect
channels; and sends wider than their destination's panner, which convert like outputs do
([channel-formats.md](channel-formats.md)).

## Checks

- `tests/routing_tests.cpp`: `channel.set` sets, clamps, refuses and undoes sends; outputs and sends through nested
  groups and effect channels never close a loop; removing a destination and duplicating a folder; the send nodes of
  `graph.get`; rendered through the dummy device, post-fader sends follow the fader, pre-fader ones don't and play
  while the channel is muted, sends glide over a block when they come on or change level, and solo keeps the effect
  channels a soloed track sends to.
- `tests/dawproject_tests.cpp` saves and loads sends, and reads them from `project.xml` alone.
- `tests/mixerwindow_tests.cpp`: the Sends section picks a destination, switches a send on and before the fader,
  turns its level with the wheel and shows another client's change.
