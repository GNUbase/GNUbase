# Link groups and VCA faders

Linked channels and the VCA faders that control them (#110, reference manual pp. 153–154, 457–461, 515–519). The
fader itself is in [mixer.md](mixer.md), mute, solo and listen in [solo-mute-listen.md](solo-mute-listen.md).

## In the engine

- **VCA tracks** (`track.add` `type` `vca`, p. 153) have a channel without audio, input or output (`format`
  `none`, not in the render snapshot or the processing graph): a fader (`volume`), mute, solo, solo defeat, listen,
  monitor and record enable, and an automation lane for its volume (`parameter.list` lists only that). New ones are
  called VCA 01, VCA 02 and so on.
- **Link groups** (`Project::linkGroups`, `LinkGroup` in `src/engine/document/document.h`, in `project.get` as
  `linkGroups`): a name, the linked `channels`, what they link and the `vca` that controls them, 0 for none. A channel
  is in one group at most and a VCA fader controls one group at most: a group that takes a channel or a VCA fader
  takes it from the group it was in. `linkGroup.add` (unless named, Link Group N with N one more than the number
  of groups, or the next number no group's name has; at least one channel), `linkGroup.set` and `linkGroup.remove`
  (unlink; a VCA fader that controlled the group stays) are undo steps; removing a track takes its channel out of
  its group, and a VCA track that goes leaves its group without one, which links the volume again (p. 518). A group
  whose last channel goes, to another group or with its track, goes too (`linkGroup.removed`), as linking needs a
  channel; its VCA fader stays and controls nothing.
- **What a group links** (p. 458): `volume` (on by default) moves the other channels' faders by as much as one moved,
  stopping at their ends; `pan` likewise (both commands below through `followLinkedMove` in
  `src/engine/document/document.cpp`, so the rules can't drift apart); `mute` passes mute, solo, solo defeat and listen
  on as they are, `record` record enable and monitor; `eq`, `dynamics` (the strip) and `sends` (on by default) the
  changes of those sections ([channel-settings.md](channel-settings.md)). `channel.set` does it in the same undo step,
  announcing each channel; with `linked` false it changes only the channel, as the mixer's Sus does (p. 460).
  `parameter.set` passes the same parameters on in the same way ([parameters.md](parameters.md)): the fader's volume,
  pan and mute, and the equalizer's and the dynamics' section parameters, but not the pre section's, which `channel.set`
  doesn't pass on either ([channel-strip.md](channel-strip.md)), nor an instrument's, a plugin's or a MIDI effect's.
  Exclusive solo leaves the channels the solo went to soloed.
- **VCA faders** (pp. 515–519). A group with a VCA fader doesn't link the volume: giving it one turns `volume` off,
  taking it away turns it back on (p. 517), and `volume` with a VCA fader is refused. The VCA fader's buttons go on
  to the group's channels, those that have them (a MIDI channel has no listen). Its volume adds to the channels' in
  dB without changing their faders: a channel at -6 dB under a VCA fader at +3 dB plays at -3 dB (p. 515).
  **Nested VCA faders** (p. 518): a VCA fader can be in a group that another one controls, whose volume and buttons
  then reach the channels of both; a VCA fader in the group it controls, or one that would control itself through
  others, is refused.
- **Automation** (p. 519): the render snapshot (`applyVcaFaders` in `src/engine/document/rendersnapshot.cpp`)
  multiplies each channel's volume, static or automated, by the gains of the VCA faders that control it, static or
  automated, so their curves combine and play through the channel's fader as its own do, sample-accurately. Where
  only one of the curves moves, the product is that curve's segment scaled; where several move at once, straight
  pieces between their products every 256 frames (`g_productInterval`). The channels' own automation stays as it is.
  Gaps in either curve (virgin territory and terminators, docs/features/virgin-territory.md) play that fader's
  static volume.
- **DAWproject**: a VCA track is a track with `contentType` `automation` and a channel with `role` `vca` and its
  `Volume`; link groups, which DAWproject doesn't have, go into GNUbase's extra entry. A file's link groups lose
  channels that are gone or already in a group, and VCA faders that aren't VCA tracks' channels, control another
  group or would control themselves (in the group they control, or through others: the first group of such a loop
  loses its VCA fader); groups left without channels go. Another application's VCA channel keeps its fader, mute and
  solo; its pan, sends and destination are left out, as a VCA channel can't have them.

## In the UI

- The mixer's strips (`ui::Mixer`) have the display line (`.mixer-link`, p. 458) under the name: the number and
  name of the channel's link group, empty for none. A VCA strip (`type` `vca`) has a fader, M, S and L, and instead
  of the panner a drop-down list `.mixer-vca-group` (VCA Channel Controls Link Group, p. 515) with None and the link
  groups, which sends `linkGroup.set`. The channel type filter has VCA Channels.
- The add track dialog has a VCA page with the name and the count (p. 154). The inspector shows a VCA track's fader,
  mute, solo, record and monitor, without routing, pan or inserts.

## Not yet

The Link Group Settings dialog and the mixer toolbar's Link, Edit Link Group Settings, Q-Link, Sus and Abs buttons,
which need channels selected in the mixer; linking inserts, automation read and write, routing and selection
(p. 458); absolute instead of relative changes; Add VCA Track to Selected Tracks and the project's VCA
folder (pp. 154, 517); the VCA fader's meter of its channels' levels (p. 516); Combine Automation of VCA and Linked
Channels, and the channels' automation tracks showing the combined curve (pp. 516, 519); VCA mute automation; and
the display line's pop-up menu (p. 459).

## Checks

- `tests/linkgroups_tests.cpp`: linked channels pass the fader and the pan on relatively and the buttons as they are,
  `linked` false, exclusive solo, one undo step; `parameter.set` passing on what the group links and not what it
  doesn't; a channel moving between groups, removing tracks and unlinking; refusals. VCA tracks' channels and their
  parameter; a VCA fader turns off the volume link, passes its buttons on through a nested VCA fader in one undo step,
  refuses loops, leaves the group it controlled for another one; removing a VCA track turns the volume link back on;
  groups without channels go; default names stay unique; saving and loading. Rendered through the dummy device: the
  reference manual's nested example (-3, -10 and +4 dB play at -9 dB), a VCA ramp over a static channel fader exactly,
  and a VCA ramp over the channel's own ramp within 0.001.
- `tests/mixerwindow_tests.cpp`: a VCA strip's fader and buttons, its group drop-down list and the display line.
- `tests/dawproject_tests.cpp`: VCA fader loops in a file are left out, and another application's VCA channel loses its
  pan, sends and destination.
