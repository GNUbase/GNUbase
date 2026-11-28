# Solo, mute and listen

Silencing channels and listening to some of them without touching the mix (#109, reference manual pp. 463–469,
447, 528). Mute and solo themselves came with the track list ([track-list.md](track-list.md)); this adds the solo
modes, solo defeat, the Deactivate All buttons and listen.

## In the engine

- **Channel switches** (`channel.set`, every channel type unless noted): `mute`, `solo`, `soloDefeat` and `listen`
  (not MIDI channels, which have no audio). They are document state like the fader: undo steps, saved in GNUbase's
  extra entry (DAWproject has only `Mute` and `solo`).
- **Solo propagation** (`markSilent` in `src/engine/document/rendersnapshot.cpp`). While any channel is soloed, a
  channel is heard if it is soloed, plays into a soloed channel, or a soloed channel plays into it, through any number
  of groups: a soloed track keeps its group, the group that one plays into and so on; a soloed group keeps every
  channel that plays into it, nested groups and effect channels routed to it included. A soloed effect channel keeps
  the group it plays through, not that group's other sources. Channels with **solo defeat** stay heard while others
  are soloed, with the channels they play through; alone, solo defeat changes nothing. A muted channel stays silent
  either way. The others are `silent` in the render snapshot and glide out over one block. Sends are links of the
  same graph: a soloed track keeps the effect channels it sends to ([routing.md](routing.md)).
- **Exclusive solo** (p. 466): `channel.set` with `solo: true` and `exclusive: true` turns every other channel's
  solo off in the same undo step, announcing each with `channel.changed`; `exclusive` without `solo: true` changes
  nothing.
- **Deactivate All Mute, Solo or Listen States** (p. 447): `channel.deactivateAll` with `state` `mute`, `solo` or
  `listen` turns it off on every channel, solo together with solo defeat, as one undo step, and returns the
  channels that changed.
- **Listen** (pp. 466, 528). A listening channel plays on as before, and also, after its fader and panner (AFL), on
  the monitoring bus. Without a control room (#191) the monitoring bus is the main mix ([busses.md](busses.md)), so:
  while any channel listens, `graph::describe` adds a `listen` node per listening channel, which receives the
  channel's node, and a `listenBus` node in the main mix's format that receives the listen nodes, and the audio thread
  (`AudioEngine::output`) turns the main mix's device ports down to the user setting `audio.listenDim` (dB, default
  -144: only the listening channels are heard) and adds the listen bus there at `audio.listenLevel` (dB, default 0).
  The dim glides over one block as listening starts and ends, and so does each channel on the listen bus: its listen
  node (`ListenStrip`) glides it in when it starts to listen and out when it stops. So with the default dim the first
  channel that starts to listen keeps its level, while a later one glides in from the dim the mix is already at (silence
  by default). A channel that stopped listening keeps its listen node until it has glided out: the audio thread keeps
  its gain in the channel's state, and the next graph keeps the channel there while that is above 0. A new dim or listen
  level glides over a block too. Other output busses are left alone. Offline rendering (`audio.render`) plays what the
  device would, and the main mix's meter and loudness ([metering.md](metering.md)) measure it too: what is heard, with
  the dim and the listen bus. The audio mixdown ([mixdown.md](mixdown.md)) exports the main mix without them.

## In the mixer

- Each strip's fader section has M, S and, for audio channels, L (`button[control=listen]`). A click switches the
  state; on S, Ctrl-click (Cmd on macOS) solos exclusively, and Alt-click and click and hold (half a second,
  `ui::SoloHold`) switch solo defeat (`ui::soloClick`).
  S has `.checked` while soloed and `.defeated` with solo defeat; the themes draw solo defeat in the solo color on an
  unlit button, and a listening L like monitoring.
- The toolbar's M, S and L buttons (`.mixer-toolbar button[deactivate=<state>]`) are Deactivate All Mute, Solo and
  Listen States: `.checked` while any channel has the state (solo or solo defeat), a click sends
  `channel.deactivateAll`.
- The track list's S, and the inspector's S and L (not for MIDI channels), take the same clicks and show solo
  defeat and listen too.
- The project window's toolbar has the same Deactivate All buttons (p. 58) in its State Buttons section
  (`#toolbar [section=states] button[deactivate=<state>]`), which `ui::EventDisplay` shows and sends.

## Not yet

The control room (#191), and with it PFL, listen on input and output channels, and the listen level and dim in its panel
rather than in user settings; the toolbar's Activate/Deactivate Read for All Tracks (p. 58); the preference Group
Channels: Mute Sources as well (p. 1622); solo on directly monitored inputs with dimming.

## Checks

- `tests/document_tests.cpp`: which channels solo silences through nested groups and an effect channel routed to a
  group, solo defeat with and without solo, exclusive solo, `channel.deactivateAll` for each state and its undo;
  `exclusive` without solo; listening channels feed one `listenBus` node in `graph.get` through their `listen`
  nodes, which receive their channel nodes, also behind plugin slots; MIDI channels refuse listen.
- `tests/mixer_tests.cpp` renders a listening track at -20 dB beside another one, sample by sample: the mix glides
  out to the default dim while the track glides in, a new dim and listen level glide in, the mix glides back and the
  track out once listening ends, and the track glides in again and a second listening track beside it.
- `tests/mixdown_tests.cpp`: a mixdown while a channel listens is the main mix, bit for bit.
- `tests/dawproject_tests.cpp` saves and loads solo defeat and listen.
- `tests/mixerwindow_tests.cpp`: S, Ctrl-click, Alt-click and click and hold on S, L, M and the toolbar's Deactivate
  All buttons; `tests/tracklist_tests.cpp` the track list's Ctrl-click, Alt-click and click and hold on S;
  `tests/inspector_tests.cpp` the inspector's S with its modifiers and L, and the project window toolbar's state
  buttons.
