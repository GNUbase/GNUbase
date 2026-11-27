# Studio setup

The studio setup dialog sets up the audio and MIDI devices (#79, reference manual pp. 16–33): the audio driver, the
device's sample rate, buffer size and clock source, how its ports and the MIDI ports are named and which are visible,
and the driver's control panel. It is `resources/ui/studio-setup.rml`, a `<dialog>` from the widget library
([widgets.md](widgets.md)) whose Lua ([ui-lua.md](ui-lua.md)) reads and changes everything through engine commands, so
MCP and Lua scripts can do all it does.

- **Opening it.** The UI command `window.studioSetup` opens it, modal, in the window it runs in
  (`app::AppUi::openStudioSetup`). As in the reference, Studio > Studio Setup in the main menu runs it
  ([main-menu.md](main-menu.md)), and it has no key. There is one dialog: running the command while it is open, as
  MCP and Lua can, brings it to the front with what the user changed in it (#394).
- **Pages.** The devices list on the left selects a page, as in the reference:
  - *Audio System*: the audio driver, a device of any backend in `audio.devices` (unavailable devices say so), the
    device's input and output latency in ms, its sample rate, the record shift in samples and multi-processing
    (`audio.setProcessing`).
  - *the audio device*, named after it: the Control Panel button (`audio.controlPanel`, disabled when the driver has
    none), the latencies, the sample rate, buffer size and clock source the driver offers, Externally Clocked
    (`externallyClocked`, [audio-devices.md](audio-devices.md)), direct monitoring (disabled when the driver can't)
    and the ports: input or output, system name, Show As, Visible and State. When the driver
    lists clock sources but flags none as current, as some ASIO drivers do, the Clock Source list shows an empty
    entry, which Apply and OK don't send, so they don't reopen the device with a source the user didn't pick (#398).
  - *MIDI Port Setup*: the ports of every MIDI backend (`midi.ports`) with Show As, Visible, State (active, inactive or
    not present) and, for inputs, In 'All MIDI Inputs'.
  - *Note Expression Input* (#1514, reference manual pp. 1295-1298): the MIDI Input list picks an input, and below it
    are that input's MPE settings (`mpe` in `midi.ports` and `midi.setPort`, [note-expression.md](note-expression.md)):
    the MIDI Message and Note Expression Assignment of Pressure, Horizontal/X and Vertical/Y (#1674), the Send Initial
    Value of X and Y, Use for Tuning and Pitch Range, Multi-Channel Input Mode (off, MPE's lower or upper zone, channel
    rotation, both zones), Used Channels (with both zones the lower zone's), Upper Zone Channels (only for both zones,
    #1675) and First Channel (only for channel rotation). Use for Tuning shows X's assignment as Tuning and disables it;
    off, X shows its own assignment again (None until another is chosen, #1749), and choosing Tuning for X turns it on.
    At the bottom the MIDI Activity display shows the input's last eight messages (channel, kind, values) as the event
    `midi.activity` brings them; the dialog turns `midi.activityListen` on while it is open and off when it closes, as
    its own `listener` (`studioSetup`), so others that listen go on (#1832). An input that is a known MPE device shows
    it (Detected Device), and the devices list has a page named after the device below Note Expression Input, which is
    this page with its input. Each input has its own fields, so switching inputs keeps what the user changed; Apply and
    OK send the changed inputs' settings; a pitch range that isn't a whole number from 1 to 96 is an error, and that
    input's settings stay as they were. Used Channels offers 16 for channel rotation; a zone gets at most 15, and both
    zones together 14, the lower zone the rest, which the page shows after Apply. Apply builds the page again from the
    engine, still showing the input that was selected.
- **Apply, OK, Cancel.** The pages keep what the user changes until Apply or OK sends it; Cancel and Escape drop it.
  Only what changed is sent: another driver with `audio.setDevice` (the old device's page doesn't apply to the new
  one, so the reference's "Switch" is this Apply) with `remember`, so the app opens it again when it starts, and
  running if the old one ran ([audio-devices.md](audio-devices.md), #712), the device's settings with `audio.set`, ports with `audio.setPort`
  and `midi.setPort`. Multi-processing changes while the device stands, so the dialog stops a running device for it
  and starts it again. Apply reads everything again from the engine and shows the errors of commands that failed; OK
  closes the dialog unless one failed. While a fallback plays because the device chosen last couldn't be used, at
  startup or when another driver was picked (`failed` in `audio.get`), the message line says which, why and what plays
  instead: the dummy device, or the backend's default device with the fallback device `default`
  ([audio-devices.md](audio-devices.md)). The driver list then shows the fallback's driver. Apply or OK with that
  driver picked in the list makes the fallback the user's choice: `audio.setDevice` with `remember`, then `audio.set`
  with what the pages changed. Only once the list was touched, though: an OK only to read the message leaves the
  choice for the next start, and `failed` stays. Reset at the bottom opens the device again (`audio.reset`), as the
  reference's Reset does; while the dummy device plays for the device that failed, it tries that device again and
  opens the dummy device again with its own settings if it still can't be used. The Reset above a ports table
  restores the system names and makes every port visible, sent with Apply or OK.
- **`audio.set`** changes some of the device's settings and keeps the others, including the dummy device's own
  (channels, latencies, loopback, input): the dialog changes the sample rate without losing what a test set up. The
  device opens again, runs on if it ran, and if it can't open with the new settings it opens with the old ones and
  the command fails.
- **Audio ports.** `audio.setPort` gives a port of the device a name to show (`showAs`; empty for the system name) and
  hides or shows it (`visible`), for routing menus and the audio connections window to use. `audio.ports`
  lists both with every port and `audio.portsChanged` sends them when they change. Busses keep using ports by their
  system names, so renaming a port reroutes nothing, and busses on a hidden port keep it. Routing menus and the
  audio connections window are to use both once they exist (#108).
- **Port settings are user settings** (#393), so they last across restarts, as in the reference:
  `audio.portSettings` keys each audio port by `backend/device id/in|out/system name`, `midi.portSettings` each MIDI
  port by `backend/in|out/port id`. An entry keeps only what differs from the defaults (the system name, visible, in
  all MIDI inputs) and stays while its device is unplugged, so the port gets its settings back when it returns.
  `audio.setPort` and `midi.setPort` change only their port's entry, in the value the settings file has when they
  save it (`Engine::modifySetting`, through `settings::Store::modify`), so several GNUbase processes on one settings
  folder keep each other's port settings (#452); then the process uses the file's whole value, applies it and sends
  `settings.changed` like `settings.set`. `audio.ports` and `midi.ports` read it. An entry's schema allows members
  this GNUbase doesn't know, and an entry that doesn't fit leaves the others alone, as for every collection setting
  ([settings.md](settings.md)); `settings.set` therefore accepts a misspelled member, which nothing reads. Whichever
  command changes these keys, the port commands or `settings.set` and `settings.reset`, the engine
  (`Engine::settingsChanged`) sends the ports afterwards: `audio.portsChanged` with `audio.ports` for
  `audio.portSettings`, `midi.changed` with `midi.ports` for `midi.portSettings` (#436). The listed MIDI ports follow
  their settings: hidden inputs close, shown ones open, and the audio thread's "all MIDI inputs" flags update. That
  doesn't list the MIDI hardware again (`MidiPorts::applySettings`, only `midi.refresh` does), and neither does a
  change of any other user setting: a port plugged in meanwhile waits for the `midi.refresh` its backend posts, which
  sends `midi.changed` with it. Before the first MIDI command has listed the ports, there is no `midi.changed`. The settings
  schema validator checks
  `additionalProperties` given as a schema, so a broken value in the file falls back to the default (no port settings)
  instead of reaching the ports.
- **Not yet:** the reference's other devices (remote control, time display, MIDI remote control), the Audio Settings
  dialog that repeats the audio pages, releasing the driver while GNUbase is in the background, processing precision,
  the extra processing buffer and power scheme options, disk preload and the MIDI input and output monitors.
- **Tests:** `tests/studiosetup_tests.cpp` opens the dialog through its UI command on the dummy device and a dummy MIDI
  keyboard and drives it with `ui::Automation`: it shows the driver, the device's settings and its ports; a new sample
  rate, a renamed and a hidden port and a renamed MIDI input that leaves all MIDI inputs reach the engine with OK; the
  Note Expression Input page sets up the keyboard's channel rotation, and an invalid pitch range keeps it open; it sets
  each dimension's MIDI Message and Note Expression Assignment, with Use for Tuning and X's Tuning following each other
  and X's own assignment coming back with Use for Tuning off; it sets up both zones with Upper Zone Channels, only for
  both zones, and shows the lower zone's Used Channels after Apply; MIDI PolyPressure is a Note Expression Assignment; a
  detected LinnStrument has its page, which shows its input with the preset, and the MIDI Activity display shows the
  device's messages, and none of them on another input's fields, and no activity once the dialog is closed; with two
  keyboards, switching between them keeps each one's edits, and after Apply the page still shows the second; Cancel
  drops a change, Apply changes multi-processing while the device runs, a bad record shift keeps the dialog open with
  the message, and Reset reopens the device. With a fake audio backend (`test::UiApp` takes audio backends as it takes
  MIDI backends), Apply switches to its device's driver with `audio.setDevice`, and OK sends no `audio.set` for a device
  without a current clock source until the user picks one. The message line says why the device chosen last can't be
  used and names the fallback that plays instead, the dummy device or the backend's default device; Apply and OK leave
  the fallback alone until the driver list is touched, and then Apply saves it as the user's choice at the block size
  it plays with. Running the command again keeps the one open dialog.
  `tests/audio_tests.cpp` checks `audio.set` and `audio.setPort`, and that port settings survive an engine restart with
  the same settings folder, wait for an unplugged MIDI port and a device that is not open, and follow `settings.reset`;
  that two engines on one settings folder each change a port and the file keeps both; that a later GNUbase's members of
  an entry stay, and an entry that doesn't fit stays in the file while the others count; that repeating the same
  `audio.setPort` or `midi.setPort` sends no second `settings.changed`; and that `settings.set` and `settings.reset` on the port settings send `audio.portsChanged` and
  `midi.changed`, while no settings change lists the MIDI ports again, counted with a dummy MIDI backend.
