# Main menu

The project window's menu bar (#392, reference manual p. 55): menus whose items run the engine's commands and the UI's
own, as keys do, show their keys and are disabled while their command can't run. The menus are in
`resources/ui/project.rml`; `ui::MainMenu` (`src/ui/mainmenu.h`) runs their items, and `app::AppUi` attaches it to the
project window.

- **Items.** A `<menuitem>` with a `command` attribute runs that command through the key commands
  (`KeyCommands::run`, [key-commands.md](key-commands.md)), with the JSON object in its `parameters` attribute:
  `<menuitem command="transport.locate" parameters='{"target": "leftLocator"}'>`. An engine command goes through the
  UI's engine client, so a menu can do nothing that MCP and Lua can't; a UI command gets the window the menu is in and
  the focus zone around the item. Adding an entry is one line in the document; a new feature adds its command (engine
  or UI) and its item, e.g. Import with #92.
- **Checked when attached** (#446). `MainMenu` checks every item before it changes the document: parameters that
  aren't a JSON object, a command that doesn't exist and parameters the command doesn't take (`KeyCommands::check`, as
  for keys) throw `std::invalid_argument` naming the command, so a typo fails the tests instead of shipping a dead
  item. The transport controls check their buttons and the time display's context menu the same way
  ([transport-bar.md](transport-bar.md)).
- **Keys.** Each command item gets a `.menu-keys` element with the first key bound to the command with those
  parameters (`KeyCommands::keysFor`), as the platform writes it: Ctrl+Z, or Cmd+Z on macOS.
- **Disabled.** An item is `disabled` while `KeyCommands::canRun` says its command can't run: what
  `KeyCommands::enableWhen` set for it, e.g. Undo and Redo while the undo history (`history.changed`) has nothing to
  undo or redo, and the project window's edit commands without the event display. The menu updates in every step of
  the shell, after the window system's input of that step, so the DOM that MCP's UI tools read is always current. Input
  from `ui::Automation` (MCP's and Lua's UI tools, the UI tests) comes outside the steps: after each click, press,
  release, key, move, drag, wheel turn, text or drop, once all of its listeners and default actions have run, the
  shell's input handler updates the menu (`Shell::onInputHandled`, set by `app::AppUi`). So the DOM is current right
  after every input, whatever changed and in whichever listener: a right click that selected a track and opened the
  context menu, a key that selected a track on its press (#1069), a tool button that dropped the selection range, a
  list view that selects its row in the click's default action (#1120), the event display that selects on the
  release, and an item's own command, such as Edit > Select > All or None (#1173). For this update the order in which
  listeners were added doesn't matter, and the menu doesn't update on every change event, such as each step of a value
  drag (#1176). A click on an item updates the menu once more before it looks for the item, so a listener of the click
  that changes what the item's command can do counts (#1120), if it ran before the menu's own click listener on the
  document: a listener of an element below the document, or of the document added before the menu. A click listener
  added to the document after the menu runs after the lookup: what it changes shows after the input, too late for
  that click (#1233).
- **The menu bar** is a docking strip, `strip="menuBar"` ([docking.md](docking.md)): Menu Bar in the Set up Window
  Layout menu and the UI command `window.menuBar` show and hide it, and `ui.layout` keeps it per window. It is a
  `<menubar>`, whose titles are buttons that open the menus ([widgets.md](widgets.md), "Menus and context menus"), and
  submenus are menus inside items, marked by an arrow. With a menu open, the mouse over another title opens that
  title's menu instead; Left and Right move between the menus, and into and out of submenus; Alt on its own opens the
  File menu on Windows and Linux, and closes it again.

## Menus

| Menu | Items |
|---|---|
| File | New Project, Open...; Save, Save As..., Back up Project..., Save as Template..., Save New Version, Revert ([project-files.md](project-files.md)); Import (Audio File... `file.importAudio`, MIDI File... `file.importMidi`, [file-import.md](file-import.md); Tempo Track... `file.importTempo`; Markers... `file.importMarkers`, [markers.md](markers.md); Tracks from Project or Track Archive... `file.importTracks`, [track-presets.md](track-presets.md)); Export (Audio Mixdown... `file.exportMixdown`, Quick Audio Export... `file.quickExport`, [mixdown.md](mixdown.md); Tempo Track... `file.exportTempo`, [tempo-track.md](tempo-track.md); Markers... `file.exportMarkers`; Selected Tracks... `file.exportTracks`, [track-presets.md](track-presets.md)); Quit (`file.quit`, Mod+Q) |
| Edit | Undo, Redo; Delete; Functions (Split at Cursor, Split Loop, Duplicate, Repeat..., Fill Loop, Convert to Real Copy); Select (All, None, Invert, In Loop, From Start to Cursor, From Cursor to End, Events under Cursor); Move (Event Start to Cursor, Event End to Cursor); Render in Place (Render (with Current Settings)), Freeze/Unfreeze (Selected Tracks (with Current Settings)) (`track.renderSelected`, `track.freezeSelected`, [render-in-place.md](render-in-place.md)); Mute, Unmute, Lock, Unlock; Quantize (`edit.quantize`, Q), Reset Quantize, Quantize Panel (`window.quantizePanel`), Advanced Quantize (Quantize MIDI Event Lengths, Quantize MIDI Event Ends, Freeze MIDI Quantize, Create Groove Quantize Preset, [quantize.md](quantize.md)); Open/Close Editor, Open Editor in Lower Zone, Open List Editor, Open Drum Editor (`editor.openClose`, `window.lowerZoneEditor`, `editor.openList`, `editor.openDrum`, [midi-editors.md](midi-editors.md), [list-editor.md](list-editor.md), [drum-editor.md](drum-editor.md)); Load Drum Map..., Save Drum Map... (`drumMap.load`, `drumMap.save`, [drum-editor.md](drum-editor.md)); Macros (the user's macros, `macro.<name>`, and Key Commands...), Key Commands... (`window.keyCommands`, [key-commands.md](key-commands.md)); Profile Manager... (`window.profiles`, [profiles.md](profiles.md)); Preferences... (`window.preferences`, [preferences.md](preferences.md)) |
| Media | Open Pool Window (`window.pool`, Mod+P); Import Medium... (`pool.import`); Create Folder; Remove Unused Media; Find Missing Files... (`pool.findMissing`); Remove Missing Files; Conform Files ([pool.md](pool.md)); Prepare Archive ([backup.md](backup.md)) |
| Transport | Start, Stop, Start/Stop, Record, Lock Record, Unlock Record ([recording.md](recording.md)); Audio Record Mode (Keep History, Cycle History + Replace, Replace, [recording.md](recording.md)); MIDI Record Mode (New Parts, Merge, Replace, [midi-recording.md](midi-recording.md); Mix, Overwrite, Keep Last, Stacked, Mix-Stacked (No Mute), [cycle-recording.md](cycle-recording.md)); Go to Project Start; Locators (Go to Left/Right Locator Position, Set Left/Right Locator to Project Cursor Position, Exchange Left & Right Locator Positions); Set Project Cursor Position (Locate Next/Previous Marker) |
| Project | Add Track...; Project Input Transformer... `project.inputTransformer`, [midi-rules.md](midi-rules.md); Project Rule Editor... `project.ruleEditor`, [project-rules.md](project-rules.md); Tempo Track (Open Tempo Track Editor, Activate Tempo Track, Tempo Detection... `tempo.detection`, [tempo-detection.md](tempo-detection.md); Process Tempo... `tempo.openProcessTempo`, Process Bars... `tempo.openProcessBars`, [time-warp.md](time-warp.md)); Chord Track (Chords to MIDI `track.chordsToMidiSelected`, Create Chord Events `track.createChordEventsSelected`, [chord-track.md](chord-track.md)); Show/Hide Chord Pads (`window.chordPads`, [chord-pads.md](chord-pads.md)); Remove Selected Tracks, Remove Empty Tracks, Duplicate Tracks; Track Folding; Auto Fades Settings... (`project.autoFades`, [fades.md](fades.md)); Project Setup... (`window.projectSetup`, [project-setup.md](project-setup.md)) |
| Audio | Fades (Fade In to Range Start, Fade Out from Range End, Adjust Fades to Range, Crossfade (X), Remove Fade In, Remove Fade Out, Remove Fades, Open Fade Editor(s), [fades.md](fades.md)); Advanced (Detect Silence... `audio.detectSilence`; Set Definition from Tempo... `audio.setDefinitionFromTempo`, Copy Warp Markers from Selected Event `audio.copyWarpMarkers`, Paste Warp Markers to Selected Events `audio.pasteWarpMarkers`, [time-warp.md](time-warp.md)), Spectrum Analyzer (`audio.spectrumAnalyzer`), Statistics (`audio.statistics`) ([audio-functions.md](audio-functions.md)); Create Sampler Track (`audio.createSamplerTrack`, [sampler-track.md](sampler-track.md)); Realtime Processing (Flatten Realtime Processing `audio.flatten`, Unstretch Audio `audio.unstretch`, [warp.md](warp.md)) |
| MIDI | Open In-Place Editor (`editor.openInPlace`, [in-place-editor.md](in-place-editor.md)); Transpose Setup...; Rule Editor... `midi.ruleEditor`, [midi-rules.md](midi-rules.md); Articulation Map Setup... `midi.articulationMapSetup`, [articulation-maps.md](articulation-maps.md); Merge MIDI in Loop..., Dissolve Part..., Bounce MIDI; Functions (Legato, Fixed Lengths, Fixed Velocity, Pedals to Note Length; Delete Overlaps (mono), Delete Overlaps (poly), Delete Doubles, Delete Controllers, Delete Continuous Controllers, Restrict Polyphony..., Thin Out Data; Reverse, Mirror, [midi-functions.md](midi-functions.md); Calculate Tempo from MIDI Events... `midi.calculateTempo`, [tempo-detection.md](tempo-detection.md)) |
| Studio | Mixer, Mixer in Project Window, Mixer 2, 3 and 4 (`window.mixer`, `window.lowerZoneMixer`, `window.mixer2` to `window.mixer4`, [mixer-window.md](mixer-window.md)), On-Screen Keyboard (`window.onScreenKeyboard`, Alt+K, [onscreen-keyboard.md](onscreen-keyboard.md)), More Options > MIDI Device Manager (`window.midiDeviceManager`, [midi-devices.md](midi-devices.md)), Studio Setup... (`window.studioSetup`, [studio-setup.md](studio-setup.md)) |
| Workspaces | Add Workspace... (`window.addWorkspace`), Update Workspace (`window.updateWorkspace`), Organize... (`window.workspaces`), No Workspace and the workspaces ([workspaces.md](workspaces.md)) |
| Window | Windows... (`window.windows`, [workspaces.md](workspaces.md)) |

The other menus of the reference (Help) and their items come with the
features they run. Quitting waits for the shell's next step, since it closes every window.

## Not yet

The reduced menus of other windows (the mixer and the editors, when they open in windows of their own); Show Menu Bar
in the context menu of a window's title bar on Windows, which the OS draws (it is in Set up Window Layout for now); the
system menu bar on macOS, where the window shows its own menu bar until then (and Alt doesn't open it).

## Checks

`tests/mainmenu_tests.cpp` drives the project window in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) with
`ui::Automation` clicks, as MCP's `ui_click` does: Studio > Studio Setup opens the studio setup dialog and closes the
menu; items show their keys per platform; Transport > Locators > Go to Right Locator Position sends `transport.locate`
with its parameters through the UI's client; Edit > Select > All and Edit > Delete run the event display's commands;
Undo and Redo are disabled and enabled as the history changes, and a disabled item ignores clicks; Edit > Select >
All and None, and a Ctrl-click on a part that takes it out of the selection on the release, enable and disable
Quantize right after the click; a click listener of the item, or of the document added before the menu, that lets the
item's command run makes the click run it, and one added to the document after the menu only the next click; every
input of `ui::Automation` (move, click, drag, press, release, wheel, key down, up and press, text and drop) runs the
shell's input handler once, after the input's listeners and the shell's unused key and key release handlers (#1233);
`canRun` and `enableWhen`; the menu bar hidden from Set up Window Layout and shown with `window.menuBar`; File > Quit
quits; and the File menu's Import item shows its arrow, the mouse moves from File to Edit, Right and Left move to Media
and back, and Alt opens and closes the File menu (mouse moves and key presses, as MCP's `ui_move` and `ui_key` do). A
menu with an item whose command doesn't exist or is empty, whose parameters an engine command doesn't take or has of the
wrong type, that gives a UI command parameters, or whose parameters aren't a JSON object throws and leaves the document
as it was; the project window, the transport panel and the time display window open, and every `command` in the
documents and fragments of `resources/ui` passes `KeyCommands::check`, as does every command their buttons run from Lua
with `gnubase.ui.command`.
