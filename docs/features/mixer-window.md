# Mixer window

The mixer windows and the mixer in the project window's lower zone (#106, reference manual pp. 434–462, 496–498):
the channels of the project side by side, with a toolbar, channel visibility and the channel type filter, the
channels' order, pictures and notes, and in the windows the left zone. The channel strip's pre, equalizer and strip
sections and the panner are in [channel-strip.md](channel-strip.md), the sends in [routing.md](routing.md); the faders'
and panners' engine side is in [mixer.md](mixer.md).

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
- **Undo and Redo Visibility Change** (p. 457, `mixer.undoVisibility`, `mixer.redoVisibility`): each mixer keeps the
  views before its last 10 visibility changes, `mixer.set` calls that change `hidden` or `hiddenTypes`
  (`Project::mixerHistory`), and those it undid until the next change. A mixer that follows the track list (below)
  also counts each change the track list makes to its channels as one, so undo takes back exactly the last; channels
  the track list hides again right after a `mixer.set` (a folder's tracks) are part of that set's change. Not kept in
  the file, and a loaded project has none; with nothing to undo or redo, the view stays.
- **Notes** (p. 497): the channel's notes (`channel.set` `notes`), which the inspector's notepad edits too
  ([inspector.md](inspector.md)). **Pictures** (p. 497): `track.set` `picture`, an image file (PNG, JPEG or SVG) that
  the mixer shows on the track's channel, or empty; a relative path is taken from the working folder and kept
  absolute. Both are undo steps and copied by `track.duplicate`; notes are saved in DAWproject's `comment` too.
  Pictures go with the project file as freeze files do, so a project keeps them when it moves (the reference keeps
  them with the project as well, in its folder Track Pictures, p. 125): `project.save` embeds each picture under
  `pictures/` and a load extracts it into the project's media folder, where `media.cleanup` leaves it; with
  `embedMedia` false the file refers to a picture relative to itself when it is in the file's folder, otherwise by its
  absolute path, as it does to a picture that is missing. GNUbase's extra entry has them as `pictures`, by track id.

## The mixer

`ui::Mixer` (`src/ui/mixer.h`) fills an element with the toolbar and the fader section. It reads the project with
`project.get` after every undo step or new project and the view with `mixer.get`, follows `mixer.changed`, and sends
engine commands, so whichever client changes the project or the view, the mixer shows it at once. A step that changed
only channels, such as each step of a fader, panner or knob drag, it reads none for: it takes the channels that
`channel.changed` brings into the project it read and updates only their strips, the Deactivate All buttons and the
channel settings window's Go to Output, so that a drag follows the mouse however many tracks the project has (#1881). A
control's last step answered, its strip shows the engine's value; a refused one reads the project again.

```
.mixer
  .toolbar.mixer-toolbar
    #mixer-filter            Set Channel Type Filter, menu #mixer-filter-menu; .checked while a type is hidden
    #mixer-agents            Set Channel Visibility Agents, menu #mixer-agents-menu
    #mixer-find              Find Track/Channel, the selector #mixer-find-menu
    #mixer-left-zone         Show/Hide Left Zone (windows only)
    #mixer-layout            Set up Window Layout, the setup pane #mixer-layout-pane
  .mixer-channels            the fader section, scrolled sideways
    .mixer-channel[sections=channel][channel=<id>][type=<type>]   a strip per channel it shows
      .mixer-channel-name[color]        the track's color, .colored while it has one, .bright on a bright color
      .mixer-picture[section=picture]   Pictures, hidden by default
      .mixer-routing[section=routing]   Routing (mixers only): select[param=input] (audio tracks), select[param=output], hidden by default
      .mixer-pre, .mixer-eq, .mixer-strip   Pre, Equalizer and Strip (audio channels), hidden by default (channel-strip.md)
      .mixer-sends[section=sends]       Sends (audio channels), hidden by default (routing.md)
      .mixer-notes[section=notes]       Notepad, hidden by default: textarea.mixer-notes-field
      .mixer-fader[section=fader]       Fader: the panner (.mixer-pan), M, S and L (button[control], solo-mute-listen.md), the fader and .mixer-volume
        button.mixer-edit               Edit Channel Settings on a track's channel (channel-settings.md)
```

- **Channels** (p. 438): the input busses first, then the channels of the tracks from top to bottom (audio,
  instrument, MIDI, group and effect tracks, and folders with a group channel), then the output busses, as in the
  reference, where input and output channels have fixed places. MIDI channels have mute and solo but no fader;
  busses have neither yet. The fader goes from -60 dB to the project's maximum volume, linear in dB until the channel
  strip's dB law (#107); its value and dB text show the channel's volume.
- **Following the project.** Strips are made again only when the channels shown or their order change; otherwise the
  names, pictures, notes, buttons and faders are set where they are, so a fader being dragged stays under the mouse.
  A drag, wheel turns or a held arrow key on the fader, a knob or a value field is one undo step, sent with
  `EngineBinding::sendStep`; while its steps are on their way, the project read in between doesn't set the control,
  and during a drag the control keeps the mouse's value, so it never jumps back (#716,
  [ui-engine-binding.md](ui-engine-binding.md)). A control made again has no value of its own yet and shows the
  engine's right away, also while a step is on its way; a step the engine refuses reads the project again, so the
  control shows the engine's value. The dB text follows the fader as it moves.
  New strips go through `Docking::arrangeSections`, so they get the user's order and hidden sections.
- **Visibility and filter** (pp. 442, 454–456). The channel type filter's menu has a check item per type; the agents'
  menu has Show All Channels, which shows every hidden channel again, and Undo and Redo Visibility Change. In a
  window, the left zone's Visibility tab (`Mixer::showVisibility`) has a toggle button per channel
  (`button.mixer-visibility-item[visibility=<id>]`), checked while the mixer shows it. Each sends `mixer.set` for its
  own mixer, and the mixer keeps the change at once, so that changes made before the engine answers all count; it
  reads its view again once the last is answered, and ignores that answer while a newer change is still out. The lower
  zone's mixer follows the project window's track visibility, and a window does with Sync Visibility of Project and
  Mixer ([track-visibility.md](track-visibility.md)).
- **Find Track/Channel** (pp. 449-450) opens a selector with a search field (`input.mixer-find-search`) over the
  channels, hidden ones too, but not those of the types the filter hides; typing narrows it to the names with the
  text, in any case. The channel picked shows again if it was hidden, and its strip scrolls into view. The mixer has no
  channel selection yet, so it isn't selected.
- **Set up Window Layout** (p. 439): the setup pane ([docking.md](docking.md)) shows and hides the left zone (in a
  window) and the strips' sections Pictures, Routing, Pre, Equalizer, Strip, Sends, Notepad and Fader, and reorders
  them, with Show All, Reset All and presets ([docking.md](docking.md)). In a window it also shows and hides the menu
  bar, GNUbase's own addition for platforms whose title bar has no Show Menu Bar item (p. 55; the reduced menus are in
  [main-menu.md](main-menu.md)). The layout is the user setting `ui.layout`, per window (`mixer1` to `mixer4`, and
  `project` for the lower zone's).
- **Routing** (pp. 471-473): Input Routing picks an audio track's input bus, Output Routing where a channel plays: an
  output bus, or a group or effect channel whose audio doesn't come back to it (`routesTo`), for a MIDI track an
  instrument; No Bus or Not Connected disconnects. Each sends `channel.set` with `input` or `output`; the lists are the
  inspector's (`inputChoices`, `outputChoices`). The channel settings window has no Routing section, as in the
  reference.
- **Order** (p. 496). A channel's name dragged onto another channel takes its place: its track moves into that
  track's folder with `track.move`, after it when it came from the left, before it otherwise, so the track list
  follows. Busses keep their places, and a folder doesn't move into itself. Any mouse button release in the window
  ends the drag, also one outside the mixer.
- **Track colors** (p. 1621). A strip's name shows its track's color, which is user data: code sets it as the name's
  `background-color` (and the `color` attribute), and marks the name `.colored`, and `.bright` where dark text has
  more contrast over the color than light text (WCAG 2's relative luminance). Every theme gives the text over it
  ([themes.md](themes.md)). Busses and tracks without a color show the theme's.
- **Notes and pictures** (p. 497). The notepad shows the channel's notes; what is typed is sent with `channel.set` when
  the field loses the focus: also on Escape (p. 498), before its strip is made again or the mixer goes (its window
  closing), and before File > Save, Save As and Save New Version, which leave the text fields of every window
  (`ui::ProjectFiles`). Open, New, Revert and Quit leave them too before they ask whether the project has unsaved
  changes, so a note still being typed counts as one. Another project that came in the meantime (`project.reset`) drops
  it, as the inspector does. The picture section shows the track's picture (an `<img>`, or an `<svg>` for SVG files); a
  double click opens the OS's file dialog for a picture, in the mixer's window, then `track.set`.

## Windows and the lower zone

- **Mixer windows** (pp. 437–438): `resources/ui/mixer.rml`, a docking window with the left zone and the main zone
  with the mixer. `window.mixer` (F3, Studio > Mixer) opens the first one in a window of its own, or closes it;
  `window.mixer2` to `window.mixer4` (Studio > Mixer 2 to 4) the others, each with its own view and layout. Like every
  window, each opens on any monitor and renders at its monitor's scale (architecture.md, "UI shell"). The left zone
  has the Visibility tab, a panel (`mixer1Visibility` to `mixer4Visibility`) of the docking system; Tab moves the
  focus between the zones (focus zones `left` and `mixer`); in a window of its own, it opens and closes with its mixer
  window. A window the user closes lets go of its mixer.
- **The lower zone's mixer** (pp. 434–436): the project window's Mixer panel, with the view `lower`.
  `window.lowerZoneMixer` (Alt+F3, Studio > Mixer in Project Window) shows it as the lower zone's tab, or hides the
  lower zone while it shows it; while the panel is in a window of its own, it brings that window to the front with the
  focus. Its toolbar has no left zone button: the project window's left zone isn't the mixer's.

## Channel meters

An audio channel's strip has its meter next to the fader (`.mixer-meter`, reference manual p. 468), a MIDI channel's
strip one without a fader: a `<meter>` per audio channel of the channel's meter (both sides of a mono channel on a
stereo destination), one for a MIDI channel, its notes' velocity, on the +3 dB digital scale (pp. 467-468, 1614), -60 to
+3 dB, with the peak since the last frame and its peak mark, the scale's marks (`.mixer-meter-scale`, 0 to -30 dB every
6 dB, then -40 and -50, and the headroom above 0 dB as a red line, `.meter-headroom`, p. 534) between the fader and the
bars, and under it, beside the fader's volume, the Meter Peak Level
(`.mixer-peak`), the highest of those maxima, which a click resets: every channel's (`meter.reset` with `meters`
`channels`), not the master meter's. The bars come once the engine measured the channel, after it first played. The
mixers read them through `ui::MeterReader`, the UI's one reader of the meters, which the right zone's meter display
shares ([metering.md](metering.md)). A VCA fader's meter sums its channels' outputs after their faders and panners,
whatever `meter.position` says ([link-groups.md](link-groups.md)); busses have no meter yet.

A right click on a meter opens its Global Meter Settings (`#mixer-meter-menu`, p. 467), the user settings that every
mixer's meters follow: Meter Peak - Hold Peaks (`meter.holdPeaks`, on by default) shows the peak marks; Meter Peak -
Hold Forever (`meter.holdForever`) makes a mark the peak maximum, until a reset; without it a mark holds a peak for the
Meters' Peak Hold Time (`meter.peakHoldTime`, 500 to 30000 ms, 2000 by default, on the preferences' Metering page, p.
1614) and then falls to the level, and a reset starts it again. The UI holds the marks, from the peaks of each frame.
Reset Meters resets the channels' maxima, as a click on the Meter Peak Level does. Meter Position - Input, Post-Fader
and Post-Panner (`meter.position`, `[meter-position]`, checked as the setting says) choose where the engine measures the
channels ([metering.md](metering.md)); after the panner by default. The bars fall back from a peak by the Meters'
Fallback (`meter.fallback`, 1 to 100 dB per second, 20 by default, on the preferences' Metering page, p. 1614), and
below their -60 dB end to silence: the UI keeps each bar's level and lowers it by the time since its last frame.

## Not yet

Channel selection and the agents and Move Selected Channels commands that work on it, dragging several channels at
once and the green drop line; the Zones tab (locking channels to the left and right), the History and Snapshots tabs and
the right zone; the channel overview, the meter bridge, Channel Latency, the inserts section (#113), routing several selected channels at once (p. 471) and the
channel meters' colors (the +3 dB digital scale's color handles) and clipping indicators (pp. 467-468, [metering.md](metering.md)); selecting the channel that Find Track/Channel finds, channel visibility configurations, linking mixers ([link-groups.md](link-groups.md) links channels), the zoom palette and the transport buttons of the toolbar (the state buttons: solo-mute-listen.md); the track pictures browser and pictures in the track list; and faders of busses. The
windows' places are kept in `ui.layout` and workspaces ([workspaces.md](workspaces.md)).

## Checks

- `tests/mixerviews_tests.cpp`: each mixer's hidden channels and types, sorted and in the filter's order, kept per
  mixer, refused for unknown ids, types and mixers, not undo steps, without channels that are gone, and saved and
  loaded; each mixer's last ten visibility changes undone and redone, without the sets that change nothing, and the track
  list's changes to the lower mixer undone one at a time; notes
  and pictures as undo steps, copied with a track, saved and loaded; a picture embedded in the file and extracted next
  to it once the original is gone, kept by `media.cleanup`, and referred to relative to the file when not embedded, so
  it loads from a folder that moved.
- `tests/mixerwindow_tests.cpp` drives the app with `ui::Automation`: F3 opens a mixer window with the busses and the
  channels in the track list's order and closes it again; tracks added, renamed, moved and removed by the engine show at
  once; the fader, the dB text and the mute and solo buttons both ways; two windows, the Visibility tab and the type
  filter hiding channels in one window only, Show All Channels and undoing and redoing it, and `mixer.set` from the
  engine; Find Track/Channel listing the channels but the filtered types, narrowing them as typed, and showing a hidden
  channel and scrolling to channels outside the view; Alt+F3 showing the lower zone's mixer with its own view and hiding the zone again; a closed window letting go of its mixer; the setup
  pane's Pictures, Notepad and Fader, notes from the engine and typed in the notepad, and dropped when another project
  loads, kept on Escape, a save from the project window, a strip made again and the window closing, a note being typed
  making Revert ask about unsaved changes, a picture shown and removed, chosen with a double click in the mixer window's
  file dialog, two visibility changes within one step, a name drag ended outside the mixer moving nothing, the
  Visibility tab in a window of its own opening and closing with its mixer window, and channel names dragged onto others
  reordering the tracks, while
  busses stay. The Routing section lists the input busses, and the output busses and the group and effect channels a
  channel can play into, an instrument for a MIDI track, sends `channel.set` and follows the engine. A strip's name shows its track's color as `track.set` changes it, marked bright or not, and the theme's once the color is gone. A fader and a send's level
  dragged and the panner turned while the engine answers late follow the mouse, never back, end where the engine is, and
  are one undo step; values set through MCP show at once, and during a drag when it ends. A fader step the engine
  refuses, its track being gone, shows in the command log, leaves no undo step and, once an undo brings the track back,
  the strip made again shows the engine's volume; a strip made again while its fader's step is in flight shows the
  engine's volume at once, not the widget's default. The lower zone's mixer in the default theme, with colored and plain
  names, is compared with the golden image `lower-zone-mixer.png`.
- `tests/meterpanel_tests.cpp`: an audio channel's strip meter shows no bars and `-inf` before it played, then a bar
  per side at the sine's level with its peak mark, in the frame that the master meter shows the same peak;
  a MIDI channel's has no bars until a note played and then one bar at the note's velocity, and a click on the peak
  level resets the channels' maxima, not the master meter's. A strip's bars go from -60 to +3 dB, and its scale's marks
  sit at their levels' heights beside them. The Global Meter Settings menu: a mark that holds the peak
  for the peak hold time while the level falls and then falls, Hold Forever's mark at the maximum past that time, Reset
  Meters, Hold Peaks off without marks, and Meter Position, which sets `meter.position`, follows it, and moves the bar
  to the level measured there. A strip's bar falls back from a peak by `meter.fallback`, not at once.
