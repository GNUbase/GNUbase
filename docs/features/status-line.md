# Status line

The project window's status line (#407, reference manual pp. 26, 71, 129), a strip of the docking window
([docking.md](docking.md)) below the zones, hidden by default and shown from the Set up Window Layout menu. The code is
`ui::StatusLine` (`src/ui/statusline.h`); `app::AppUi::showProject` makes it for `resources/ui/project.rml`.

- **Record format.** `#record-format` shows the project's sample rate, e.g. "Record: 44.1 kHz". While the audio device
  runs at another rate, because it doesn't support the project's ([audio-devices.md](audio-devices.md)), the field has
  the class `mismatch`, which the themes color as a warning (p. 26: the field "is highlighted in a different color").
  The bit depth joins it with the project setup (#78), which brings the project's record bit depth.
- **Lost audio device.** While a device is lost, `#lost-device` shows "Audio Device Lost: " and the device's id, with
  the reason it can't be opened as its tooltip (`title`). Otherwise it has the class `hidden`. It goes once the device
  returns or another one is picked.
- **The engine.** Both follow `audio.changed`, after `audio.get` when the window opens: `projectSampleRate` (also there
  without a device), `device.sampleRateMismatch` and `lost`. The engine sends `audio.changed` for every new project
  sample rate, also when the device stays at its rate, so the status line needs no project events. MCP and Lua read
  the same with `audio.get`, and the UI tools see the classes and the tooltip (`ui_dom`).
- Not done yet: the reference's other fields (p. 71), such as the maximum record time, the project frame rate, the pan
  law and the buffer size, hiding fields from the status line's context menu, and opening the project setup dialog
  with a click on the record format.
