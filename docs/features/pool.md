# Pool

The pool lists every clip and media file of the project and manages them (#93, reference manual pp. 739-760): folders
that organize clips, which clips are used, missing files and where they went, removing unused media, importing into
the pool and conforming files to the project's sample rate. The engine commands are in
`src/engine/document/pool.cpp`; the panel is `ui::PoolPanel` (`src/ui/poolpanel.h`).

## Engine

- **`pool.get`** (query) returns the pool folders and every clip with its folder, how many audio events use it
  (`used`), its media file's path, sample rate, channel count and length, `missing` (the file isn't there) and
  `external` (outside the project folder; every file of a project that has no file yet), plus the info line's totals:
  `audioFiles`, `usedFiles` (media files an event plays), `missingFiles`, `externalFiles` and `totalSize` in bytes.
  It looks at each media file on disk once (a `std::filesystem::directory_entry` for whether it is there and its
  size), not once per clip and again per file, since the panel asks after every edit.
- **Folders.** `poolFolder.add` (name, default New Folder; parent), `poolFolder.set` (rename, or move into another
  folder but not into itself or a folder inside it) and `poolFolder.remove` (what was in it goes into its parent).
  Clips have a `folder` (0: the pool's Audio folder); `clip.set` renames a clip or moves it into a folder. Folders
  only organize the pool: files keep their names and places. They are objects with ids in the document
  (`Project::poolFolders`), so they are in undo and in the project file's GNUbase entry. A file with a folder inside
  one that isn't there loads with it at the top; of folders inside each other in a loop, the first one (by id) goes
  to the top and the others stay inside it, as do folders that are only inside the loop.
- **`pool.removeUnused`** removes the clips that no audio event uses, then the media files no clip uses, as one undo
  step. The files stay on disk.
- **`media.locate`** looks for missing media files in a folder and the folders inside it, by file name: the same
  spelling first, else one whose ASCII letters (A to Z) differ in case; other letters, such as Ä and ä, must match
  exactly, since folding them would need Unicode's case tables. It takes a file only if it has the media file's
  sample rate, channel count and length, so a same-named file of another take (a common kick.wav) isn't relinked
  with events pointing past its end, and relinks the ones found with `media.relink`, as one undo step. `media`
  limits it to some media files, each looked for once however often it is given. A single file is relinked with
  `media.relink` ([backup.md](backup.md)).
- **`media.removeMissing`** removes the missing media files with their clips and the audio events that play them
  (`audioEvent.remove`, so crossfades from them go too), as one undo step.
- **`media.conform`** writes each media file at another sample rate than the project's into the media folder as a
  32-bit float WAV file at the project's rate (as `media.convert` does) and points the media file at it, the
  reference's Convert Files with New + Replace in Pool. Clip fades, and the offsets, lengths, fades, crossfades and
  snap points of the events that play it, are in the file's sample frames, so they are scaled to the new rate and
  events play the same section. A crossfade's `length` is in the frames of the event it goes into and its
  `outLength` in those of the event it comes from, so each scales with its own file. A media file given twice in
  `media` is conformed once. The originals stay on disk; it needs a saved project and is one undo step.
- **Import.** `audioFile.import` with `clipsOnly` adds the files to the pool without tracks or events, with copy,
  convert and split channels as for any import ([file-import.md](file-import.md)).
- `media.locate` and `media.conform` read and write files on the command thread, as `media.convert` does; running
  them as jobs with progress and cancel is #498. None of this runs on an audio thread; the render snapshot follows
  the new paths as it does for `media.relink`.

## Pool panel

- **Opening it.** Media > Open Pool Window and Ctrl+P (Cmd+P on macOS; the reference's key, p. 1567) run the UI command
  `window.pool`, which shows the panel Pool in the project window's lower zone; like every panel it can open in a
  window of its own ([docking.md](docking.md)), which the command then brings to the front with the focus.
- **What it shows.** A toolbar, the message line, the info line (Audio Files, Used, Total Size, External Files,
  Missing) and a tree (`<listview id="pool">`, [widgets.md](widgets.md)) with the Audio folder, its folders and their
  clips, folders first and each by name, ASCII letters in either case alike (alpha before Bass), in the columns Media,
  Used (empty when unused), Status (Missing or External), Info (sample rate, channels, length) and Path. Rows have the
  state classes `.folder` or `.clip`, and clips `.missing` and `.unused`. It follows `history.changed` and
  `project.saved`, so changes from MCP, Lua or undo show, but reads the pool only while it shows
  (`Docking::showing`, [docking.md](docking.md)): behind another tab or in a hidden zone it notes that the pool
  changed, and `AppUi`'s step reads it once the panel shows again.
- **Messages.** `#pool-message` says what the last pool command did: how many missing files Find Missing Files found,
  what Remove Unused Media and Remove Missing Files removed, how many files Conform Files converted, or why one
  failed, with the state class `.error`, which the themes color as danger. The app hears the results of
  `pool.removeUnused`, `media.locate`, `media.removeMissing` and `media.conform` from the toolbar, the Media menu and
  the folder dialog alike (`EngineBinding::onResult`, [ui-engine-binding.md](ui-engine-binding.md)) and opens the
  pool to show them, so Conform Files on a project that was never saved says to save it first. A rename, a move or
  New Folder that fails says so there too.
- **Editing.** The names of folders and clips are fields: what is typed renames them (`poolFolder.set`, `clip.set`)
  with Enter or when the field loses the focus; a name typed away keeps the old one. Clips dragged onto a folder's
  row move into it (`clip.set`, several selected clips in one `history.transaction`). A drag lasts only while the
  button stays down on what it went down on (its `:active` state): a clip let go outside the panel isn't moved by a
  later click on a folder's name or a drag from outside onto a folder. New Folder adds a folder in the selected
  folder or the selected clip's folder.
- **Toolbar and Media menu.** Import and Media > Import Medium... (`pool.import`) open the OS's file dialog, then the
  Import Options dialog with `clipsOnly`; Find Missing Files (`pool.findMissing`) opens the OS's folder dialog and
  sends `media.locate` with the folder; from the toolbar, the dialogs belong to the window the pool shows in. Remove
  Unused Media, Remove Missing Files, Conform Files and Create Folder send `pool.removeUnused`,
  `media.removeMissing`, `media.conform` and `poolFolder.add`.

## Not yet

The Trash folder and emptying it, which deletes files from disk (p. 747), and the confirmation the reference asks
before removing (Remove from Pool is what GNUbase does); renaming files on disk with their clips; the Video folder,
regions, the Record folder and Set Pool Record Folder; the Resolve Missing Files dialog when a project with missing
files opens, Browse for File and the Search for File dialog (`media.relink` and `media.locate` do their work); the
search section; auditioning; the Musical Mode, Tempo, Sign., Key, Algorithm, Type, Date, Origin Time, Image and Reel
Name columns and choosing columns; Insert into Project and dragging clips into the project window; Minimize File,
New Version, Reconstruct, pool files and libraries; converting to other bit depths, channels and file formats (only
the sample rate is conformed); a theme color for missing clips.

## Checks

`tests/pool_tests.cpp` checks the engine: `pool.get`'s use counts, missing and external files and totals; folders
and `clip.set` as undo steps, their checks, and kept through saving and loading; `poolFolderOutside` on its own, and
a project whose folders point nowhere, loop or sit inside a loop loading with each loop broken once;
`pool.removeUnused` and `media.removeMissing` as single undo steps, also with a crossfade from a missing file's event,
which goes and comes back with undo; `media.conform` scaling events and fades to the new sample frames, crossfades
between files at different rates in the frames each end is counted in, snap points and event fades, and a media file
given twice conformed once; and `audioFile.import` with `clipsOnly`. The acceptance test relinks missing files: a
saved project's files are moved into another folder while it is closed, the loaded project shows them missing,
`media.locate` finds one in a subfolder under another letter case and skips one whose channel count doesn't fit, the
event plays the same section, the saved project keeps the new path, and undo brings back the old one. Another test
has `media.locate` skip a same-named file of another length, prefer the same spelling over another case, match ASCII
letters in any case but not Ö for ö, and look for a media file given twice once. `tests/poolpanel_tests.cpp` drives
the panel in the app's UI with `ui::Automation`: Media > Open Pool Window and its key, the rows and columns, renaming
by typing, dragging a clip onto a folder, New Folder and Remove Unused Media with its message, undo from another
client, Import Medium through the Import Options dialog; names sorted in any case, a clip let go outside the panel not
moved by a click on a folder's name or a drag from outside, a name typed away kept, no reading while another tab
shows; and Conform Files from the Media menu on a project that was never saved opening the pool with the error.
`tests/docking_tests.cpp` checks `Docking::showing`, `tests/enginebinding_tests.cpp` `EngineBinding::onResult`.
