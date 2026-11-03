# Project handling

Creating, opening and saving projects (#77, reference manual pp. 120-127, 131-135): the engine's `project.*` commands,
which MCP and Lua have as they are, and the File menu and the start screen in the UI. Backups and self-contained
projects are in [backup.md](backup.md).

## Engine

The project model keeps the project's file (`project.location`) and whether the project changed since it was last
saved, opened or started: `project.revision` has `modified`. The commands are in `projectmodel.cpp` (save, load, new)
and `projectfiles.cpp` (the rest); none of them is an undo step.

- **New.** `project.new` starts an empty project with the default busses, or with `template` a copy of that project
  template. Either way it has no file until it is saved, and the undo history is cleared. A template name must be a
  file name, as for `project.saveTemplate`, so that only templates in `Project Templates` open.
- **Open.** `project.load` opens a DAWproject file.
- **Save, Save As.** `project.save` with `path` saves the project as that file, which is then its file; without `path`
  it saves to the project's file and fails for a project that has none. The engine saves under the path as it is,
  without adding `.dawproject`, so that a script or MCP client gets the file it named; Save As in the UI adds the
  extension (see the File menu below).
- **Save New Version.** `project.saveNewVersion` saves the project next to its file with the next number, as the
  reference does: `Song-01.dawproject` after `Song.dawproject`, and after `Song-02` the number after the highest
  `Song-NN` in the folder. The project goes on with the new file; the earlier versions stay. Numbers of more than nine
  digits aren't versions. Only `.dawproject`, in any case, is taken for the extension: a file `Song v2.1` that
  `project.save` saved as named gets `Song v2.1-01.dawproject`. Files in the folder count as versions whatever case
  their name and extension are written in (`song-03.DAWPROJECT`), since the file systems of Windows and macOS don't tell
  them apart and the next version would otherwise overwrite one.
- **Revert.** `project.revert` opens the project's file again, which discards every change since it was saved.
- **Recent projects.** Every file saved (not auto saved) or opened becomes the first of the recent projects, at most 10,
  kept in `recent-projects.json` in the settings folder ([settings.md](settings.md)), so they are the user's, not the
  project's, and several GNUbase processes add to the list without losing each other's entries (`Store::modify`).
  `project.recent` lists them with whether each file is still there; `project.forgetRecent` removes one, or all.
- **Templates.** `project.saveTemplate` saves the project as `Project Templates/<name>.dawproject` in the settings
  folder; `project.templates` lists them by name, and `project.new` with `template` starts from one. Templates refer to
  their media where they are instead of copying them (the reference asks to remove the clips from the pool first). The
  project itself, its file and whether it is saved don't change.
- **Auto save.** `project.autoSave` saves a copy of a project that changed since it was last saved or auto saved into
  the folder `Auto Saves` of its project folder as `<name>-01.bak`, `<name>-02.bak` and so on, up to the user setting
  `project.autoSaveFiles` (5), after which the oldest is overwritten. A project without a file goes to `Auto Saves` in
  the settings folder, named after the project; without a settings folder it isn't auto saved. Auto saves don't copy
  media and don't save the project: it stays modified, and its file stays as it is.
  The engine's clock (`Engine::clock`, `engine/clock.h`) ticks every 30 seconds on a thread of its own, and each tick
  posts `project.autoSave` with `due`; that saves only when the user setting `project.autoSave` is on (the default)
  and `project.autoSaveInterval` minutes (15) have passed since the last save, opening or auto save, and stays out of
  `engine.commandLog` when it saves nothing. Auto saves read the time from the same clock.
- **Failed auto saves**, such as when `Auto Saves` can't be made or the disk is full, count as auto saves for the
  interval, so that they are tried again only after it instead of every 30 seconds. Each one emits
  `project.autoSaveFailed` with the file and the error, although the command fails (`Engine::broadcast`, so the event
  names no request). A due auto save that fails again, before one succeeded or the project was saved or opened, stays
  out of `engine.commandLog` and the error log (`CommandContext::failQuietly`), so that a project that can't be auto
  saved for days doesn't push the user's own commands out of the log's 256 entries. A client's own failing
  `project.autoSave` without `due` is always in the log and fails with the reason.
- **Opening an auto save.** `project.load` opens an auto save (a `.bak` in a folder `Auto Saves`, in any case on every
  platform, since Windows and macOS open other case as the same file) without a file and as modified, and it isn't a
  recent project (#484). So Save asks for a path instead of overwriting the auto save, there is nothing to revert to,
  `project.location` has no folder and no media folder (media aren't copied to `Auto Saves/Audio`), and its next auto
  saves go to the settings folder's `Auto Saves` like those of other untitled projects instead of
  `Auto Saves/Auto Saves`. Saving it as the project's file, after the user chose one, makes that its file and project
  folder again. The reference manual doesn't say what opening an auto save does; it calls auto saves copies of the
  project file (pp. 134, 1608), and going on as the project file next to `Auto Saves` would overwrite that file on Save
  with what may be an older copy that the user only wanted to look at. A `.bak` elsewhere isn't an auto save and opens
  as the project's file.
- **Project folder.** The folder of the project's file. GNUbase puts the media folder `Audio` ([backup.md](backup.md))
  and `Auto Saves` into it; the other folders of the reference (Edits, Images, Track Pictures) come with their
  features.

## UI

`ui::ProjectFiles` (`src/ui/projectfiles.h`) adds the File menu's commands as UI commands with the reference's keys
(manual pp. 1564-1565), and `app::AppUi` gives them to the main menu ([main-menu.md](main-menu.md)):

| Item | Command | Keys | What it does |
|---|---|---|---|
| New Project | `file.new` | Mod+N | opens the start screen |
| Open... | `file.open` | Mod+O | an open dialog for a DAWproject file, then `project.load` |
| Save | `file.save` | Mod+S | `project.save`, or Save As for a project without a file |
| Save As... | `file.saveAs` | Mod+Shift+S | a save dialog, then `project.save` with the path (`.dawproject` added unless the name ends with it, in any case: `Song v1.2` is saved as `Song v1.2.dawproject`, `Mix.DAWPROJECT` as it is) |
| Back up Project... | `file.backup` | | a folder dialog, then `project.backup` into it |
| Save as Template... | `file.saveTemplate` | | the Save as Template dialog (`save-template.rml`) for the name, then `project.saveTemplate` |
| Save New Version | `file.saveNewVersion` | Mod+Alt+S | `project.saveNewVersion`, or Save As for a project without a file |
| Revert | `file.revert` | | `project.revert`; disabled while the project has no file |
| Quit | `file.quit` | Mod+Q | quits GNUbase |

- **Unsaved changes.** Opening a project (Open, the start screen), creating one, reverting and quitting first ask
  `project.revision`; if the project is modified, the dialog `discard-changes.rml` asks whether to discard the changes,
  and only Discard goes on. Quitting asks the same however it is asked for: File > Quit, the main window's close button
  or Alt+F4, and the OS's quit request (the macOS app menu's and the Dock's Quit, and the signals SIGINT, Ctrl+C in the
  terminal GNUbase runs in, and SIGTERM, which SDL turns into its quit request), which `Shell::onQuitRequest` hands to
  `ProjectFiles` instead of quitting at once. A project dialog that is open then, such as the start screen, closes as
  cancelled first (`ui::closeDialog`, as its Cancel button would), so that the question can open. One question
  answers every quit request that comes while it is coming or open: SDL follows the close request of the last visible
  window with its own quit request in the same step, and the user may close the window again while the question
  waits. After Cancel the next request asks again.
- **Start screen.** `start-screen.rml`, the minimal version of the reference's hub: the templates with Create, Create
  Empty Project, the recent projects that are still there with Open, and Open Other..., which asks for a file. A double
  click on a row does what its button does. File > New Project opens it, and the app at its start when no project was
  given on the command line.
- **File dialogs** are the OS's (`Shell::showFileDialog`). UI tests switch them off (`Shell::showOsFileDialogs`) and
  answer them with `Shell::answerFileDialog`; MCP's `ui_file_dialog` and Lua's `gnubase.ui.file_dialog` list the
  dialogs that wait and answer one the same way ([ui-automation.md](ui-automation.md)). MCP's `ui_close` and Lua's
  `gnubase.ui.close` ask to close a window or to quit as the OS does (`Shell::requestClose`, `Shell::requestQuit`).
- **Errors.** When saving, opening, reverting or backing up fails, the dialog `project-error.rml` shows the error, so
  that a failed save isn't taken for done. Backups fail mostly in their job, so `job.finished` of a failed
  `project.backup` the UI asked for shows it too.
- The UI knows the project's file from `project.saved` and, after `project.reset`, from `project.location`.
  It doesn't show `project.autoSaveFailed` yet.

## Not yet

The reference's hub beyond the start screen (audio settings, user locations, sorting, project previews, web content
and its settings dialog); the default project location and folder name, so that a new project asks for its folder;
renaming and deleting templates, their descriptions and categories; Recent Projects in the File menu; asking whether to
keep audio recorded since the last save on Revert; Close and several open projects with Activate Project; the time of
the last edit in auto save names; showing a failed auto save (`project.autoSaveFailed`) in the UI; Run Setup on Create
New Project and Open Last Project on Startup.

Quit requests (#615): the unsaved changes question opens in the main window, also when the request comes from the OS
(the Dock's Quit, Close from the taskbar while the main window is minimized, a signal) and the main window is
minimized or behind other windows, so that GNUbase seems to ignore the request; the main window should come to the
front, restored, when the question opens. And a signal waits for the question like the other requests, although on a
Linux shutdown or a `kill` nobody may answer it before SIGKILL: whether a signal should rather quit after an auto save
is still to decide.

## Checks

`tests/projectfiles_tests.cpp` covers the engine commands with an engine whose settings folder is temporary: save and
`modified`, save new version numbering (also next to versions in other case and of a file saved under a name with a
dot), revert, auto save rotation, opening auto saves (also of untitled projects and of projects whose file is gone, and
under paths in other case or with `.`) and where their media and next auto saves go, the engine's own auto saves once
the interval passed and failing ones with their events and log entries (`Engine::clock().advance` moves the clock
forward and returns once its tick ran, so no test waits), recent projects across restarts, and templates; and in the
app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) Save, Save As (also of a name with a dot and of one with the
extension in upper case) and Save New Version after each through file dialogs, Open and Revert with the unsaved changes
dialog, Save asking for a file after Open of an auto save, quitting through the main window's close button and the OS's
quit request with and without unsaved changes, SDL's close and quit requests for the last window and more requests while
the question is open asking one question (the same document stays open) that Cancel and Discard answer, the start
screen's buttons and double clicks, Save as Template and Back up Project, and the error dialog for a failed save and
backup. `tests/uitools_tests.cpp` checks `ui_file_dialog` and `ui_close`, `tests/lua/ui/dialog.lua` the unsaved
changes question through `gnubase.ui.close`, and `tests/widgets_tests.cpp` `ui::closeDialog`.
`tests/clock_tests.cpp` checks the clock's ticks and `advance` on their own.
