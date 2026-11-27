# Pool

The pool lists every clip and media file of the project and manages them (#93, reference manual pp. 739-760): folders
that organize clips, which clips are used, missing files and where they went, removing unused media, importing into
the pool and conforming files to the project's sample rate. The engine commands are in
`src/engine/document/pool.cpp`; the panel is `ui::PoolPanel` (`src/ui/poolpanel.h`).

## Engine

- **`pool.get`** (query) returns the pool folders and every clip with its folder, how many audio events use it
  (`used`), its media file's path, sample rate, channel count and length, `missing` (the file isn't there) and
  `external` (outside the project folder; every file of a project that has no file yet), `rootKey`, `rootKeysDiffer`
  and `audioEvents` (see the Key column below), plus the info line's totals: `audioFiles`, `usedFiles` (media files
  an event plays), `missingFiles`, `externalFiles` and `totalSize` in bytes, plus `missingExternalFiles`, the
  external files that are missing, which Prepare Archive can't copy.
  It looks at each media file on disk once (a `std::filesystem::directory_entry` for whether it is there and its
  size), not once per clip and again per file, since the panel asks after every edit.
- **Folders.** `poolFolder.add` (name, default New Folder; parent), `poolFolder.set` (rename, or move into another
  folder but not into itself or a folder inside it) and `poolFolder.remove` (what was in it goes into its parent).
  Clips have a `folder` (0: the pool's Audio folder); `clip.set` renames a clip or moves it into a folder. Folders
  only organize the pool: files keep their names and places. They are objects with ids in the document
  (`Project::poolFolders`), so they are in undo and in the project file's GNUbase entry. A file with a folder inside
  one that isn't there loads with it at the top; of folders inside each other in a loop, the first one (by id) goes
  to the top and the others stay inside it, as do folders that are only inside the loop.
- **The Key column** (#1555, #1644, p. 417, [transpose.md](transpose.md)). `clip.set` `rootKey` gives every audio
  event that plays the clip that root key, also those in inactive track versions; the file stays as it is. `pool.get`
  gives each clip `rootKey`, the root key its audio events share (`""` when they have none or differ),
  `rootKeysDiffer` (they differ) and `audioEvents`, how many audio events play it, those in inactive track versions
  too: a clip without any has nowhere to keep a key, so `clip.set` refuses its `rootKey` (#1719).
- **`pool.removeUnused`** removes the clips that no audio event uses, then the media files no clip uses, as one undo
  step. The files stay on disk.
- **`media.cleanup`** lists the files that loads extracted and nothing uses (#1827), as the reference's Cleanup did:
  those in the project's media folder (`<name> media` next to its project file, named as its versions are: `Song
  media` for `Song.dawproject` and `Song.dawproject.`, `Mix v2.1 media` for a file `Mix v2.1`, #798) and in the track presets'
  (`Track Presets/<name> media` in the settings folder) that neither the project, nor a step of its undo history, nor
  its project file, nor a project template (`Project Templates` in the settings folder) has as a media or freeze file or a track picture.
  Such are the numbered files (`2-kick.wav`) that a load puts beside a file with other content
  ([project-files.md](project-files.md), [track-presets.md](track-presets.md)), and the files of media files removed
  from the pool once no undo step has them. A peak file goes with its audio file, and the peak
  worker's temporary files are left alone. With `files`, it removes those of them that are among these from the disk,
  which undo can't bring back; the result lists the files removed and those that couldn't be (`failed`), with `sizes`,
  the bytes of each, and `size`, their sum. A project or track preset that loads again extracts what it needs. The
  project file and the templates, which `project.saveTemplate` saves without embedding media, are read for the files
  they refer to without embedding them, and one that can't be read refuses the command, so nothing goes that it may
  need; a file that another project refers to without embedding it, which `project.save` with `embedMedia` false, auto
  saves and `track.export` with `copyMedia` false write, counts as unused. The project's own paths may be relative to
  the working folder, as `media.add` and loads take them.
- **`media.locate`** looks for missing media files in a folder and the folders inside it, by file name, matching ASCII
  letters (A to Z) in any case; other letters, such as Ä and ä, must match exactly, since folding them would need
  Unicode's case tables. It takes a file only if it has the media file's sample rate, channel count and length, so a
  same-named file of another take (a common kick.wav) isn't relinked with events pointing past its end. A length that
  another application's project.xml gave for a file missing at load is its `duration` times the sample rate, off by the
  duration's rounding, so a file within a millisecond's frames fits too (#506). Of the files that fit, it takes the one
  closest in length, and of those equally close the one with the same spelling, and the media file takes that file's
  length; what played past a shorter file's end is trimmed, as `media.relink` does. `media` limits it to some media
  files, each looked for once however often it is given. Since the folder can be large (a whole drive), it runs as a job
  (see below). A file found that goes or changes before the job relinks it is skipped and reported as `changed` (its
  `media.relink` entry has the `length` the file must have), so the other files found are still relinked. A single file
  is relinked with `media.relink` ([backup.md](backup.md)).
- **`media.removeMissing`** removes the missing media files with their clips and the audio events that play them
  (`audioEvent.remove`, so crossfades from them go too), as one undo step.
- **`media.conform`** writes each media file at another sample rate than the project's into the media folder as a 32-bit
  float WAV file at the project's rate (as `media.convert` does) and points the media file at it, the reference's
  Convert Files with New + Replace in Pool. Clip fades, warp markers and their reset origins, grid start, edited beats,
  hitpoints, their minimum separation and regions scale to the new file frames, as do the offsets, lengths, fades,
  crossfades, snap points and volume curves of the events that play it. The events' slice origins and both original and
  quantized reset timings move too, in every track version, so Reset Quantize and Reset Slices restore the same audio
  section in the new frames. Shorter files trim these saved sections and crossfade lengths to the file bounds, clamp
  saved snap points to the last frame and fit saved fade outs into the restored event's `fadeRoom`. Empty quantize
  originals stay empty so resetting still removes those split pieces. Beat numbers and clip tempo stay unchanged. Event
  fades fit the new `fadeRoom`, including stretched events whose fades exceed their source length. Shorter relinks trim
  region ends and drop empty regions, hitpoints and anchors outside the file. A grid start beyond the new end moves to
  its last frame; negative grid starts rescale without being clamped to zero. Rounding collisions drop later hitpoints
  and anchors, keeping their positions strictly increasing. A crossfade's `length` is in the frames of the event it goes
  into and its `outLength` in those of the event it comes from, so each scales with its own file. A media file given
  twice in `media` is conformed once; one given that is missing is refused. The originals stay on disk; it needs a saved
  project and is one undo step. Converting runs as a job (see below); `media.relinkResampled` then points the media
  files at the new files and scales what plays them. That command takes the new file's sample rate and length, refuses a
  file of another channel count or without sample frames and a media file given twice (before it looks at whether the
  entries apply, since a second one would rescale what the first did), and skips an entry whose media file was removed
  or no longer has `from`'s path or `fromSampleRate`'s rate, and every entry if the project is no longer at
  `projectSampleRate` or another project is open (`project`).
- **Import.** `audioFile.import` with `clipsOnly` adds the files to the pool without tracks or events, with copy,
  convert and split channels as for any import ([file-import.md](file-import.md)).
- **Jobs.** `media.locate` and `media.conform` would hold up every other command while they look through a large
  folder or convert long files, so they run as jobs (#498, `Jobs` in [architecture.md](../architecture.md)), as
  copying media does. The command checks its parameters (the folder is there; the project is saved), copies what the
  job needs (the missing media files' ids, paths, sample rates, channel counts and lengths; the files to convert)
  and returns the job at once. `media.locate`'s job walks the folder, stopping between entries when cancelled, with
  `job.progress` naming the folder it looks in (at most every 50 ms, measured in the folder's own entries), and
  keeps only the files named like a media file it looks for, so a whole drive doesn't fill the memory. Then it
  opens the candidates, with `job.progress` naming each media file, and relinks the ones found with one
  `media.relink` in a transaction ("Locate missing files"), whose `from` makes it skip media files relinked or
  removed meanwhile and whose `length` skips files that went or changed since. Its result is `relinked`, `missing`
  and `changed` (found, but changed meanwhile).
  `media.conform`'s job converts into files it creates exclusively in the media folder, with `job.progress` in
  sample frames read and the file's name, stopping between pieces when cancelled; on cancel or failure it removes
  the files it wrote, so the project and the folder's files stay as they were (the folder itself stays, since other
  jobs and recordings may write into it). Then `media.relinkResampled` runs on the command thread in a transaction
  ("Conform files"), one undo step that skips media files whose path or sample rate changed meanwhile, and all of
  them if the project's sample rate changed meanwhile, and their new files are removed. Its result is `media`
  (conformed) and `changed`. With `merge`, the transaction has that merge key, so it joins the last undo step if
  that one has the key and nothing was undone since: the project setup dialog sets a new sample rate in a
  transaction with a key of its own and then starts the job with it, so that the rate and the conformed files are
  one undo step unless something else was done in between ([project-setup.md](project-setup.md)). Being jobs,
  neither can be part of a `history.transaction`, which is one step on the command thread and can't wait for one;
  so no file is written for a transaction that then fails.
- **The project a job started for.** The final command of `media.locate`'s and `media.conform`'s jobs, and of the
  media copy jobs (`startMediaCopy`, [backup.md](backup.md)), gives `project`: `ProjectModel::openedRevision` when
  the job started, as `recording.add` does. If another project was opened meanwhile, e.g. another version of the
  same song with the same media ids and paths, `media.relink` and `media.relinkResampled` skip every entry, so
  nothing in it changes; the job reports the media files as `changed` and removes the files it wrote or copied.
  Since every edit command counts for `project.revision` even when it changes nothing, the job first asks
  `project.revision` and sends its command only if its project is still open, so a project opened meanwhile isn't
  even marked modified; the guard in the command is for one opened between the two. None of this runs on an audio
  thread; the render snapshot follows the new paths as it does for `media.relink`.

## Pool panel

- **Audio analysis.** Audio > Statistics and Audio > Spectrum Analyzer use selected clips, including unused clips, when
  the pool has focus in the lower zone. The same commands target its clips in a floating pool window, independently of
  project selection. They create no events or undo steps; see [audio-functions.md](audio-functions.md).
- **Opening it.** Media > Open Pool Window and Ctrl+P (Cmd+P on macOS; the reference's key, p. 1567) run the UI command
  `window.pool`, which shows the panel Pool in the project window's lower zone; like every panel it can open in a
  window of its own ([docking.md](docking.md)), which the command then brings to the front with the focus.
- **What it shows.** A toolbar, the message line, the info line (Audio Files, Used, Total Size, External Files,
  Missing) and a tree (`<listview id="pool">`, [widgets.md](widgets.md)) with the Audio folder, its folders and their
  clips, folders first and each by name, ASCII letters in either case alike (alpha before Bass), in the columns Media,
  Used (empty when unused), Status (Missing or External), Key (a drop-down list of the root key of the clip's audio
  events, `select[field=rootKey]`, which sets it with `clip.set`; #1555, p. 417), Info (sample rate, channels,
  length) and Path. The toolbar's View/Attributes (`#pool-attributes`, a pop-up menu `#pool-attributes-menu` as in
  the reference, pp. 417, 741) shows and hides every column but Media with its items Used, Status, Root Key, Info and
  Path; as in the reference, the Key column shows only once Root Key turns it on. Which columns show is the user
  setting `ui.poolColumns` (one boolean per column, those it leaves out at their defaults), so it stays when GNUbase
  starts again, and its changes from MCP or Lua show at once; a hidden column has the state class `.hide-key` (and so
  on) on `.pool`, which the shared style sheet hides. The Key list is empty when the events' root keys differ, where "-"
  clears them, and disabled for a clip that no audio event plays; it holds only the key it shows until it gets the
  focus, when it gets all 13, so that a pool of thousands of clips doesn't hold 13 options for each (#1644). Rows have
  the state classes `.folder` or `.clip`, and clips `.missing` and `.unused`. It follows `history.changed` and
  `project.saved`, so changes from MCP, Lua or undo show, but not a step that changed only channels' levels and sends,
  such as a fader drag's (#1881): other channel-only steps, such as a drum pad's Reset Pad, change which clips the
  sampler tracks and drum pads play, which the Used column counts (`LevelSteps`). It reads the pool only while it shows
  (`Docking::showing`, [docking.md](docking.md)): behind another tab or in a hidden zone it notes that the pool
  changed, and `AppUi`'s step reads it once the panel shows again.
- **Messages.** `#pool-message` says what the last pool command did: how many missing files Find Missing Files found,
  what Remove Unused Media and Remove Missing Files removed, how many files Conform Files converted, how many files
  Prepare Archive copied and how many media files and frozen tracks it left alone because they changed meanwhile
  (`changed`, `changedTracks`, #2001), what Clean Up Media Folders removed from the disk, or why one failed, with the
  state class `.error`, which the themes color as danger. The app hears the results of
  `pool.removeUnused`, `media.locate`, `media.removeMissing`, `media.conform` and `media.prepareArchive` (Media >
  Prepare Archive, [backup.md](backup.md)) from the toolbar, the Media menu and
  the folder dialog alike (`EngineBinding::onResult`, [ui-engine-binding.md](ui-engine-binding.md)) and opens the
  pool to show them, bringing its window to the front if it is in one of its own, so Conform Files on a project that
  was never saved says to save it first. For Find Missing Files and Conform Files the result is a job: the line
  follows its `job.progress` (e.g. "Conform Files: 42 %, slow.wav") and shows what it did from its `job.finished`,
  or that it failed or was cancelled. A job that ended before its command's result came is asked for with `job.get`.
  While a job runs, the line's row (`.pool-status`) has the state class `.running`, which shows Stop (`#pool-stop`,
  as the reference's file search dialog has, p. 751); Stop sends `job.cancel` for it. Stop is only hidden otherwise
  (`visibility`), so the row keeps its height and the list doesn't move when a job starts.
  A rename, a move or New Folder that fails says so there too, and so does a folder dialog of Find Missing Files that
  fails (`AppUi::reportInPool`).
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
- **Media > Clean Up Media Folders** (`pool.cleanup`, `ui::PoolPanel::cleanup`, #1827) opens the pool and lists the
  files `media.cleanup` finds unused: none says so in the message line; else `resources/ui/cleanup.rml` asks, as
  removing files from the disk can't be undone, with how many there are, their size in all and each file with its size
  (`#cleanup-files`), and Remove sends `media.cleanup` with them. The message line then says how many it removed and
  their size, and names the files it couldn't remove (`failed`) with the state class `.error`.

## Not yet

The Trash folder and emptying it, which deletes files from disk (p. 747), and the confirmation the reference asks before
removing (Remove from Pool is what GNUbase does); renaming files on disk with their clips; the Video folder, regions,
the Record folder and Set Pool Record Folder; the Resolve Missing Files dialog when a project with missing files opens,
Browse for File and the Search for File dialog (`media.relink` and `media.locate` do their work); the search section;
auditioning; the Musical Mode, Tempo, Sign., Algorithm, Type, Date, Origin Time, Image and Reel Name columns, and with
them their View/Attributes items; Insert into
Project and dragging clips into the project window; Minimize File, New Version, Reconstruct, pool files and libraries;
converting to other bit depths, channels and file formats (only the sample rate is conformed); a theme color for missing
clips.

## Checks

`tests/pool_tests.cpp` conforms and shortens clips with warp markers, definition edits, hitpoints and regions,
preserving stretched event fades within `fadeRoom`, undo/redo and subsequent warp edits. Resampling to fewer frames also
checks coincident anchors and hitpoints, anchors rounded onto clip endpoints and regions rounded to nothing. Markers
created from hitpoints and then warp quantized keep their rescaled creation and quantize origins; Quantize Reset
restores the rescaled originals, and Reset Warp Markers removes the generated markers while preserving manual ones.

`tests/pool_tests.cpp` also quantizes and slices events before conforming, resampled relinking and shorter same-rate
relinking, in active and inactive track versions. Both resets restore the rescaled sections within the file, including
offsets beyond a shorter file's end, saved snap points and stretched fade outs. Saved crossfades scale each side with
its own file, even when only the origin still refers to a changed source; undo/redo restore both states, and empty
quantize originals still remove their split pieces on reset.

`tests/audiofunctions_ui_tests.cpp` opens Statistics and Spectrum Analyzer through the Audio menu in the pool's zone and
the same commands in its floating window. It analyzes a whole unused clip independently of the selected project event,
without changing the document. See [audio-functions.md](audio-functions.md).

`tests/pool_tests.cpp` checks the engine: `pool.get`'s use counts, missing and external files and totals; folders
and `clip.set` as undo steps, their checks, and kept through saving and loading; `poolFolderOutside` on its own, and
a project whose folders point nowhere, loop or sit inside a loop loading with each loop broken once;
`pool.removeUnused` and `media.removeMissing` as single undo steps, also with a crossfade from a missing file's event,
which goes and comes back with undo; `media.cleanup` listing a numbered file of the project's media folder and of a
track preset's, keeping a removed media file's file while an undo step and then the project file have it, and removing
only the listed files of those given, with their peak files; keeping the files of media added by a path relative to the
working folder, a frozen track's freeze file also while only an undo step has the track, and a track preset's file
that a template refers to; and refusing, with every file kept, while the project file can't be read; `media.conform`
scaling events and fades to the new sample frames, crossfades between files at different rates in the frames each end
is counted in, snap points and event fades, and a media file
given twice conformed once; and `audioFile.import` with `clipsOnly`. The acceptance test relinks missing files: a
saved project's files are moved into another folder while it is closed, the loaded project shows them missing,
`media.locate` finds one in a subfolder under another letter case and skips one whose channel count doesn't fit, the
event plays the same section, the saved project keeps the new path, and undo brings back the old one. Another test
has `media.locate` skip a same-named file of another length but take one within a millisecond of a length that another
application's project.xml rounded, give the media file the found file's length, which undo takes back, trim the event of
a file a few frames shorter than the rounded length to the new end, where `audioEvent.set` still changes it and undo
gives back both lengths, prefer the closest length over the same spelling and the same spelling over another case among
equally close ones, match ASCII letters in any case but not Ö for ö, and look for a media file given twice once. Both
run as jobs, which the tests wait for: `transport.locate` is answered while `media.locate` on a folder of
2500 files runs, whose progress names the folder; `job.cancel` stops `media.locate` and `media.conform` with nothing
changed and no file left in the media folder; a missing file removed while `media.locate` looks for it, a media file
relinked by hand while `media.conform` converts it, and a new project sample rate set meanwhile keep the edit, and the
conformed file is removed;
`media.locate` relinks a file it found although another one it found no longer fits by the time of the relink (undo took
its media file back to another sample rate while the job looked), which it reports as `changed`; `media.relinkResampled`
refuses unknown media files, files of another channel count, missing or empty files and a media file given twice, and
skips entries whose `from`, `fromSampleRate` or `projectSampleRate` or `project` no longer fit, and a media file given
twice even when an entry wouldn't apply; `media.conform` refuses a given media file that is missing and can't be part of
a transaction, and with a merge key joins the last step only if that one has the key; `media.relink` skips entries with
`length` whose file went, has another length or doesn't fit, relinking the others, and every entry for another project;
and a project opened while `media.locate`, `media.conform` or `media.prepareArchive` runs (another version of the song,
same media ids and paths) is left as it was and unmodified, the job's files removed. Only WAV files count as files in
the media folder, not the peak files the peak worker writes next to conformed files whenever it gets to them. The
commands that must run while a job works, `job.cancel` among them, run while `test::HeldJob`
(`tests/testutils/heldjob.h`) holds the job at its first `job.progress`, so that the job can't get further first,
however the threads are scheduled (#1154): `test::whileHeld` starts the job, runs them while it is held, checks that the
job still runs and that its hold didn't lapse, and waits for it to end. It holds only the job's thread, through
`Engine::setJobProgressHook`, which runs on the job's thread outside every lock: events and commands, also those that
send events, go on meanwhile. `tests/poolpanel_tests.cpp` drives the panel in the app's UI with `ui::Automation`:
Media > Open Pool Window and its key, the rows and columns, renaming by typing, dragging a clip onto a folder, New
Folder and Remove Unused Media with its message, undo from another client, the View/Attributes menu hiding and showing
columns, the Key column only once Root Key turns it on, and `ui.poolColumns` set from another client, Import Medium
through the Import Options dialog; names sorted in any case, a clip let go outside the panel not moved by a click on a
folder's name or a drag from outside, a name typed away kept, no reading while another tab shows; Conform Files from the
Media menu on a project that was never saved opening the pool with the error, and on a saved one showing the job's
result once it is done, its progress, and Stop, which cancels it without moving the list; Find Missing Files through its
folder dialog, a failed dialog's message and its job's result; a failed job's message; Media > Clean Up Media Folders
saying that nothing is unused in an unsaved project, its dialog's text, file list and sizes, Cancel keeping the file,
Remove deleting it and saying so, and a refused `media.cleanup` (a project file it can't read) named as failed with
`.error`; and, with a panel and binding of its own, a job that ended before its command's result came (the `job.get`
path) and found files that changed meanwhile counted among those looked for, and the files Clean Up Media Folders
couldn't remove (`failed`) named with `.error`. `tests/docking_tests.cpp` checks `Docking::showing`,
`tests/enginebinding_tests.cpp` `EngineBinding::onResult`.
