# Preferences

The preferences dialog (#162, reference manual pp. 1593-1625) shows the user settings that are preferences
([settings.md](settings.md)) on the reference's pages and changes them through the settings commands, so MCP and Lua
scripts can do all it does. Its presets keep complete or partial sets of preferences. Which settings are preferences
is part of their definitions (`settings::Definition::preference`, `settings.list` reports it), so the engine keeps
presets to preferences and clients can tell them from the settings that have a place of their own.

## Engine

- **`settings.set` with `values`** changes several settings at once, values by key, in one write of the settings file:
  all of them or, if one isn't a setting or doesn't fit its schema, none (`settings::Store::set`). It returns the
  settings' values and emits one `settings.changed` with those that changed. `key` and `value` change one setting as
  before; a call gives either.
- **Presets** are the user setting `preferences.presets` (in `settings.json`): by name, the values of the settings each
  one sets, all the preferences or only some. `preferences.savePreset {name, values}` saves one, replacing a preset of
  that name; every value must be a preference that fits its schema, so a preset never holds a setting that has a place
  of its own, such as `audio.device` or `ui.layout`. `preferences.renamePreset {name, newName}` and
  `preferences.removePreset {name}` do what they say, and fail for a preset that doesn't exist or a new name that is
  taken. They change the setting from its value in the file (`Store::modify`), so presets another GNUbase process saved
  meanwhile stay, and emit `settings.changed`. A preset applies with `settings.set {values = preset}`; saving one
  changes no preference.

## The dialog

- **Opening it.** The UI command `window.preferences` opens `resources/ui/preferences.rml`, modal, in the window it
  runs in (`app::AppUi::openPreferences`): Edit > Preferences... runs it. The manual names no key for it, so it has
  none. Running it while the dialog is open brings it to the front.
- **Pages.** A tree on the left, in the reference's order, with the pages that have preferences in GNUbase; a page
  whose own preferences GNUbase doesn't have yet (Event Display) only holds the pages under it. Each preference is a
  check box, a drop-down list or a number field, from its setting's schema, labelled as in the manual:
  - Editing: Display Warning before Deleting Non-Empty Tracks (`ui.removeTracksWarning`), Default Track Time Type
    (`editing.defaultTimeBase`), Use Up/Down Navigation Commands for Selecting Tracks only
    (`ui.upDownSelectsTracksOnly`). Audio: Treat Muted Audio Events like Deleted (`editing.treatMutedAudioLikeDeleted`,
    [comping.md](comping.md)), On Import Audio Files (`editing.onImportAudio`) and its settings, Copy Files to Project
    Folder, Convert to Project Sample Rate, Split Channels and Name Format (`editing.importAudio*`,
    [file-import.md](file-import.md)). MIDI: Legato Overlap (`editing.legatoOverlap`), Legato Mode: Between Selected
    Notes Only (`editing.legatoBetweenSelected`). Tool Modifiers: each action's modifier keys (`ui.toolModifiers`, over
    the defaults that `ui::EventDisplay::defaultToolModifiers` has; the dialog keeps only those that differ), chosen
    from a list or, as in the reference (p. 1574), by holding the keys and clicking the action's Assign (Ctrl or Cmd,
    Alt, Shift). When another action of the same tool has those keys already, the dialog asks first (p. 1602): Replace
    gives them to the action and leaves the other without any, Cancel keeps what both had. Tools: Show Toolbox on
    Right-Click (`ui.toolboxOnRightClick`), Show Notification when Switching Tool Mode with Key Command
    (`ui.toolSwitchNotification`). Zoom: Zoom Tool Standard Mode: Horizontal Zooming Only (`ui.zoomToolHorizontalOnly`,
    [tools.md](tools.md)).
  - Event Display - Folders: Show Data on Folder Tracks (`ui.folderData`), Apply Color of Folder Track Automatically
    (`editing.applyFolderColor`). Markers: Show Marker Lines (`ui.markerLines`).
  - General: Auto Save, Auto Save Interval, Maximum Number of Auto Save Files (`project.autoSave*`).
  - MIDI: Chase Events for notes, controllers, program changes and pitch bend, and Chase not limited to Part
    Boundaries (`midi.chase*`). MIDI Filter: the Record section's message types, the channels and the controllers
    (`midi.recordFilter`); the Thru section waits for MIDI thru filtering. MIDI File: the Import Options
    (`midi.import*`, [file-import.md](file-import.md)); its Export Options wait for the export preferences.
  - Record: Stop after Automatic Punch Out (`audio.stopAfterPunchOut`). Audio: Audio Pre-Record Seconds
    (`audio.preRecordSeconds`). MIDI: Snap MIDI Parts to Bars, MIDI Record Catch Range in ms, Retrospective Record
    Buffer Size in Events (`midi.snapPartsToBars`, `midi.recordCatchRange`, `midi.retrospectiveEvents`).
  - User Interface: Application Scaling (`ui.scale`) and the theme (`ui.theme`), which lists the themes there are with
    the names they carry ([themes.md](themes.md)). Track & Channel Colors: Auto Track/Channel Color Mode
    (`editing.autoTrackColor`).
  - Audio, the reference's page of the audio system's preferences (p. 1622): Mute Pre-Send when Mute
    (`audio.mutePreSends`), Default Send Level (`audio.defaultSendLevel`), Auto Monitoring (`audio.autoMonitoring`).
- **Not preferences.** User settings that have a place of their own stay there, and their definitions say so by
  leaving the preference flag off: Studio Setup's devices and ports, the markers window's options, the tempo track's
  type of new tempo events and Tap Tempo Mode, the layout, presets and sizes the UI keeps. Every setting with the flag
  must be on a page, and every setting on a page must have it (`tests/preferences_tests.cpp` checks both), so a feature
  that adds a preference flags it and adds it to the dialog's `pages` table.
- **Apply, OK, Cancel, Defaults.** Apply sends what the pages changed, in one `settings.set` with `values`, and shows
  what the engine has then; OK does the same and closes; Cancel and Escape drop it. Defaults puts the shown page's
  preferences back to their defaults in the dialog, for Apply or OK to send. A number that isn't one keeps the dialog
  open with the message in `#preferences-message`, as does what the engine refuses, and then nothing changes.
  - **Only what changed.** What a page shows counts as a change only when it differs from what the engine has, in the
    form the dialog shows it: numbers as the text their fields show, so one a field rounds doesn't come back rounded,
    and the MIDI filter's lists as sets, since the dialog fills them in its own order. A page that shows part of a
    setting keeps the rest of it: the Tool Modifiers page sends `ui.toolModifiers` with the overrides of the actions it
    doesn't list, which a newer GNUbase, MCP or Lua can have set.
- **While it is open.** The dialog follows `settings.changed`, so a setting a script or an MCP client changes shows up
  in it at once; what the user changed in a page and hasn't applied stays.
- **Presets.** The Preference Presets list, a name field, Save, Rename and Delete. Save keeps what the dialog shows,
  applied or not, on every page, or with Save marked preferences only on the pages marked in the Save column that
  then shows in the list of pages. Choosing a preset puts its values into the dialog; the preferences it lacks keep
  what the dialog shows, and Apply or OK sends them. Rename gives the chosen preset the name in the field; Delete
  removes it. What the engine refuses, such as Delete with no preset chosen, shows its message and changes nothing,
  not even the name field.
- **Lua.** The dialog finds the themes with `gnubase.ui.themes()` (returns `themes`, each with its `id` and the `name`
  to show for it) and the default tool modifiers with `gnubase.ui.toolModifierDefaults()` (returns `modifiers`), which
  `app::AppUi` adds to the UI's Lua.

## Not yet

- The reference's other preferences: each comes with the feature it belongs to, and goes onto its page then.
