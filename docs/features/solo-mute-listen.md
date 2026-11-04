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
- **Exclusive solo** (p. 466): `channel.set` with `solo` and `exclusive: true` turns every other channel's solo
  off in the same undo step, announcing each with `channel.changed`.
- **Deactivate All Mute, Solo or Listen States** (p. 447): `channel.deactivateAll` with `state` `mute`, `solo` or
  `listen` turns it off on every channel, solo together with solo defeat, as one undo step, and returns the
  channels that changed.
- **Listen** (pp. 466, 528). A listening channel plays on as before, and also, after its fader and panner (AFL), on
  the monitoring bus. Without a control room (#191) the monitoring bus is the main mix ([busses.md](busses.md)), so:
  while any channel listens, `graph::describe` adds a `listenBus` node in the main mix's format that receives every
  listening channel's node, and the audio thread (`AudioEngine::output`) turns the main mix's device ports down to the
  user setting `audio.listenDim` (dB, default -144: only the listening channels are heard) and adds the listen bus
  there at `audio.listenLevel` (dB, default 0). The dim glides over one block as listening starts and ends. Other
  output busses are left alone. Offline rendering (`audio.render`) plays what the device would.

## In the mixer

- Each strip's fader section has M, S and, for audio channels, L (`button[control=listen]`). A click switches the
  state; on S, Ctrl-click (Cmd on macOS) solos exclusively and Alt-click switches solo defeat (`ui::soloClick`).
  S has `.checked` while soloed and `.defeated` with solo defeat; the themes draw solo defeat in the solo color on an
  unlit button, and a listening L like monitoring.
- The toolbar's M, S and L buttons (`.mixer-toolbar button[deactivate=<state>]`) are Deactivate All Mute, Solo and
  Listen States: `.checked` while any channel has the state (solo or solo defeat), a click sends
  `channel.deactivateAll`.
- The track list's S takes the same Ctrl- and Alt-clicks and shows solo defeat too.

## Not yet

The control room (#191), and with it PFL, listen on input and output channels, and the listen level and dim in its
panel rather than in user settings; listened channels gliding in and out (they come in at once; the dim glides);
the project window toolbar's state buttons (p. 58); click and hold for solo defeat; the inspector's solo modifiers
and its Listen indicator; linking mute, solo and listen in link groups (p. 458); the preferences Group Channels:
Mute Sources as well and Mute Pre-Send when Mute (p. 1622); solo on directly monitored inputs
with dimming.

## Checks

- `tests/document_tests.cpp`: which channels solo silences through nested groups and an effect channel routed to a
  group, solo defeat with and without solo, exclusive solo, `channel.deactivateAll` for each state and its undo;
  listening channels feed one `listenBus` node in `graph.get`, MIDI channels refuse listen.
- `tests/mixer_tests.cpp` renders a listening track at -20 dB beside another one: the mix glides out to the default
  dim, a new dim and listen level glide in, and the mix glides back once listening ends, sample by sample.
- `tests/dawproject_tests.cpp` saves and loads solo defeat and listen.
- `tests/mixerwindow_tests.cpp`: S, Ctrl-click and Alt-click on S, L, M and the toolbar's Deactivate All buttons.
