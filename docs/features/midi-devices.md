# MIDI devices and patch banks

MIDI devices stand for external instruments with their patches by name, and a MIDI track chooses its patch, the
program and bank it sends to its instrument (#136, reference manual pp. 1108–1114).

- **A MIDI track's patch** (p. 1108). `channel.set` `program` (0 to 127) and `bank` (0 to 16383), `null` for none, the
  default, are a MIDI channel's patch. The channel sends it to its instrument as a program change on its MIDI channel
  (`midiChannel`, channel 1 for Any, [mixer.md](mixer.md)), after bank select MSB (CC 0) and LSB (CC 32) when it has a
  bank: whenever they change, also while the transport stands, and again where playback starts, locates or jumps, so the
  instrument has the patch wherever playback begins. Rerouting sends the patch, MIDI volume and pan to the new
  instrument at once. Stopping at the playback cursor sends none of them again; returning to the start or locating while
  stopped does. A silent MIDI track (muted, soloed out or disabled) sends neither, and once it is heard again only a
  patch that changed meanwhile ([track-list.md](track-list.md)). Without a program nothing is sent, also with a bank. It
  is a static lane of the channel's automation, before the MIDI volume and pan (`PlayedLane::Kind::Program` in
  `src/engine/audio/automation.h`), so the audio thread only emits an event it was given; a program change of a part
  plays over it where it is. `project.get` shows them with the channel, the project file keeps them in GNUbase's entry,
  and undo takes them back.
- **MIDI devices** (pp. 1109–1114) are the user setting `midi.devices`, so they belong to the user, not to a project, as
  in the reference: devices by name, each with banks, each bank a name, patches and optional folders, each patch a name,
  a program, an optional bank and an optional folder it is listed in, a path with `/` between nested folders
  (`Keys/Pianos`). A bank's `folders` are folders it keeps while no patch is in them. `midiDevice.install` installs one: with `template` `gm` General
  MIDI's 128 programs in the bank GM, in folders by family (the reference's GM Device), or with `empty`, the default, no
  banks (Define New). `midiDevice.remove` removes one. Banks and patches are edited with `midiDevice.set` (one installed
  device's `name` and `banks`, returning the device) or `settings.set`; both check the same schema. A MIDI track names a
  device with `channel.set` `device` (empty for none); a device that is removed keeps the tracks' program and bank.
- **The inspector** (p. 1113, [inspector.md](inspector.md)). Its Routing section has three more rows on MIDI tracks:
  Device (`select[control=device]`: No Device, the installed devices, and a device the track names that isn't
  installed, "(not installed)"), Bank (`textdisplay[control=bank]`, "Off" or 1 to 16384: a double click types it,
  anything else is Off) and Program (`select[control=program]`). Without a device the program selector offers Off and
  1 to 128 and keeps the bank; with one, the Bank row goes (`.unavailable`) and the program selector offers Off and
  the device's patches by name ("Folder / Name", with the bank's name first when the device has several banks), each
  with its program and bank, and lists the track's own patch as "Program N" if no patch has it. Programs and banks
  show from 1, as in the reference; the engine counts from 0, as MIDI does. The inspector follows `settings.changed`.
- **The MIDI device manager** (pp. 1109–1113): `resources/ui/midi-devices.rml`, a modal dialog that the UI command
  `window.midiDeviceManager` opens, as Studio > More Options > MIDI Device Manager does ([main-menu.md](main-menu.md));
  running it again brings the open one to the front. It lists the installed devices and shows the selected one's patch
  banks as a tree of banks, folders and patches with their programs and banks. Install Device installs a GM Device or a
  new one (Define New) with the name typed and selects it; Remove Device removes the selected one. Changes go to the
  engine at once (its Lua runs `midiDevice.install` and `midiDevice.remove`), and failures show at the bottom. The open
  dialog follows `settings.changed`, keeping the selected device while it is installed, and groups patches by folder
  even when they are interleaved in the setting. Enable Edit unlocks Create Bank, New Folder, New Preset and Add
  Multiple Presets, renaming a bank, folder or preset, editing a preset's program (1 to 128), optional bank (blank or 1
  to 16384) and folder, removing a bank, folder or preset, and moving presets by drag and drop (p. 1111). Apply writes these edits with `midiDevice.set`; invalid numbers leave
  the setting unchanged. A preset's folder field groups it with other presets of that folder; a blank field puts it
  directly in its bank. Add Multiple Presets takes a count and uses the selected preset's name (with numbered suffixes),
  starting program, bank and folder, or starts at program 1 without a template. The range must fit programs 1 to 128.
  Backspace in the patch tree removes its selected bank, folder or preset when Enable Edit is on.
- **Folders** (p. 1111). New Folder makes a folder ("New Folder", numbered when the name is taken) in the selected bank
  or folder, or in the selected preset's folder, and keeps it while it is empty; New Preset and Add Multiple Presets add
  theirs in the selected folder too. Folders nest: the tree shows a folder's subfolders and presets in the order they
  first appear in the bank's patches, then its folders without presets. Renaming a folder (a name without `/`) takes the
  folders and presets inside it along, and removing it removes them. A preset dragged onto a bank or a folder goes to
  its end, one dragged onto a preset goes before it, into that preset's bank and folder. The folder it leaves stays,
  also once it has no preset left.
- **Bank Assignment** (pp. 1109, 1111). A device with several banks can give a MIDI channel one of them:
  `bankAssignment`, the bank's index in `banks` by the channel's number from `"1"` to `"16"`, which `midiDevice.set`
  replaces (a channel it leaves out uses all banks) and keeps by index when it is left out, dropping indices past the
  last bank, so a caller that removes a bank passes the shifted assignment, as the manager does. The inspector's program
  selector on a track whose MIDI channel has a bank (Any counts as 1, GNUbase's choice) offers only that bank's patches,
  without the bank's name. The manager shows it for a device with several banks (`#bank-assignment`): a MIDI channel and
  its bank, All Banks for none; a bank that the manager removes takes its channels' assignment with it, and the channels
  of the banks after it keep theirs (`tests/mididevices_ui_tests.cpp`, `tests/midipatches_tests.cpp`).
- **Export Setup and Import Setup** (pp. 1110, 1115). `midiDevice.export` writes every installed device, with its banks,
  patches and Bank Assignment, into a MIDI device setup file: XML of GNUbase's own, `<MidiDevices>` with a
  `<Device name>` each, its `<Bank name>`s with their `<Patch name program bank folder>`s and `<Folder name>`s, and
  its `<BankAssignment channel bank>`s, programs and banks from 0 as in `midi.devices`. The reference's setup files have a
  format of their own, which GNUbase doesn't read. `midiDevice.readSetup` lists a file's devices without installing
  them, and `midiDevice.import` installs the ones it names, or all: a name that is installed already or that the file
  doesn't have installs none, and the file's devices are checked against the schema of `midi.devices`. The manager's
  Export Setup asks for the file (`MIDI Devices.xml` at first, `.xml` added when the name has no extension) and shows
  why the export failed; Import Setup asks for a file and opens Import MIDI Devices
  (`resources/ui/import-midi-devices.rml`), which lists its devices with a check box each, checked unless a device of
  that name is installed, and installs the checked ones with OK, or shows why it couldn't. They are the UI commands
  `midiDevice.exportSetup` and `midiDevice.importSetup`.

## Not yet

- Devices playing through a MIDI output port: MIDI tracks play to MIDI ports ([midi-output.md](midi-output.md)), but a
  device has no Output yet and doesn't replace the track's output routing as in the reference (p. 1113); the track
  names it beside its output.
- Preset events other than bank select MSB and LSB and program change (p. 1112), the reference's other device scripts
  (XG and the instruments'), device maps and device panels with their automation (pp. 1115–1117), and the program
  selector in the track list.

## Checks

- `tests/automation_tests.cpp`: newer automation arriving while the old graph still plays does not consume a new MIDI
  destination; its patch and controllers go out when the graph changes destination, while stopped and playing.
- `tests/midipatches_tests.cpp`: installing a GM device and an empty one, a taken or empty name refused, removing,
  patches edited and a broken one refused with `settings.set`; a MIDI track's program, bank and device set, refused on
  an instrument track and out of range, saved and loaded, undone; and a patch selected reaching the instrument
  (`midi.capture`) as bank select MSB and LSB and program change, at once while stopped, again when playback starts, the
  program change alone without a bank, nothing without a program, and a device's patch; stopping after a part's program
  and controllers without resending the track's values, Return to Start resending them, rerouting while stopped and
  playing, loading ignoring program and bank on non-MIDI channels, the device bank limit checked, and `midiDevice.set`
  replacing banks, empty lists and a bank's empty and nested folders without changing other devices or accepting
  missing devices or malformed banks;
  `midiDevice.export` writing a setup, with its banks' folders, that `midiDevice.readSetup` lists without installing and `midiDevice.import`
  installs, chosen devices or all, refusing taken names, names the file doesn't have, files that are missing or aren't
  setups, broken patches and a device twice. On a MIDI track's channel ([mixer.md](mixer.md)): the patch, MIDI volume
  and pan going out on it and again on another; its parts', automation's (a controller, pitch bend and channel pressure)
  and input's events (notes, controllers, pitch bend, pressure and program changes) on it, a held key's note-off on the
  channel its note-on went to, and Any keeping theirs; a drum sound's own channel; sound slots' output events in
  playback and from remote triggers (key switches with and without latch, program changes), a selected slot going out
  again on a new channel and back to Any, and one with a channel of its own staying there; a send before the MIDI
  modifiers getting the input there, through the drum map (a drum sound on Any and one with a channel of its own), Live
  Input and the slot that the remote triggers selected, without the trigger, also once it starts to play again after the
  trigger, with the map's slots changed before, and while a trigger without latch is held, whose release brings the
  default slot back there too; and automation lanes of one controller playing there side by side: of two, only the one
  whose latest point lies latest sending where playback starts or locates; two lanes of the MIDI volume, the first
  sending the channel's before its first point and in its gaps, the second nothing; every point sent, also one that
  repeats its lane's value, and the value a lane's last point left standing in its gap sent again at a locate, also past
  two lanes that end at one position, where the second one's terminator wins; a point that a gap follows, a terminator
  that repeats its lane's value and the lanes' last points with virgin territory, each sent when played through and
  again at a locate past it; and at a cycle jump the value of the lane whose point lies latest.
- `tests/mididevices_ui_tests.cpp` drives the app with `ui::Automation`: the inspector choosing a program and typing a
  bank, then with an installed device its patches by name, the bank row gone, the track's own patch listed, a patch
  chosen and a removed device shown as not installed; the MIDI device manager installing a GM device with its bank,
  folders and patches, defining a new one, refusing a taken name, removing the selected device and closing; following
  external installs, settings edits and removals while preserving selection, closing and reopening, interleaved folders
  shown once with all their patches, and Enable Edit creating, renaming and removing banks, folders and presets, editing
  program, optional bank and folder, escaping names, Backspace removal, adding a preset range from a template, and
  refusing invalid numbers or an overflowing range without changing settings; New Folder making empty and nested
  folders with unique names, a renamed folder taking its subfolders along and refusing `/`, a preset added in a nested
  folder, presets dragged onto another bank, onto a preset and onto a folder, a folder removed with what is inside it,
  and a drag without Enable Edit moving nothing; Export Setup writing every device into the
  file chosen and Import Setup offering the file's devices, the installed ones unchecked, installing the checked ones
  and showing why a taken name installs none.
