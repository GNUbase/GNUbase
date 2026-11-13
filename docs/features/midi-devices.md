# MIDI devices and patch banks

MIDI devices stand for external instruments with their patches by name, and a MIDI track chooses its patch, the
program and bank it sends to its instrument (#136, reference manual pp. 1108–1114).

- **A MIDI track's patch** (p. 1108). `channel.set` `program` (0 to 127) and `bank` (0 to 16383), `null` for none, the
  default, are a MIDI channel's patch. The channel sends it to its instrument as a program change on its MIDI channel
  (`midiChannel`, channel 1 for Any, [mixer.md](mixer.md)), after bank select MSB (CC 0) and LSB (CC 32) when it has a
  bank: whenever they change, also while the transport stands, and again where playback starts, locates or jumps, so the
  instrument has the patch wherever playback begins. A silent MIDI track (muted, soloed out or disabled) sends neither,
  and once it is heard again only a patch that changed meanwhile ([track-list.md](track-list.md)). Without a program
  nothing is sent, also with a bank. It is a static lane of the channel's automation, before the MIDI volume and pan
  (`PlayedLane::Kind::Program` in `src/engine/audio/automation.h`), so the audio thread only emits an event it was
  given; a program change of a part plays over it where it is. `project.get` shows them with the channel, the project
  file keeps them in GNUbase's entry, and undo takes them back.
- **MIDI devices** (pp. 1109–1114) are the user setting `midi.devices`, so they belong to the user, not to a project,
  as in the reference: devices by name, each with banks, each bank a name and patches, each patch a name, a program,
  an optional bank and an optional folder it is listed in. `midiDevice.install` installs one: with `template` `gm`
  General MIDI's 128 programs in the bank GM, in folders by family (the reference's GM Device), or with `empty`, the
  default, no banks (Define New). `midiDevice.remove` removes one. Banks and patches are edited with `settings.set`,
  whose schema checks them. A MIDI track names a device with `channel.set` `device` (empty for none); a device that
  is removed keeps the tracks' program and bank.
- **The inspector** (p. 1113, [inspector.md](inspector.md)). Its Routing section has three more rows on MIDI tracks:
  Device (`select[control=device]`: No Device, the installed devices, and a device the track names that isn't
  installed, "(not installed)"), Bank (`textdisplay[control=bank]`, "Off" or 1 to 16384: a double click types it,
  anything else is Off) and Program (`select[control=program]`). Without a device the program selector offers Off and
  1 to 128 and keeps the bank; with one, the Bank row goes (`.unavailable`) and the program selector offers Off and
  the device's patches by name ("Folder / Name", with the bank's name first when the device has several banks), each
  with its program and bank, and lists the track's own patch as "Program N" if no patch has it. Programs and banks
  show from 1, as in the reference; the engine counts from 0, as MIDI does. The inspector follows `settings.changed`.
- **The MIDI device manager** (pp. 1109–1113): `resources/ui/midi-devices.rml`, a modal dialog that the UI command
  `window.midiDeviceManager` opens, as Studio > More Options > MIDI Device Manager does
  ([main-menu.md](main-menu.md)); running it again brings the open one to the front. It lists the installed devices
  and shows the selected one's patch banks as a tree of banks, folders and patches with their programs and banks.
  Install Device installs a GM Device or a new one (Define New) with the name typed and selects it; Remove Device
  removes the selected one. Changes go to the engine at once (its Lua runs `midiDevice.install` and
  `midiDevice.remove`), and failures show at the bottom.

## Not yet

- Devices playing through a MIDI output port: MIDI tracks don't play to MIDI ports yet, so a device has no Output
  and doesn't replace the track's output routing as in the reference (p. 1113); the track names it beside its output.
- The per-channel Bank Assignment (p. 1109), which picks a device's bank by the track's MIDI channel.
- Editing banks, folders and patches in the manager (Enable Edit, Create Bank, New Folder, New Preset, Add Multiple
  Presets, renaming, drag and drop; pp. 1111–1112): `settings.set` does it for now. Preset events other than bank
  select MSB and LSB and program change (p. 1112), the reference's other device scripts (XG and the instruments'),
  Export Setup and Import Setup (XML), device maps and device panels with their automation (pp. 1115–1117), and the
  program selector in the track list.

## Checks

- `tests/midipatches_tests.cpp`: installing a GM device and an empty one, a taken or empty name refused, removing,
  patches edited and a broken one refused with `settings.set`; a MIDI track's program, bank and device set, refused on
  an instrument track and out of range, saved and loaded, undone; and a patch selected reaching the instrument
  (`midi.capture`) as bank select MSB and LSB and program change, at once while stopped, again when playback starts,
  the program change alone without a bank, nothing without a program, and a device's patch.
- `tests/mididevices_ui_tests.cpp` drives the app with `ui::Automation`: the inspector choosing a program and typing a
  bank, then with an installed device its patches by name, the bank row gone, the track's own patch listed, a patch
  chosen and a removed device shown as not installed; the MIDI device manager installing a GM device with its bank,
  folders and patches, defining a new one, refusing a taken name, removing the selected device and closing.
