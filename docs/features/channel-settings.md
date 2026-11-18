# Channel settings window

One channel in a window of its own, with all its sections (#112, reference manual pp. 499-512), copying and pasting
channel settings (p. 461), and link groups that link the equalizers, the dynamics and the sends (p. 458). The sections
themselves are in [channel-strip.md](channel-strip.md) and [routing.md](routing.md), the mixer in
[mixer-window.md](mixer-window.md), link groups in [link-groups.md](link-groups.md).

## In the engine

- **`channel.copySettings`** `{channel}` copies a channel's settings for `channel.pasteSettings`: the pre section, the
  equalizer, the strip, the sends and the inserts (the plugin slots, with their plugins' parameters and state). Not
  the fader, the pan, the buttons, the routing or the notes. It has a clipboard of its own, outside the project and
  its undo history (`src/engine/document/channelsettings.cpp`), like `automation.copy`'s; MIDI and VCA channels have
  no settings to copy.
- **`channel.pasteSettings`** `{channels}` gives each channel what was copied, in one undo step (Paste Channel
  Settings): the copy as it was when it was copied, whatever the source does since. A channel's inserts are replaced by
  copies of the copied ones, and the automation of the plugins it had goes with them, as with `plugin.remove`. A send
  keeps its destination only where the channel may send, so not to itself and never in a loop (`routingLoops`);
  otherwise it keeps its other settings without a destination. It refuses MIDI and VCA channels, and pastes nothing
  before something was copied.
- **Linking** (p. 458). A link group's `eq`, `dynamics` and `sends` (`linkGroup.add`, `linkGroup.set`) pass
  `channel.set`'s `eq`, `strip` and `sends` on to the group's other channels as they were given, in the same undo
  step: only what changed, so a band or a send that wasn't touched stays each channel's own. A channel that can't take
  a change keeps its own: a MIDI channel, or a send that would go to itself or close a loop. As in the reference,
  `sends` is on by default, also with a VCA fader; `eq` and `dynamics` are off. `linked` false changes only the
  channel. Files from before get `sends` on.

## In the app

The channel settings window (`resources/ui/channel-settings.rml`) is a docking window, `channelSettings`, whose main
zone has a `ui::Mixer` of the kind `MixerKind::ChannelSettings`: the mixer's strip for one track's channel, with its
sections side by side and the pre section, the equalizer, the strip, the sends and the fader shown by default (the
pictures and the notepad too with Set up Window Layout, which keeps the window's own layout in `ui.layout`).

```
.mixer.channel-settings
  .toolbar.mixer-toolbar
    #settings-previous, #settings-next   Go to Previous and Next Mixer Channel: the tracks' channels in the mixer's order; disabled at the ends
    .settings-name                       the channel's name
    #settings-output                     Go to Output: the name of the bus or channel it plays into; goes to a group or effect channel
    #settings-functions                  the Functions menu #settings-functions-menu
    #mixer-layout                        Set up Window Layout
  .mixer-channels
    .mixer-channel[sections=channel][channel=<id>][type=<type>]   the strip, as in the mixer
#settings-functions-menu
  menuitem[function=copy]     Copy Channel Settings: channel.copySettings
  menuitem[function=paste]    Paste Channel Settings: channel.pasteSettings onto the channel shown
  menuitem[function=follow]   Follow Edit Buttons or Selection Changes, checked by default
```

- **Opening it** (p. 500): Edit Channel Settings, the `e` button (`button.mixer-edit`) on every track's strip in the
  mixers and beside the track's name in the inspector (`button.inspector-edit`), and the UI command
  `window.channelSettings` for the first selected track. Each opens the window, or shows the channel in it while it is
  open, and selects the track in the track list (`AppUi::openChannelSettings`).
- **Following the selection** (pp. 499-500). While Follow is on, the window shows the first selected track's channel,
  and going to another channel in the window (previous, next, Go to Output) selects its track in the track list. With
  Follow off, the window and the track list's selection go their own ways.
- **Add Effect Track to Send** (p. 554). The destination list of every send, in the mixers and in the window, ends with
  Add Effect Track to Send: it opens the add track dialog on the effect page in that window, with the track and the
  send (the document's `tracks`, `send` and `below` attributes), and the dialog's Add Track passes them to
  `track.add`: the new effect track goes below the track whose send it is. The list
  shows the send's destination again when the project comes back.

## Not yet

The channel settings window's left and right zones and their layout as in the reference; the inserts section with
the strip's pre- or post-insert position (#113), the cue sends and direct routing sections and the output chain; Go
to Last and Next Edited Channel, Search Channels, Go to Input, Edit Instrument, track presets and the window's other
functions; the equalizer's curve display, its settings panel and channel comparison (pp. 509-511); the details views
of the strip's modules (pp. 505-508); several channel settings windows at once; the track list's own Edit Channel
Settings button; the channels of input and output busses; copying the first selected channel's settings from the
mixer's Functions menu, which needs channel selection in the mixer; linking the inserts, the automation read and
write states, the routing and the selection (p. 458).

## Checks

- `tests/channelsettings_tests.cpp`: `channel.copySettings` and `channel.pasteSettings` copy the sections, the sends
  and copies of the inserts, not the fader, in one undo step, paste what was copied rather than what the source is
  now, drop a send's destination that would go to the channel itself, and refuse MIDI channels, unknown channels and
  pasting before a copy; link groups pass the equalizer's, the dynamics' and the sends' changes on, not by default but
  for the sends, skip a send to the channel itself, take one undo step, and respect `linked` false. In the app: the
  mixer's `e` opens the window for its channel and selects the track; the sections it shows; a control sending
  `channel.set`; previous, next and Go to Output; following the track selection and not following it; the UI command;
  a removed track; Copy and Paste Channel Settings; Add Effect Track to Send through the add track dialog, below the
  track whose send it is; and the
  inspector's `e`.
- `tests/linkgroups_tests.cpp`: the link group's JSON with `eq`, `dynamics` and `sends`.
