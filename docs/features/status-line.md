# Status line

The project window's status line (#407, reference manual pp. 26, 71, 129), a strip of the docking window
([docking.md](docking.md)) below the zones, hidden by default and shown from the Set up Window Layout menu. The code is
`ui::StatusLine` (`src/ui/statusline.h`); `app::AppUi::showProject` makes it for `resources/ui/project.rml`.

- **Record format.** `#record-format` shows the project's sample rate and record bit depth, e.g. "Record: 44.1 kHz -
  24 bit" or "- 32 bit float" (#454). While the audio device runs at another rate, because it doesn't support the
  project's ([audio-devices.md](audio-devices.md)), the field has the class `mismatch`, which the themes color as a
  warning (p. 26: the field "is highlighted in a different color"). A click on it opens the project setup dialog
  (p. 71, [project-setup.md](project-setup.md)).
- **Remaining record time.** `#record-time` shows how long the record-enabled audio tracks can still record
  (#104, p. 356), from `recording.remainingTime` ([recording.md](recording.md)): "Max. Record Time: 52 h 07 min", under
  an hour "4 min 09 s", and "-" while no track would record or the disk has no measurement yet. The status line reads
  it again every second (`StatusLine::update`, which `app::AppUi` calls with `Shell::every`), as arming tracks, the
  record format and the disk filling up, also by other programs, change it; only while the field is shown, and with
  no question of its own still waiting (#645). Shown again, the field follows within a second. The Max. Record Time
  window (Studio > More Options > Max. Record Time, `window.maxRecordTime`, p. 356) shows the same field in a window
  of its own (`resources/ui/max-record-time.rml`, another `StatusLine`), which workspaces open and place as the other
  windows; tests: `tests/statusline_tests.cpp`.
- **Lost audio device.** While a device is lost, `#lost-device` shows "Audio Device Lost: " and the device's name, with
  the reason it can't be opened and the device's id in parentheses as its tooltip (`title`) (#455): the id tells devices
  of one name apart, and is left out for a device without a name, which shows its id already. Otherwise it has the
  class `hidden`. It goes once the device returns or another one is picked.
- **Silent audio device.** While the device plays silence although it runs, e.g. a PipeWire device whose graph another
  client holds at another sample rate ([audio-devices.md](audio-devices.md), #422), `#silent-device` shows "Audio
  Device Silent: " and the device's name (its id if it has none), with the reason as its tooltip (#1974). The themes
  color it as a warning. Otherwise it has the class `hidden`.
- **The engine.** The fields follow `audio.changed`, after `audio.get` when the window opens: `projectSampleRate` and
  `recordBitDepth` (also there without a device), `device.sampleRateMismatch`, `device.silenceReason` and `lost`. The
  engine sends `audio.changed` for every new project sample rate and record bit depth, also when the device stays at
  its rate and after undo, as the audio engine follows both in the render snapshot, so the status line needs no
  project events. MCP and Lua read the same with `audio.get`, and the UI tools see the classes and the tooltip
  (`ui_dom`).
- Not done yet: the reference's other fields (p. 71), such as the project frame rate, the pan
  law and the buffer size, and hiding fields from the status line's context menu.
