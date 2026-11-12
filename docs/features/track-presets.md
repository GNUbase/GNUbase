# Track presets and track import and export

Track presets, which keep the sound of one or more tracks, track archives, which keep tracks with their events, and
importing tracks from archives and other projects (#164, reference manual pp. 199-206, 238-244). Presets and archives
are DAWproject files of their own, written and read with the project file's serialization
([project-files.md](project-files.md)), so they keep exactly what a project keeps of a track, and another application
opens an archive as a project. The commands are in `src/engine/document/trackpresets.cpp`; MCP and Lua have them as
they are.

## Engine

- **Saving a preset.** `track.savePreset` `{tracks, name}` saves the sound of tracks as `Track
  Presets/<name>.dawproject` in the settings folder: of one track, or of several as a multi-track preset (p. 241).
  Audio, instrument, MIDI, group and effect tracks have presets; other types are refused. The tracks keep the track
  list's order, whatever the order of `tracks`. The file has the tracks with their channels and inserts, without events
  or automation, and the project's busses, tempo, time signatures and fader top. It isn't an undo step. A preset name
  must be a file name, as a template's must.
- **Listing them.** `track.presets` lists the presets by name with their file and the types of their tracks, top to
  bottom. A file that isn't a readable DAWproject file is left out.
- **Loading a preset** (pp. 239-243). `track.loadPreset` `{tracks, name}` gives tracks the preset's sound, in one undo
  step (the reference can't undo it; GNUbase can, as every edit): the pre section, the equalizer, the strip, the fader
  and pan, the inserts (copies of the preset's, which replace the track's own and the automation of their plugins, as
  Paste Channel Settings does), the MIDI modifiers, inserts, sends, volume, pan, program, bank and device, the
  instrument with its parameters, and for MIDI and instrument tracks the color. Not the name, routing, sends, buttons
  or notes. The tracks must be as many as the preset's, of the same types in the same order (p. 241): the first of
  `tracks` gets the sound of the preset's top track, and so on.
- **Adding tracks from a preset** (Use Track Preset, p. 199). `track.addFromPreset` `{name, track}` adds the preset's
  tracks below `track` in its folder, or at the bottom of the track list, with their sound. Outputs go to the project's
  busses with the same names, or the main output.
- **Exporting tracks** (pp. 205-206). `track.export` `{tracks, path, copyMedia}` writes a track archive: a DAWproject
  file with the tracks, a folder with the tracks inside it, their channels, inserts, events, parts, markers, track
  versions and automation, and the project's sample rate, busses, tempo, time signatures and fader top, so that
  positions, volumes and routing mean the same there. `copyMedia` (default true, the reference's Copy Media Files)
  embeds the media files in the archive; false refers to them where they are (Reference Media Files). The archive is a
  DAWproject file rather than the reference's XML, as the project format is DAWproject. Routing to tracks that weren't
  exported doesn't go along. Tracks inside an exported folder that can't be exported yet (VCA tracks) stay behind,
  and the result lists them as `skipped`; listed themselves, they are refused.
- **The tracks of a file.** `track.importSources` `{path}` lists the tracks of an archive or a project file, with their
  ids in the file, names, types, folders and whether they can be imported (`importable`), for choosing what to import,
  and the file's name, sample rate, frame rate and start time, for comparing them with the project's (Project
  Settings, p. 205). Listing extracts no media files, nor does `track.presets`: the engine reads the file without them,
  and doesn't read a file in their place either, as the path of an embedded file that isn't extracted is its entry's
  name in the file, which would resolve against the working folder.
- **Importing tracks** (pp. 200-205). `track.import` `{path, tracks, track, events, settings, automation,
  destinations, existing, position, range}` copies tracks of an archive or project file into the project, in one undo
  step: `tracks` (default: every top-level track it can import), a listed folder with the tracks inside it, in the
  file's order, below `track` in its folder or at the bottom (Import Position for New Tracks); marker tracks go to the
  top level, and the project's marker track limit holds. Tracks that can't be imported yet stay behind, inside a
  folder or, without `tracks`, at the top level, and the result lists them as `skipped`, from top to bottom in the
  file; listed themselves, also inside a listed folder, they are refused. `events` brings events, parts, markers and
  track versions, `settings` the channel settings and inserts, `automation` the automation; all default to true.
  Without `settings` the channels start from the defaults, keeping their format, routing and instrument. Mute, solo,
  listen, monitor and record enable are off, and automation lanes read, as a new lane does: the reference doesn't
  import these states (p. 200). Routing between imported tracks stays; sends and outputs to other channels go to the
  tracks the project had before the import with the same name and type (not to other copies, which could route them
  in a loop), outputs to busses to its busses with the same name, or else the main output (p. 200). Clips come with
  the events, in the pool's top folder, and media files the project has already are shared; drum maps the project has
  with the same name and sounds too. Media files embedded in the file are extracted next to it, into `<name> media`, as
  `project.load` does, and the result lists the media files the import added as `media`. Linear positions from a file
  at another sample rate are rescaled to keep their time, as `project.set` does; faders, send levels and volume
  automation above the project's fader top come down to it, from presets too.
- **Onto existing tracks** (Destination Track, pp. 200, 203). `destinations` `[{source, track}]` imports listed tracks
  onto the project's tracks of their type instead of as new ones. A group, effect, folder, marker or ruler track
  replaces the track, a folder with the tracks inside it: the copy takes its place, among the new tracks the result
  lists, and what played or sent into the track's channel goes into the copy's. A track inside it can't be a
  destination too, in either order, and new tracks below a track inside it go below the copy instead. An audio, instrument or MIDI track gives
  the track what it brings, and the track keeps its id, name and buttons: with `events` its events and parts as
  `existing` says (On Import on Existing Tracks: `replace` removes the track's first, `add` adds them, `version`, the
  default, puts them into a new version of the track, as the manual's note on Events and Parts says), and its other
  versions after the track's; with `settings` its channel settings, inserts, routing and drum and articulation maps, the
  track's plugins going with their automation; with `automation` its automation in place of the track's. What played or
  sent into the copy's channel, such as another imported track, goes into the track's. Its events, parts, versions and
  automation take the track's time base. A source goes onto one track, and not one inside a listed folder, which brings
  it along.
- **Where the events go** (Import Position, p. 204). `position` `absolute` (the default) puts them at the timecode
  they had in the file, so they move by the difference of the project start times (project setup's start); `relative`
  keeps them as far from the project start as from the file's; `cursor` puts them as far from the project cursor. That
  moves linear positions by samples and musical positions by the same time in the project's tempo. An import that
  would put events before the project start is refused. `range` `{start, end}` (Range) imports only the events,
  parts, markers and automation points that start from `start` up to `end`, in samples of the file from its start, an
  audio event in a part with its part.
- **Media files** (Media Files, p. 205): `media.prepareArchive` `{media}` copies the media files given into the
  project's media folder, and `media.conform` `{media}` converts them to the project's sample rate, both as jobs with
  undo steps of their own, as for the pool; the dialog sends them with the import's `media`.
- Tempo, signature, arranger, transpose and chord tracks, which a project has one of, and VCA tracks with their link
  groups can't be exported or imported yet.
- `track.duplicate` copies tracks with code of its own (`copyTrack` in `projectmodel.cpp`) rather than the copier
  of `track.import`: a duplicate keeps what an import leaves out (buttons, routing to the same channels, shared MIDI
  parts, clips, freeze files, chord track following, VCA tracks), and copies within one project.

## UI

| Item | Command | Where | What it does |
|---|---|---|---|
| Tracks from Project or Track Archive... | `file.importTracks` | File > Import | the OS's open dialog, then the Import Options dialog (below) for the file |
| Selected Tracks... | `file.exportTracks` | File > Export | the OS's save dialog (`.dawproject` added to a name without an extension), then `track.export` of the selected tracks, copying the media; disabled without a selection; the engine's refusal shows in the project error dialog, the tracks it left out in the same dialog titled Export Selected Tracks (`ProjectFiles::showNotice`) |
| Add Track from Preset... | `project.addTrackFromPreset` | track list's context menu | the track preset dialog: `track.addFromPreset` below the first selected track |
| Save Track Preset... | `track.saveTrackPreset` | track list's context menu | the track preset dialog: `track.savePreset` of the selected tracks; disabled without a selection |
| Load Track Preset... | `track.loadTrackPreset` | track list's context menu | the track preset dialog: `track.loadPreset` onto the selected tracks; disabled without a selection |

The Import Options dialog (`resources/ui/track-import.rml`, pp. 202-205) gets the file and the first selected track
from `app::AppUi` and lists the file's tracks with `track.importSources` as a tree: each with a checkbox for importing
it (on for every track that can be imported), its name and type, and its destination, New Track or a track of the
project of its type. The tracks inside a folder that is imported go along with it, so their own choices are disabled;
with the folder off, they are imported on their own. Tracks that can't be imported yet show, disabled, marked as such,
so the tracks an import leaves out are known before it. Above the list are Select All, Deselect All, Expand All,
Collapse All (the folders fold), Select Matching (each imported track's destination becomes the project's track of the
same name and type), Reset (New Track for each) and the search field with Clear Search, which shows only the tracks
whose names contain the text. Below it are the options of `track.import`: Events and Parts, On Import on Existing
Tracks (disabled without events), Import Position, Range with its start and end as timecodes of the file and Use
Locators (the timecodes of the left and right locators, which is where Absolute Position puts what has them in the
file), Channel and Inspector Settings, Automation and Import Position for New Tracks (Bottom of Track List without a
selected track). Project Settings shows the file's and the project's file
name, sample rate, frame rate and start. Copy to Active Project Folder (disabled while the project has no file) sends
`media.prepareArchive` for the media files the import added, and Convert Sample Rate (only with it, for a file at
another sample rate) `media.conform` for those at another sample rate than the project's, through `gnubase.ui.command`
so that the pool shows how they go. The engine's refusal shows in the dialog, which stays open.

```
#track-import-dialog
  #import-select-all, #import-deselect-all, #import-expand-all, #import-collapse-all
  #import-select-matching, #import-reset, #import-search, #import-clear-search
  #import-tracks             .import-track rows: #import-fold-<id> (folders), #import-source-<id>,
                             #import-destination-<id>; #import-children-<id> holds a folder's tracks
  #import-events, #import-existing, #import-position, #import-range, #import-range-start, #import-range-end,
  #import-locators, #import-settings, #import-automation, #import-new-position
  #import-source-settings, #import-active-settings
  #import-copy, #import-convert
  #import-message            the engine's message when it refuses
  #import-confirm            OK
```

The track preset dialog (`resources/ui/track-preset.rml`) gets the mode and the selected tracks from `app::AppUi` and
does the rest in its Lua through the engine's commands: a name field for Save, a list of the presets for Add, and for
Load only the presets whose track types are those of the selected tracks in their order. When the engine refuses, its
message shows and the dialog stays open.

```
#track-preset-dialog
  #preset-title     Save Track Preset, Load Track Preset or Add Track from Preset
  #preset-row       #preset-name (Save) or #preset-list (Load, Add)
  #preset-message   the engine's message when it refuses
  #preset-confirm   OK
```

## Not yet

Of the Import Options dialog: Range cuts nothing, so events and parts that start in it come whole, and musical
positions move by the same time rather than by bars and beats when Bars+Beats is the primary time format (p. 204); the
project's length doesn't grow to fit what is imported; switching Automation off when importing onto a track keeps the
track's automation rather than resetting it (p. 204); tracks inside an imported folder are all imported or none; the
media files are copied and converted after the import, as undo steps of their own. Importing chord, signature and
tempo tracks, VCA tracks and link groups; the Export Selected Tracks dialog with Reference Media Files (the engine's
`copyMedia` false); routing of exported tracks to tracks that weren't exported; frozen tracks keep their freeze file
only in their own project, so they import unfrozen; shared copies of MIDI parts import as parts of their own.
Presets: the media browser with attributes, previews, the type subfolders, renaming and removing presets, factory
presets, instrument plugin presets and extracting the sound of an instrument (p. 241), loading only the inserts or the
equalizer of a preset (p. 243), drum maps and Input Transformer settings in presets, sampler track presets, pattern
banks, quick control presets (pp. 243-244), dragging presets onto tracks, and the inspector's Load Track Preset
button.

## Checks

- `tests/trackpresets_tests.cpp`: `track.savePreset` and `track.loadPreset` give tracks of the preset's types its sound
  in one undo step, not the buttons or the routing, and refuse other types, counts and orders, unknown presets, folders
  and bad names; multi-track presets; `track.presets` lists names and types; `track.addFromPreset` adds below a track or
  at the bottom; `track.export` and `track.import` round-trip a folder with its group channel, an audio track with its
  event, media file, insert, send and automation, a MIDI track playing an instrument track and an effect track, in one
  undo step, and import some tracks only, without events, settings or automation, with sends that find the project's
  track of the same name; `track.import` from a project file below a track, with marker tracks at the top level;
  automation reads after an import; listing a file's tracks extracts no media and reads no file of an entry's name in
  the working folder; `track.importSources` tells which tracks can be imported and the file's project settings; folders
  go along without the VCA tracks inside them, which `skipped` lists from top to bottom, and listed inside a listed
  folder they are refused; routing by name finds only the tracks the project had; importing onto existing tracks
  replaces an effect track and a folder in their places, with sends going to the imported effect track, refuses a
  destination inside a replaced folder and puts new tracks below a track inside it below the copy, and gives an
  instrument track the settings and a new version with the part (or replaces or adds to its parts), keeping its buttons,
  in one undo step, and refuses tracks of another type and sources that aren't imported; Absolute, Relative and Cursor
  Position and Range place the parts, and events before the project start are refused. `tests/backup_tests.cpp`:
  `media.prepareArchive` with `media` copies only those. `tests/mixer_tests.cpp`: a lower fader top brings volume
  automation down, and the project saved then opens with GNUbase's own data. In the app: File > Export > Selected Tracks
  and File > Import > Tracks through the file dialogs and the Import Options dialog, which imports below the selected
  track, imports onto the track of the same name with Select Matching, shows the engine's refusal and the tracks that
  can't be imported; the engine's refusals of an export in the project error dialog and the tracks it left out in the
  dialog titled Export Selected Tracks; and the track preset dialog's Save, Load (with only matching presets listed) and
  Add.
