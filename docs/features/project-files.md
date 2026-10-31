# Project handling

Creating, opening and saving projects (#77, reference manual pp. 120-127, 131-135): the engine's `project.*` commands,
which MCP and Lua have as they are, and the File menu and the start screen in the UI. Backups and self-contained
projects are in [backup.md](backup.md).

## Engine

The project model keeps the project's file (`project.location`) and whether the project changed since it was last
saved, opened or started: `project.revision` has `modified`. The commands are in `projectmodel.cpp` (save, load, new)
and `projectfiles.cpp` (the rest); none of them is an undo step.

- **New.** `project.new` starts an empty project with the default busses, or with `template` a copy of that project
  template. Either way it has no file until it is saved, and the undo history is cleared.
- **Open.** `project.load` opens a DAWproject file.
- **Save, Save As.** `project.save` with `path` saves the project as that file, which is then its file; without `path`
  it saves to the project's file and fails for a project that has none.
- **Save New Version.** `project.saveNewVersion` saves the project next to its file with the next number, as the
  reference does: `Song-01.dawproject` after `Song.dawproject`, and after `Song-02` the number after the highest
  `Song-NN` in the folder. The project goes on with the new file; the earlier versions stay.
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
  media and don't save the project: it stays modified, and its file stays as it is. They open with `project.load`.
  The engine's ticker thread posts `project.autoSave` with `due` every 30 seconds; that saves only when the user
  setting `project.autoSave` is on (the default) and `project.autoSaveInterval` minutes (15) have passed since the last
  save, opening or auto save, and stays out of `engine.commandLog` when it saves nothing.
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
| Save As... | `file.saveAs` | Mod+Shift+S | a save dialog, then `project.save` with the path (`.dawproject` added without an extension) |
| Back up Project... | `file.backup` | | a folder dialog, then `project.backup` into it |
| Save as Template... | `file.saveTemplate` | | the Save as Template dialog (`save-template.rml`) for the name, then `project.saveTemplate` |
| Save New Version | `file.saveNewVersion` | Mod+Alt+S | `project.saveNewVersion`, or Save As for a project without a file |
| Revert | `file.revert` | | `project.revert`; disabled while the project has no file |
| Quit | `file.quit` | Mod+Q | quits GNUbase |

- **Unsaved changes.** Opening a project (Open, the start screen), creating one, reverting and quitting first ask
  `project.revision`; if the project is modified, the dialog `discard-changes.rml` asks whether to discard the changes,
  and only Discard goes on.
- **Start screen.** `start-screen.rml`, the minimal version of the reference's hub: the templates with Create, Create
  Empty Project, the recent projects that are still there with Open, and Open Other..., which asks for a file. A double
  click on a row does what its button does. File > New Project opens it, and the app at its start when no project was
  given on the command line.
- **File dialogs** are the OS's (`Shell::showFileDialog`). UI tests switch them off (`Shell::showOsFileDialogs`) and
  answer them with `Shell::answerFileDialog`; MCP's `ui_file_dialog` and Lua's `gnubase.ui.file_dialog` list the
  dialogs that wait and answer one the same way ([ui-automation.md](ui-automation.md)).
- The UI knows the project's file from `project.saved` and, after `project.reset`, from `project.location`.

## Not yet

The reference's hub beyond the start screen (audio settings, user locations, sorting, project previews, web content
and its settings dialog); the default project location and folder name, so that a new project asks for its folder;
renaming and deleting templates, their descriptions and categories; Recent Projects in the File menu; asking whether to
keep audio recorded since the last save on Revert; Close and several open projects with Activate Project; asking about
unsaved changes when the OS closes the main window; the time of the last edit in auto save names; Run Setup on Create
New Project and Open Last Project on Startup.

## Checks

`tests/projectfiles_tests.cpp` covers the engine commands with an engine whose settings folder is temporary: save and
`modified`, save new version numbering, revert, auto save rotation and due checks, recent projects across restarts, and
templates; and in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) Save, Save As and Save New Version through
file dialogs, Open and Revert with the unsaved changes dialog, the start screen's buttons and double clicks, Save as
Template and Back up Project. `tests/uitools_tests.cpp` checks `ui_file_dialog`.
