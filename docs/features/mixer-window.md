# Mixer window

The mixer windows and the mixer in the project window's lower zone (#106, reference manual pp. 434–462, 496–498):
the channels of the project side by side, with a toolbar, channel visibility and the channel type filter, the
channels' order, pictures and notes, and in the windows the left zone. The channel strip's sections, such as routing,
inserts, EQ, sends and the full fader with its panner and meters, come with #107; the faders' and panners' engine side
is in [mixer.md](mixer.md).

## In the engine

- **What a mixer hides** (`mixer.get`, `mixer.set`). Each mixer has a view of its own: `lower`, the lower zone's, and
  `window1` to `window4`, the windows'. A view has `hidden`, the channels and busses it doesn't show by id, and
  `hiddenTypes`, the channel types its filter hides: `input`, `audio`, `instrument`, `midi`, `group` (folders' group
  channels too), `effect` and `output` (`g_mixerChannelTypes`, `mixerChannelType` in
  `src/engine/document/mixerviews.h`). `mixer.set` changes what it is given and emits `mixer.changed` with the view;
  it refuses ids that are neither a channel nor a bus. `mixer.get` leaves out channels that have gone since. As in the
  reference, the lower zone's mixer and the windows don't follow each other's visibility (p. 442).
- Views are settings, like snap: not undo steps (undo takes back a track, not what a mixer hides), saved with the
  project in GNUbase's extra entry (`mixers` in `project.get`), empty for files from before.
- **Notes** (p. 497): `channel.set` `notes`, the text of a channel's notepad. **Pictures** (p. 497): `track.set`
  `picture`, an image file (PNG, JPEG or SVG) that the mixer shows on the track's channel, or empty. Both are undo
  steps, copied by `track.duplicate` and saved in GNUbase's extra entry.

## The mixer

`ui::Mixer` (`src/ui/mixer.h`) fills an element with the toolbar and the fader section. It reads the project with
`project.get` after every undo step or new project and the view with `mixer.get`, follows `mixer.changed`, and sends
engine commands, so whichever client changes the project or the view, the mixer shows it at once.

```
.mixer
  .toolbar.mixer-toolbar
    #mixer-filter            Set Channel Type Filter, menu #mixer-filter-menu; .checked while a type is hidden
    #mixer-agents            Set Channel Visibility Agents, menu #mixer-agents-menu
    #mixer-left-zone         Show/Hide Left Zone (windows only)
    #mixer-layout            Set up Window Layout, the setup pane #mixer-layout-pane
  .mixer-channels            the fader section, scrolled sideways
    .mixer-channel[sections=channel][channel=<id>][type=<type>]   a strip per channel it shows
      .mixer-channel-name
      .mixer-picture[section=picture]   Pictures, hidden by default
      .mixer-notes[section=notes]       Notepad, hidden by default: textarea.mixer-notes-field
      .mixer-fader[section=fader]       Fader: M and S (button[control]), the fader and .mixer-volume
```

- **Channels** (p. 438): the input busses first, then the channels of the tracks from top to bottom (audio,
  instrument, MIDI, group and effect tracks, and folders with a group channel), then the output busses, as in the
  reference, where input and output channels have fixed places. MIDI channels have mute and solo but no fader;
  busses have neither yet. The fader goes from -60 dB to the project's maximum volume, linear in dB until the channel
  strip's dB law (#107); its value and dB text show the channel's volume.
- **Following the project.** Strips are made again only when the channels shown or their order change; otherwise the
  names, pictures, notes, buttons and faders are set where they are, so a fader being dragged stays under the mouse.
  New strips go through `Docking::arrangeSections`, so they get the user's order and hidden sections.
- **Visibility and filter** (pp. 442, 454–456). The channel type filter's menu has a check item per type; the agents'
  menu has Show All Channels, which shows every hidden channel again. In a window, the left zone's Visibility tab
  (`Mixer::showVisibility`) has a toggle button per channel (`button.mixer-visibility-item[visibility=<id>]`), checked
  while the mixer shows it. Each sends `mixer.set` for its own mixer.
- **Set up Window Layout** (p. 439): the setup pane ([docking.md](docking.md)) shows and hides the left zone (in a
  window) and the strips' sections Pictures, Notepad and Fader, and reorders them; the layout is the user setting
  `ui.layout`, per window (`mixer1` to `mixer4`, and `project` for the lower zone's).
- **Order** (p. 496). A channel's name dragged onto another channel takes its place: its track moves into that
  track's folder with `track.move`, after it when it came from the left, before it otherwise, so the track list
  follows. Busses keep their places, and a folder doesn't move into itself.
- **Notes and pictures** (p. 497). The notepad shows the channel's notes; what is typed is sent with `channel.set`
  when the field loses the focus. The picture section shows the track's picture (an `<img>`, or an `<svg>` for SVG
  files); a double click opens the OS's file dialog for a picture, then `track.set`.

## Windows and the lower zone

- **Mixer windows** (pp. 437–438): `resources/ui/mixer.rml`, a docking window with the left zone and the main zone
  with the mixer. `window.mixer` (F3, Studio > Mixer) opens the first one in a window of its own, or closes it;
  `window.mixer2` to `window.mixer4` (Studio > Mixer 2 to 4) the others, each with its own view and layout. Like every
  window, each opens on any monitor and renders at its monitor's scale (architecture.md, "UI shell"). The left zone
  has the Visibility tab, a panel (`mixer1Visibility` to `mixer4Visibility`) of the docking system; Tab moves the
  focus between the zones (focus zones `left` and `mixer`). A window the user closes lets go of its mixer.
- **The lower zone's mixer** (pp. 434–436): the project window's Mixer panel, with the view `lower`.
  `window.lowerZoneMixer` (Alt+F3, Studio > Mixer in Project Window) shows it as the lower zone's tab, or hides the
  lower zone while it shows it. Its toolbar has no left zone button: the project window's left zone isn't the
  mixer's.

## Not yet

Channel selection and the agents and Move Selected Channels commands that work on it, dragging several channels at
once and the green drop line; the Zones tab (locking channels to the left and right), the History and Snapshots tabs and
the right zone; the channel overview, the meter bridge, Channel Latency and the channel strip's
sections (#107); Find Track/Channel, channel visibility configurations, Undo Visibility Change, syncing the visibility
with the project window's track visibility, linking channels and mixers, the zoom palette and the transport and state
buttons of the toolbar; the track pictures browser and pictures in the track list; faders of busses; notes in
DAWproject's `comment`; and the windows' places, which workspaces will keep (#163).

## Checks

- `tests/mixerviews_tests.cpp`: each mixer's hidden channels and types, sorted and in the filter's order, kept per
  mixer, refused for unknown ids, types and mixers, not undo steps, without channels that are gone, and saved and
  loaded; notes and pictures as undo steps, copied with a track, saved and loaded.
- `tests/mixerwindow_tests.cpp` drives the app with `ui::Automation`: F3 opens a mixer window with the busses and the
  channels in the track list's order and closes it again; tracks added, renamed, moved and removed by the engine show
  at once; the fader, the dB text and the mute and solo buttons both ways; two windows, the Visibility tab and the
  type filter hiding channels in one window only, Show All Channels, and `mixer.set` from the engine; Alt+F3 showing
  the lower zone's mixer with its own view and hiding the zone again; a closed window letting go of its mixer; the
  setup pane's Pictures, Notepad and Fader, notes from the engine and typed in the notepad, a picture shown and
  removed, and channel names dragged onto others reordering the tracks, while busses stay.
