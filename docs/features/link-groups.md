# Link groups and VCA faders

Linked channels and the VCA faders that control them (#110, reference manual pp. 153–154, 457–461, 515–519). The
fader itself is in [mixer.md](mixer.md), mute, solo and listen in [solo-mute-listen.md](solo-mute-listen.md).

## In the engine

- **VCA tracks** (`track.add` `type` `vca`, p. 153) have a channel without audio, input or output (`format`
  `none`, not in the render snapshot or the processing graph): a fader (`volume`), mute, solo, solo defeat, listen,
  monitor and record enable, and an automation lane for its volume (`parameter.list` lists only that). New ones are
  called VCA 01, VCA 02 and so on. With `tracks` (Add VCA Track to Selected Tracks, p. 517), the tracks' channels go
  into a new link group that the new VCA fader controls, in the same undo step; tracks without a channel, such as
  folders without a group channel, are left out. The reference turns that group's Selection off, which link groups
  don't link yet. `projectFolder` puts it into the project's VCA folder (Add to Project 'VCA' Folder, p. 154), as it
  does group and effect tracks into theirs ([routing.md](routing.md)).
- **Link groups** (`Project::linkGroups`, `LinkGroup` in `src/engine/document/document.h`, in `project.get` as
  `linkGroups`): a name, the linked `channels`, what they link and the `vca` that controls them, 0 for none. A channel
  is in one group at most and a VCA fader controls one group at most: a group that takes a channel or a VCA fader
  takes it from the group it was in. `linkGroup.add` (unless named, Link Group N with N one more than the number
  of groups, or the next number no group's name has; at least one channel), `linkGroup.set` and `linkGroup.remove`
  (unlink; a VCA fader that controlled the group stays) are undo steps; removing a track takes its channel out of
  its group, as do turning a folder's group channel off (`track.set` `groupChannel` false) and Remove Empty Tracks
  (`track.removeEmpty`), and a VCA track that goes leaves its group without one, which links the volume again
  (p. 518). A group whose last channel goes, to another group or with its track, goes too (`linkGroup.removed`), as
  linking needs a channel; its VCA fader stays and controls nothing.
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
- **Meter** (p. 516): a VCA fader's meter shows the summed level of the audio channels it controls, also through
  nested VCA faders. The render snapshot lists them (`RenderSnapshot::vcas`), and the graph gets a node per VCA fader
  with channels (`graph::NodeKind::VcaMeter`, in the main mix's format) that sums their outputs after their faders and
  goes nowhere: its processor measures the sum in the VCA channel's `ChannelState`, as a channel strip measures its
  output ([metering.md](metering.md)). `meter.get` and the mixer read it as any channel's meter.
- **DAWproject**: a VCA track is a track with `contentType` `automation` and a channel with `role` `vca` and its
  `Volume`; link groups, which DAWproject doesn't have, go into GNUbase's extra entry. A file's link groups lose
  channels that are gone or already in a group, and VCA faders that aren't VCA tracks' channels, control another
  group or would control themselves (in the group they control, or through others: the first group of such a loop
  loses its VCA fader); a group that loses its VCA fader links the volume again, as when its VCA track goes, and
  groups left without channels go. Another application's VCA channel keeps its fader, mute and solo; its pan, sends
  and destination are left out, as a VCA channel can't have them, and so are the audio channels, input, output, pan,
  sends and slots of a VCA channel in a hand-edited GNUbase entry.

## In the UI

- The mixer's strips (`ui::Mixer`) have the display line (`textdisplay.mixer-link`, p. 458) under the name while the
  project has a link group (`.mixer-channels.link-groups`): the number and name of the channel's link group, empty for
  none. A click opens its menu (`#mixer-link-menu`, p. 459), filled for the channel as it opens: Unlink Selected
  Channels (`linkGroup.remove` of the line's group, as the mixer has no channel selection yet), Edit Link Group
  Settings (below), Included in Link Group with None and the groups, which moves the channel into the group chosen with
  `linkGroup.set` `channels` or takes it out of its group (pp. 460-461; a group goes with its last channel), and Linked
  Channels, the names of the group's channels. A double click types the group's name, which `linkGroup.set` `name`
  takes; the number the field starts with is left out. A double click with a modifier key (Ctrl, Cmd, Alt or Shift)
  opens the Link Group Settings dialog instead.
- The **Link Group Settings dialog** (`resources/ui/link-group-settings.rml`, pp. 457-458) for the line's group shows
  its name, Use VCA Fader with Select VCA Fader (the VCA faders not in the group) and what it links: Volume, Pan, EQ,
  Dynamics, Sends, Mute/Solo/Listen and Record Enable/Monitor. Use VCA Fader turns Volume off and disables it, as a
  group with a VCA fader doesn't link the volume, and turning it off again turns Volume back on (p. 517). OK sends one
  `linkGroup.set` with what the dialog shows, Cancel and Escape nothing. Inserts, Automation Read/Write, Routing and
  Selection aren't there, as link groups don't link them yet.
- A VCA strip (`type` `vca`) has a fader with its meter, M, S and L, and instead of the panner a drop-down
  list `.mixer-vca-group` (VCA Channel Controls Link Group, p. 515) with None and the link groups, which sends
  `linkGroup.set`. The channel type filter has VCA Channels.
- The add track dialog has a VCA page with Add to Project 'VCA' Folder, the name and the count (p. 154). The track
  list's context menu has VCA Track to Selected Tracks (`track.addVcaToSelected`), which opens it for the selected
  tracks ([add-track.md](add-track.md)). The inspector shows a VCA track's fader,
  mute, solo, record and monitor, without routing, pan or inserts.

## Not yet

The Link Group Settings dialog for new groups and the mixer toolbar's Link, Edit Link Group Settings, Q-Link, Sus
and Abs buttons, which need channels selected in the mixer; linking inserts, automation read and write, routing and
selection (p. 458); the Abs button's absolute instead of relative changes (p. 460): without it the equalizers', the dynamics' and the send
levels' changes should go on by as much as they moved, as the fader and the pan do, but they go on as the values given;
Add VCA Track in the mixer's context menu, and Add VCA Track to Selected Tracks for channels selected there
(pp. 516-517); Combine Automation of VCA and Linked
Channels, and the channels' automation tracks showing the combined curve (pp. 516, 519); VCA mute automation.

## Checks

- `tests/linkgroups_tests.cpp`: linked channels pass the fader and the pan on relatively and the buttons as they are,
  `linked` false, exclusive solo, one undo step; `parameter.set` passing on what the group links and not what it
  doesn't; a channel moving between groups, removing tracks and unlinking; refusals. VCA tracks' channels and their
  parameter; a VCA fader turns off the volume link, passes its buttons on through a nested VCA fader in one undo step,
  refuses loops, leaves the group it controlled for another one; removing a VCA track turns the volume link back on;
  groups without channels go, also when a folder's group channel goes or `track.removeEmpty` removes their tracks;
  default names stay unique; a VCA track for tracks controlling a new group of their
  channels, in the project's VCA folder, in one undo step; saving and loading. Rendered through the dummy device: the
  reference manual's nested example (-3, -10 and +4 dB play at -9 dB), a VCA ramp over a static channel fader exactly,
  and a VCA ramp over the channel's own ramp within 0.001; a VCA fader's meter 6 dB above its two equal channels', also
  through a nested VCA fader, and none for a VCA fader without channels.
- `tests/mixerwindow_tests.cpp`: a VCA strip's fader, meter and buttons, its group drop-down list and the display line;
  the display line's menu moving a channel into another group, out of its group and into one again, its own group
  checked and picking it again sending nothing, Linked Channels and Unlink Selected Channels, renaming a group by a
  double click, and no display line without a group; the Link Group Settings dialog from the menu and from a double
  click with Ctrl, showing the group's settings, Use VCA Fader turning Volume off, OK sending the settings and Cancel
  nothing.
- `tests/dawproject_tests.cpp`: VCA fader loops in a file are left out; groups whose VCA fader is an audio channel, a
  channel that isn't there or one that controls an earlier group lose it and link the volume again; another
  application's VCA channel loses its pan, sends and destination, and one in a hand-edited GNUbase entry its audio
  channels, routing, pan, sends and slots; a project with a VCA track's volume automation loads from GNUbase's
  entry, link groups and all.
