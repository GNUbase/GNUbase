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
- **Names** are folder names on every OS: 1 to 100 characters, none of `< > : " / \ | ? *`, not ending with a dot or a
  space.

| Command | What it does |
|---|---|
| `profile.list` | The profiles, sorted, the active one and `next`, the one that becomes active at the next start |
| `profile.add` | Adds a profile with the factory settings (New) |
| `profile.duplicate` | Adds a copy of a profile's files under a new name (Duplicate) |
| `profile.rename` | Renames a profile, the active one too (Rename) |
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
  user's settings files stay as they are for the next start.
- `--delete-settings` (Delete program preferences) deletes the active profile's files from the settings folder, then
  starts with the factory settings, which it saves there again. This can't be undone.

## Not yet

Selecting several profiles to delete them at once; a profile's toolbar settings, track control presets, bus presets
and the other presets of the reference's list, once they are user settings of their own; telling held keys at start
on macOS and Linux.

## Checks

- `tests/profiles_tests.cpp`: profiles added with the factory settings, duplicated with the settings, names checked,
  activated at the next start while the others keep theirs, renamed, also the active one, which can't be deleted;
  exported and imported on another settings folder, under the file's name or another; none without a settings folder.
- `tests/profiles_ui_tests.cpp`: the Profile Manager from the Edit menu, adding, duplicating, renaming and deleting
  profiles through its name field and buttons, a taken name's error, and Activate Profile saying that the profile
  becomes active at the next start.
- `tests/safemode_tests.cpp`: the command line's options; the settings folder as it is, disabled into memory with the
  files left alone, or deleted, without the recent projects; third-party plugins deactivated, as `pluginhost.get`
  says, failing to load.
