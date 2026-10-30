# Settings

GNUbase keeps two kinds of settings apart (#51):

- **User settings** belong to the user and apply to every project: the UI scale and theme, the audio device, and later key commands, workspaces, profiles and the preferences (#162, #163). They are files in the user's settings folder, changed with `settings.*` commands, and not part of undo.
- **Project settings** belong to one project: sample rate, tempo, tracks, channels and everything else `project.*` and the other document commands change. They live in the document, so they are saved in the `.dawproject` file and undone like any other change.

A setting is one or the other, never both: no user setting is saved in a project, and no project setting in the settings folder. Where a feature needs a user's preference as a project's starting point (say, the sample rate of new projects), the user setting is copied into the project when the project is made, and from then on the project's own value counts.

**Where.** The settings folder is per user (`files::settingsDirectory()` in `src/platform/files.h`):

| OS | Folder |
|---|---|
| Windows | `%APPDATA%\GNUbase`, the roaming application data, so settings follow the user in a domain |
| macOS | `~/Library/Application Support/GNUbase` |
| Linux | `$XDG_CONFIG_HOME/gnubase` if that is an absolute path, otherwise `~/.config/gnubase` |

- On macOS, `~/Library/Preferences` belongs to the system's preferences service: it holds the property lists that `cfprefsd` reads and caches for an application's user defaults, and Apple's guidelines put files that an application writes itself into Application Support. GNUbase writes its own JSON files, so they go there.
- Without `APPDATA`, `HOME` and the like, GNUbase asks the OS for the folder: `SHGetKnownFolderPath` on Windows, the user database (`getpwuid_r`) elsewhere. If the user has none, the settings are kept in memory only, never in a folder that other users share, such as the temporary one.
- `GNUBASE_SETTINGS_DIR` overrides the folder on every OS, and `gnubase --settings DIR` overrides both, for portable installs and tests. On Windows the app reads its command line as UTF-16, so `--settings`, `--script` and `--project` take paths with any characters. ctest sets the variable to the build's test output folder, so tests never touch the user's settings. An `Engine` constructed without a folder, as in tests, keeps its settings in memory only.
- Copying the folder to another computer takes every user setting along.

**Files.** A `settings::Store` (`src/engine/settings/store.h`) holds the settings of one file, `<name>.json`, described by a `settings::Format`: a version, and for each setting a key (`area.name`), a description, a JSON schema and a default. `settings.json` holds the user settings (`settings::userFormat()`); other kinds of settings get formats and files of their own: `quantize-presets.json` holds the user's quantize presets ([quantize.md](quantize.md)), and key commands or workspaces will follow. A feature finds the settings folder with `Engine::settingsDirectory()`.

- The file is `{"version": N, "values": {...}}` with only the values that differ from their defaults, so a default that changes in a later version reaches every user who didn't change the setting.
- Values are checked against the setting's schema when they are set and when they are read. C++ code reads them typed: `store.get(settings::g_uiScale)` is a `double`.
- Every change writes the file at once and atomically (`files::writeAtomically`): a temporary file next to it, flushed to the disk, then renamed over it. A crash or a full disk leaves the old file or the new one, never half of one; a temporary file that a crash left behind is removed the next time the file is locked. If `settings.json` is a symbolic link, as dotfile managers make them, the file it points to is replaced and the link stays. If writing fails, the setting keeps its old value and the command fails.
- Several GNUbase processes can run at once. A change takes the lock file `<name>.lock` (`files::FileLock`), reads the file again, changes only its own setting in it and writes it, so that no process loses another one's changes. Each process keeps using the values it read until it starts again. A setting that holds a collection, such as a list of presets, changes with `Store::modify` instead of `set`: the change starts from the value in the file as it is under the lock, so adding one item never drops the items another process added.
- Values the file has that this GNUbase doesn't know, or doesn't accept, stay in the file: a newer release may have added the setting or a value (a new theme, say), and the setting doesn't change the file's version. This GNUbase uses the default instead, and only a change of that very setting replaces the value.

**Loading.** The store reads its file when it is created and never discards anything without keeping it:

- A file that can't be read, isn't JSON or isn't a settings file is moved to `<name>.corrupt.json`, and the settings start from their defaults.
- A file of an older version runs through the format's **migrations**, one per version step: each one turns the values of version n into those of version n + 1 (renaming keys, converting values). The original is copied to `<name>.v<n>.json`, so an older GNUbase can still be used, and the migrated file is saved at the current version.
- A file older than the oldest migration, or a migration that fails, is moved to `<name>.v<n>.json`, and the settings start from their defaults.
- A file of a newer GNUbase stays where it is, untouched, so the newer GNUbase finds it again; this one starts from the defaults and saves nothing.
- A single value that is unknown or doesn't fit its schema falls back to its default and stays in the file (see above).
- **Read-only.** If the file belongs to a newer GNUbase, or a backup can't be made before the file is moved or rewritten, or the file can't be locked, the store is read-only until GNUbase quits: changes apply, but nothing is written, so nothing is lost.
- `settings.list` reports what happened (`missing`, `loaded`, `migrated`, `reset`, `newer`), the problems found, where the backup is and whether the store is read-only; the log has the same.

Adding a setting only needs a definition. Renaming one, or changing what its values mean, raises the format's version and adds a migration; `tests/data/settings` gets an example file of every version, and a test checks that all of them still load.

**Commands.** `settings.get` reads one setting, `settings.set` changes one, `settings.reset` puts one or all back to their defaults, and `settings.list` describes every setting with its schema, default and value, the file and how it was loaded. Changes emit `settings.changed` with the changed values. The keys are an enum in the parameter schemas, and the description of `settings.set` lists every setting, so MCP clients and [lua.md](../lua.md) show them all.

**Threads.** The engine loads the settings in its constructor; after that only the command thread reads and writes them. Audio threads never do: the store's functions assert it (`rt::assertNotRealtime`), and a feature that needs a setting on an audio thread gets it like everything else there, copied by the command thread into a render snapshot or a lock-free message.
