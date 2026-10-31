# Studio setup

The studio setup dialog sets up the audio and MIDI devices (#79, reference manual pp. 16–33): the audio driver, the
device's sample rate, buffer size and clock source, how its ports and the MIDI ports are named and which are visible,
and the driver's control panel. It is `resources/ui/studio-setup.rml`, a `<dialog>` from the widget library
([widgets.md](widgets.md)) whose Lua ([ui-lua.md](ui-lua.md)) reads and changes everything through engine commands, so
MCP and Lua scripts can do all it does.

- **Opening it.** The UI command `window.studioSetup` opens it, modal, in the window it runs in
  (`app::AppUi::openStudioSetup`). The reference opens it from its Studio menu and has no key for it; GNUbase's main
  menu will run the command too.
- **Pages.** The devices list on the left selects a page, as in the reference:
  - *Audio System*: the audio driver, a device of any backend in `audio.devices` (unavailable devices say so), the
    device's input and output latency in ms, its sample rate, the record shift in samples and multi-processing
    (`audio.setProcessing`).
  - *the audio device*, named after it: the Control Panel button (`audio.controlPanel`, disabled when the driver has
    none), the latencies, the sample rate, buffer size and clock source the driver offers, direct monitoring (disabled
    when the driver can't) and the ports: input or output, system name, Show As, Visible and State.
  - *MIDI Port Setup*: the ports of every MIDI backend (`midi.ports`) with Show As, Visible, State (active, inactive or
    not present) and, for inputs, In 'All MIDI Inputs'.
- **Apply, OK, Cancel.** The pages keep what the user changes until Apply or OK sends it; Cancel and Escape drop it.
  Only what changed is sent: another driver with `audio.setDevice` (the old device's page doesn't apply to the new
  one, so the reference's "Switch" is this Apply), the device's settings with `audio.set`, ports with `audio.setPort`
  and `midi.setPort`. Multi-processing changes while the device stands, so the dialog stops a running device for it
  and starts it again. Apply reads everything again from the engine and shows the errors of commands that failed; OK
  closes the dialog unless one failed. Reset at the bottom opens the device again (`audio.reset`), as the reference's
  Reset does; the Reset above a ports table restores the system names and makes every port visible, sent with Apply
  or OK.
- **`audio.set`** changes some of the device's settings and keeps the others, including the dummy device's own
  (channels, latencies, loopback, input): the dialog changes the sample rate without losing what a test set up. The
  device opens again, runs on if it ran, and if it can't open with the new settings it opens with the old ones and
  the command fails.
- **Audio ports.** `audio.setPort` gives a port of the device a name to show (`showAs`; empty for the system name) and
  hides or shows it (`visible`), for routing menus and the audio connections window to use. `audio.ports`
  lists both with every port and `audio.portsChanged` sends them when they change. Busses keep using ports by their
  system names, so renaming a port reroutes nothing, and busses on a hidden port keep it. The settings are kept per
  device (backend, device id, direction, name) while GNUbase runs, like the MIDI ports'; saving them with the user
  settings comes later.
- **Not yet:** the reference's other devices (remote control, time display, MIDI remote control), the Audio Settings
  dialog that repeats the audio pages, releasing the driver while GNUbase is in the background, processing precision,
  the extra processing buffer and power scheme options, disk preload and the MIDI input and output monitors.
- **Tests:** `tests/studiosetup_tests.cpp` opens the dialog through its UI command on the dummy device and a dummy MIDI
  keyboard and drives it with `ui::Automation`: it shows the driver, the device's settings and its ports; a new sample
  rate, a renamed and a hidden port and a renamed MIDI input that leaves all MIDI inputs reach the engine with OK;
  Cancel drops a change, Apply changes multi-processing while the device runs, a bad record shift keeps the dialog
  open with the message, and Reset reopens the device. `tests/audio_tests.cpp` checks `audio.set` and
  `audio.setPort`.
