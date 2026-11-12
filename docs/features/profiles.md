# Profiles and the safe start mode

Profiles, named sets of the user settings, and the safe start mode, which starts GNUbase without third-party plugins
or without the user's preferences (#163, reference manual pp. 1579–1586). Workspaces are in
[workspaces.md](workspaces.md), the settings folder in [settings.md](settings.md).

## Profiles

A profile is the files of the settings folder that belong to a user's way of working
(`settings::g_profileFiles` in `src/engine/settings/profiles.h`): `settings.json`, the user settings (preferences, key
commands and macros, global workspaces, toolbars and the other UI settings), and `quantize-presets.json`. The recent
projects, templates and recordings belong to the computer and stay out, as the reference leaves out the studio setup,
track presets and templates (p. 1579).

- **Where.** The active profile's files are in the settings folder, where GNUbase reads them; every other profile has
  its folder `profiles/<name>` with its own. `profiles.json` has the active profile's name (`Default` at first) and
  the profile that becomes active at the next start.
- **Activating** (p. 1581) takes a restart, as in the reference: `profile.activate` only notes the profile. When the
  engine starts, before it reads any setting (`settings::switchProfile`), the active profile's files go into its
  folder and the chosen profile's come into the settings folder. A profile without a file has that file's defaults.
  The switch copies both profiles' files into the folder `profile-switch` first and writes `profiles.json` only once
  all files are in place: if a step fails, the active profile's files go back and it stays active, and the switch is
  tried again at the next start. If GNUbase stops during the switch, such as in a crash, the next start first puts
  the active profile's files back from `profile-switch/active` (complete once `profile-switch/active-profile` names
  the profile, written after both copies) while `profiles.json` still has that profile active, then switches again;
  if they can't be put back, `profile-switch` stays, as it may hold their only complete copy, and the switch waits.
  While it stays, the settings may be half of each profile's: `profile.list` says `unfinished`, the Profile Manager
  opens after the start and says that what the user changes until the files go back is overwritten then, and the
  active profile can't be renamed, as the next start would take its files in `profile-switch` for another profile's.
  The safe start mode's Delete program preferences drops `profile-switch` with the pending switch. A chosen profile
  whose folder the file system takes for the active profile's (see Names) isn't switched to.
- **Deleting folders.** A folder that a switch or `profile.remove` deletes (the next profile's, `profile-switch`, a
  deleted profile's) first gets a name of its own in the settings folder, `profile-removed-<16 hex digits>`, which
  frees its name at once (#1284). Where a deleted folder stays while something such as a virus scanner still has it
  open (Windows, on file systems without POSIX deletes such as FAT, exFAT and network shares; NTFS frees the name at
  once), no folder could be made under its name meanwhile, so the next switch, which makes it again, would fail; and a
  folder with a file that can't be deleted would stay a profile. What of it can't be deleted goes at the next start
  that uses the settings folder (not one with the factory settings).
- **Names** are folder names on every OS: 1 to 100 characters, none of `< > : " / \ | ? *`, not ending with a dot or a
  space, and not a name that Windows keeps for devices (CON, CONIN$, CONOUT$, PRN, AUX, NUL, COM1 to COM9, LPT1 to
  LPT9 and COM¹ to COM³, LPT¹ to LPT³ with superscript digits, also with an extension). Since the folders aren't case-sensitive on Windows and macOS, a name that differs from another profile's
  only in case is taken; GNUbase compares ASCII letters itself and asks the file system about the others (NTFS and
  APFS fold letters such as Ä too, and APFS ignores the Unicode normalization): a name whose folder exists already, or
  that the file system takes for the active profile's, is taken. A profile can be renamed to its own name in another
  case.

| Command | What it does |
|---|---|
| `profile.list` | The profiles, sorted, the active one, `next`, the one that becomes active at the next start, and `unfinished`, whether a switch waits in `profile-switch` |
| `profile.add` | Adds a profile with the factory settings (New) |
| `profile.duplicate` | Adds a copy of a profile's files under a new name (Duplicate) |
| `profile.rename` | Renames a profile, the active one too, except while a switch is unfinished (Rename) |
| `profile.remove` | Deletes a profile that isn't active, with its files (Delete) |
| `profile.activate` | Makes a profile active at the next start (Activate Profile) |
| `profile.export`, `profile.import` | Saves a profile in a file, JSON with its name and files, and adds one from such a file under its name or another (Export, Import) |

The changes emit `profile.changed` with what `profile.list` returns. Without a settings folder (settings in memory, as
in tests) the commands fail.

**Profile Manager** (Edit > Profile Manager, `window.profiles`, `resources/ui/profiles.rml`, p. 1579): the profiles,
the active one marked; New, Duplicate and Rename take the name in the name field, Delete, Import and Export (the OS's
file dialogs, through the UI's Lua functions `ui.importProfile` and `ui.exportProfile`), and Activate Profile, after
which the dialog says which profile becomes active when GNUbase starts again.

## Safe start mode

The safe start mode (pp. 1584–1586) comes before the engine and the UI, so its dialog is the OS's message boxes
(`ui::SdlWindowSystem::ask`): first whether to deactivate all third-party plugins, then whether to use the current
preferences, disable them or delete them. `gnubase --safe-mode` opens it, and so does holding Ctrl+Shift+Alt while
GNUbase starts on Windows; other OSes can't tell held keys before a window opens. The choices are also options of
their own, which work headless too:

- `--no-third-party-plugins` deactivates every plugin that runs in a plugin host process for this run
  (`plugins::HostManager::setDeactivated`): loading one fails, so it stays bypassed where a project has it, and
  `pluginhost.get` says `deactivated`. GNUbase's own processors work as usual.
- `--factory-settings` (Disable program preferences) starts with the factory settings, kept in memory only, so the
  active profile's files stay as they are for the next start (the engine's `factorySettings`). The rest of the
  settings folder, such as the recent projects and templates, is used as usual; the profile commands fail, as without
  a settings folder, and a profile chosen with Activate Profile waits for the next start.
- `--delete-settings` (Delete program preferences) deletes the active profile's files from the settings folder, then
  starts with the factory settings, which it saves there again. A profile chosen with Activate Profile is dropped, so
  that the start has the factory settings, not that profile's. This can't be undone.

## Not yet

Selecting several profiles to delete them at once; a profile's toolbar settings, track control presets, bus presets
and the other presets of the reference's list, once they are user settings of their own; telling held keys at start
on macOS and Linux.

## Checks

- `tests/profiles_tests.cpp`: profiles added with the factory settings, duplicated with the settings, names checked,
  activated at the next start while the others keep theirs, renamed, also the active one, which can't be deleted;
  exported and imported on another settings folder, under the file's name or another; none without a settings folder;
  add, duplicate, rename and import refusing a name that differs from another profile's only in case; device names
  refused; names in another case of letters such as Ä taken where the file system folds them, also the active
  profile's; a switch that fails on the way leaving the settings and the active profile as they were, before anything
  changed, when the active profile's files can't be saved, and after files in the settings folder were replaced (a
  test hook of `settings::switchProfile` makes that step fail), then done at the next start; a switch whose files
  can't be put back keeping `profile-switch`, from which the next start puts them back before it switches; a switch
  that GNUbase didn't finish undone at the next start, then done, or kept for later when its files can't be put back,
  with `next` and `unfinished` in `profile.list` and the active profile's rename refused, and dropped by Delete
  program preferences; a switch to a folder that is the active profile's refused.
- `tests/profiles_ui_tests.cpp`: the Profile Manager from the Edit menu, adding, duplicating, renaming and deleting
  profiles through its name field and buttons, a taken name's error, and Activate Profile saying that the profile
  becomes active at the next start; Export and Import through the file dialogs, answered as `ui_file_dialog` does;
  the dialog opening after the start with its message while a switch is unfinished.
- `tests/safemode_tests.cpp`: the command line's options; the settings folder as it is, disabled into memory with the
  files left alone, or deleted, without the recent projects; the factory settings with the profile's files left alone
  and the templates still there; deleting the settings with a profile switch pending, which starts with the factory
  settings of the active profile; third-party plugins deactivated, as `pluginhost.get` says, failing to load.
