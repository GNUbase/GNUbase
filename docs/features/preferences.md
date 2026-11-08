# Preferences

The preferences dialog (#162, reference manual pp. 1593-1625) shows the user settings that are preferences
([settings.md](settings.md)) on the reference's pages and changes them through the settings commands, so MCP and Lua
scripts can do all it does. Its presets keep complete or partial sets of preferences.

## Engine

- **`settings.set` with `values`** changes several settings at once, values by key, in one write of the settings file:
  all of them or, if one isn't a setting or doesn't fit its schema, none (`settings::Store::set`). It returns the
  settings' values and emits one `settings.changed` with those that changed. `key` and `value` change one setting as
  before; a call gives either.
- **Presets** are the user setting `preferences.presets` (in `settings.json`): by name, the values of the settings each
  one sets, all the preferences or only some. `preferences.savePreset {name, values}` saves one, replacing a preset of
  that name; every value must be a user setting (other than `preferences.presets`) and fit its schema.
  `preferences.renamePreset {name, newName}` and `preferences.removePreset {name}` do what they say, and fail for a
  preset that doesn't exist or a new name that is taken. They change the setting from its value in the file
  (`Store::modify`), so presets another GNUbase process saved meanwhile stay, and emit `settings.changed`. A preset
  applies with `settings.set {values = preset}`; saving one changes no preference.

## The dialog

- **Opening it.** The UI command `window.preferences` opens `resources/ui/preferences.rml`, modal, in the window it
  runs in (`app::AppUi::openPreferences`): Edit > Preferences... runs it. The manual names no key for it, so it has
  none. Running it while the dialog is open brings it to the front.
- **Pages.** A tree on the left, in the reference's order, with the pages that have preferences in GNUbase; a page
  whose own preferences GNUbase doesn't have yet (Event Display) only holds the pages under it. Each preference is a
  check box, a drop-down list or a number field, from its setting's schema, labelled as in the manual:
  - Editing: Display Warning before Deleting Non-Empty Tracks (`ui.removeTracksWarning`), Default Track Time Type
    (`editing.defaultTimeBase`), Use Up/Down Navigation Commands for Selecting Tracks only
    (`ui.upDownSelectsTracksOnly`). MIDI: Legato Overlap (`editing.legatoOverlap`). Tool Modifiers: each action's
    modifier keys (`ui.toolModifiers`, over the defaults that `ui::EventDisplay::defaultToolModifiers` has; the
    dialog keeps only those that differ), chosen from a list or, as in the reference (p. 1574), by holding the keys
    and clicking the action's Assign (Ctrl or Cmd, Alt, Shift); another action of the same category with those keys
    is then left without any. Tools: Show Toolbox on Right-Click (`ui.toolboxOnRightClick`), Show
    Notification when Switching Tool Mode with Key Command (`ui.toolSwitchNotification`).
  - Event Display - Folders: Show Data on Folder Tracks (`ui.folderData`), Apply Color of Folder Track Automatically
    (`editing.applyFolderColor`). Markers: Show Marker Lines (`ui.markerLines`).
  - General: Auto Save, Auto Save Interval, Maximum Number of Auto Save Files (`project.autoSave*`).
  - MIDI: Chase Events for notes, controllers, program changes and pitch bend, and Chase not limited to Part
    Boundaries (`midi.chase*`). MIDI Filter: the Record section's message types, the channels and the controllers
    (`midi.recordFilter`); the Thru section waits for MIDI thru filtering.
  - Record: Stop after Automatic Punch Out (`audio.stopAfterPunchOut`). Audio: Audio Pre-Record Seconds
    (`audio.preRecordSeconds`). MIDI: Snap MIDI Parts to Bars, MIDI Record Catch Range in ms, Retrospective Record
    Buffer Size in Events (`midi.snapPartsToBars`, `midi.recordCatchRange`, `midi.retrospectiveEvents`).
  - User Interface: Application Scaling (`ui.scale`) and the theme (`ui.theme`, the themes there are). Track & Channel
    Colors: Auto Track/Channel Color Mode (`editing.autoTrackColor`).
  - Audio, the reference's page of the audio system's preferences (p. 1622): Mute Pre-Send when Mute
    (`audio.mutePreSends`), Default Send Level (`audio.defaultSendLevel`), Auto Monitoring (`audio.autoMonitoring`).
- **Not preferences.** User settings that have a place of their own stay there: Studio Setup's devices and ports, the
  markers window's options, the tempo track's type of new tempo events and Tap Tempo Mode, the layout, presets and
  sizes the UI keeps. `tests/preferences_tests.cpp` lists them; every other user setting must be on a page, so a
  feature that adds a preference adds it to the dialog's `pages` table.
- **Apply, OK, Cancel, Defaults.** Apply sends what the pages changed, in one `settings.set` with `values`, and shows
  what the engine has then; OK does the same and closes; Cancel and Escape drop it. Defaults puts the shown page's
  preferences back to their defaults in the dialog, for Apply or OK to send. A number that isn't one keeps the dialog
  open with the message in `#preferences-message`, as does what the engine refuses, and then nothing changes.
- **Presets.** The Preference Presets list, a name field, Save, Rename and Delete. Save keeps what the dialog shows,
  applied or not, on every page, or with Save marked preferences only on the pages marked in the Save column that
  then shows in the list of pages. Choosing a preset puts its values into the dialog; the preferences it lacks keep
  what the dialog shows, and Apply or OK sends them. Rename gives the chosen preset the name in the field; Delete
  removes it.
- **Lua.** The dialog finds the themes with `gnubase.ui.themes()` (returns `themes`) and the default tool modifiers
  with `gnubase.ui.toolModifierDefaults()` (returns `modifiers`), which `app::AppUi` adds to the UI's Lua.

## Not yet

- The reference's other preferences: each comes with the feature it belongs to, and goes onto its page then.
