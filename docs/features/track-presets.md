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
  or automation, and the project's busses, tempo, time signatures and fader top, and the media files of the samples
  that a sampler and drum pads play (p. 940), so that it keeps playing them when the project moves or goes. It isn't an
  undo step. A preset name
  must be a file name, as a template's must. A MIDI track's preset keeps both its MIDI channel and its patch, where
  the reference lets a MIDI track preset include either the channel or the patch (p. 238). Choosing one when saving is
  not done yet: the preset file would need to say which one it left out, so that loading it keeps the track's own.
- **Listing them.** `track.presets` lists the presets by name with their file and the types of their tracks, top to
  bottom. A file that isn't a readable DAWproject file is left out.
- **Loading a preset** (pp. 239-243). `track.loadPreset` `{tracks, name}` gives tracks the preset's sound, in one undo
  step (the reference can't undo it; GNUbase can, as every edit): the pre section, the equalizer, the strip, the fader
  and pan, the inserts (copies of the preset's, which replace the track's own and the automation of their plugins, as
  Paste Channel Settings does), the MIDI modifiers, inserts, sends, volume, pan, program, bank and device, the MIDI
  channel (`channel.set` `midiChannel`), the instrument with its parameters, a sampler's sample, a drum machine's pads
  with their names and samples, and for MIDI and instrument tracks the color. The samples' files are extracted next to
  the preset (`Track Presets/<name> media`), as `track.import` extracts an archive's; their clips come into the pool
  (`media.added`, `clip.added`) unless it has the same clips already, so loading a preset again adds none, and the
  lanes of a drum track's patterns take the names of the pads (`pattern.changed`). Not the name, routing, sends, buttons
  or notes. The tracks must be as many as the preset's, of the same types in the same order (p. 241): the first of
  `tracks` gets the sound of the preset's top track, and so on.
- **Adding tracks from a preset** (Use Track Preset, p. 199). `track.addFromPreset` `{name, track}` adds the preset's
  tracks below `track` in its folder, or at the bottom of the track list, with their sound. Outputs go to the project's
  busses with the same names, or the main output.
- **Exporting tracks** (pp. 205-206). `track.export` `{tracks, path, copyMedia}` writes a track archive: a DAWproject
  file with the tracks, a folder with the tracks inside it, their channels, inserts, events, parts, patterns, markers,
  track versions and automation, and the project's sample rate, busses, tempo, time signatures and fader top, so that
  positions, volumes and routing mean the same there. `copyMedia` (default true, the reference's Copy Media Files)
  embeds the media files in the archive; false refers to them where they are (Reference Media Files). The archive is a
  DAWproject file rather than the reference's XML, as the project format is DAWproject. Routing to tracks that weren't
  exported doesn't go along. Tracks inside an exported folder that can't be exported yet (VCA tracks) stay behind,
  and the result lists them as `skipped`; listed themselves, they are refused.
- **The tracks of a file.** `track.importSources` `{path, range}` lists the tracks of an archive or a project file, with
  their ids in the file, names, types, folders, whether they can be imported (`importable`), and the sample rates of the
  media files their events play (`mediaSampleRates`, each once, from the lowest, the events of the versions that aren't
  active too, and a sampler's sample and a drum machine's pads' samples, which come with the instrument), for choosing
  what to import and telling whether Convert Sample Rate has anything to convert for the tracks chosen; a media file
  nothing plays is nobody's. With `range`, as `track.import` takes it, only the events that the import brings with it
  count: the engine brings the file to the project's sample rate and leaves out what starts outside the range, as the
  import does, so that the two never disagree; a sampler's samples count wherever the range is. And the file's name, sample rate, frame rate and start time, for
  comparing them with the project's (Project Settings, p. 205). Listing extracts no media files, nor does
  `track.presets`: the engine reads the file without them, and doesn't read a file in their place either, as the path
  of an embedded file that isn't extracted is its entry's name in the file, which would resolve against the working
  folder.
- **Importing tracks** (pp. 200-205). `track.import` `{path, tracks, track, events, settings, automation, destinations,
  existing, position, range}` copies tracks of an archive or project file into the project, in one undo step: `tracks`
  (default: every top-level track it can import), a listed folder with the tracks inside it, in the file's order, below
  `track` in its folder or at the bottom (Import Position for New Tracks); marker tracks go to the top level, and the
  project's marker track limit holds for new ones. Tracks that can't be imported yet stay behind, inside a folder or,
  without `tracks`, at the top level, and the result lists them as `skipped`, from top to bottom in the file; listed
  themselves, also inside a listed folder, they are refused. `events` brings events, parts, patterns, markers and track
  versions, `settings` the channel settings and inserts, `automation` the automation; all default to true. Without
  `settings` the channels start from the defaults, keeping their format, routing and instrument. Mute, solo, listen,
  monitor and record enable are off, and automation lanes read, as a new lane does: the reference doesn't import these
  states (p. 200). Routing between imported tracks stays; sends and outputs to other channels go to the tracks the
  project had before the import with the same name and type (not to other copies, which could route them in a loop),
  outputs to busses to its busses with the same name, or else the main output (p. 200). Clips come with the events, in
  the pool's top folder, and media files the project has already are shared; drum maps the project has with the same
  name and sounds too. Media files embedded in the file are extracted next to it, into `<name> media`, as `project.load`
  does, and the result lists the media files the import added as `media`. Linear positions from a file at another sample
  rate are rescaled to keep their time, as `project.set` does; faders, send levels and volume automation above the
  project's fader top come down to it, from presets too. When what is imported ends after the project's end (its length
  in the project setup), the end moves to where it ends, up to the setup's 24 hours (the note on Import Position,
  p. 204): its events, parts, markers (a cycle marker's end too) and automation points, not those of versions that
  aren't active.
- **Onto existing tracks** (Destination Track, pp. 200, 203). `destinations` `[{source, track}]` imports listed tracks
  onto the project's tracks of their type instead of as new ones. A group, effect, folder, marker or ruler track
  replaces the track, a folder with the tracks inside it: the copy takes its place, among the new tracks the result
  lists, and what played or sent into the track's channel goes into the copy's. A marker track replaced makes room for
  its copy, so a project with the most marker tracks (10) can still take a marker track's data that way (p. 201), and
  the copy of the active marker track is the active one. A track inside a replaced one can't be a destination too, in
  either order: the refusal names both tracks. New tracks below a track inside it go below the copy instead, in the
  folder around it. An audio, instrument or MIDI track gives the track what it brings, and the track keeps its id, name
  and buttons: with `events` its events and parts as `existing` says (On Import on Existing Tracks: `replace` removes
  the track's first, `add` adds them, `version`, the default, puts them into a new version of the track, as the
  manual's note on Events and Parts says; the tracks imported onto get their new versions together), and its other
  versions after the track's, versions that share an ID in the file sharing a new one in the project, and a version
  named as one of the track's getting the next free `vN` name; with `settings` its channel settings, inserts, routing
  and drum and articulation maps, the track's plugins going with their automation; with `automation` its automation in
  place of the track's, and without it, once events or settings are imported onto the track, the track loses its
  automation (the note on Automation, p. 204). The IDs of the file's versions become new ones, one for each ID of the
  file, on the new tracks too, including the implicit first version of a track without a version list, so that none
  of them falls into a version group the project has of its own; the new
  version of a track imported onto gets the ID its source's active version has, so that the tracks imported onto get
  one ID when the versions their sources played shared one in the file, and the versions that shared one there keep
  playing together here when they didn't (p. 233). What played or sent into the copy's channel, such as another
  imported track, goes into the track's. Its events, parts, versions and automation take the track's time base. A
  source goes onto one track, and not one inside a listed folder, which brings it along.
- **Where the events go** (Import Position, p. 204). `position` `absolute` (the default) puts them at the timecode
  they had in the file, so they move by the difference of the project start times (project setup's start); `relative`
  keeps them as far from the project start as from the file's; `cursor` puts them as far from the project cursor. That
  moves linear positions by samples and musical positions by the same time in the project's tempo. An import that
  would put events before the project start is refused. `range` `{start, end}` (Range) imports only the events,
  parts, markers and automation points that start from `start` up to `end`, an audio event in a part with its part.
  `start` and `end` are samples from the file's start at the project's sample rate, as a file at another rate is
  converted to the project's first.
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

The Import Options dialog (`resources/ui/track-import.rml`, pp. 202-205) gets the file and the first selected track from
`app::AppUi` and lists the file's tracks with `track.importSources` as a tree: each with a checkbox for importing it (on
for every track that can be imported), its name and type, and its destination, New Track or a track of the project of
its type. The tracks inside a folder that is imported go along with it, so their own choices are disabled; with the
folder off, they are imported on their own. Tracks that can't be imported yet show, disabled, marked as such, so the
tracks an import leaves out are known before it. Above the list are Select All, Deselect All, Expand All, Collapse All
(the folders fold), Select Matching (each imported track's destination becomes the project's first track of the same
name and type, unless another imported track has it or a folder around it, which replaces the tracks inside it: the
outermost tracks go first, so that the import isn't refused; tracks that go along with their folder keep New Track),
Reset (New Track for each) and the search field with Clear Search, which shows only the tracks whose names contain the
text. Below it are the options of `track.import`: Events and Parts, On Import on Existing Tracks (disabled without
events), Import Position, Range with its start and end as timecodes of the file and Use Locators (the timecodes of the
left and right locators, which is where Absolute Position puts what has them in the file), Channel and Inspector
Settings, Automation and Import Position for New Tracks (Bottom of Track List without a selected track). The range
fields are timecodes at the project's frame rate, which the dialog turns into `range` as samples at the project's sample
rate from the file's start. Project Settings shows the file's and the project's file name, sample rate, frame rate and
start. Copy to Active Project Folder (disabled while the project has no file) sends `media.prepareArchive` for the media
files the import added, and Convert Sample Rate (only with it, and only while one of the tracks imported, checked or
going along with a checked folder, has a media file at another sample rate than the project's in what the import
brings, whatever the file's own rate: the dialog unions those tracks' `mediaSampleRates`, p. 205, listed again for the
range while Range is on and its fields are two timecodes, once for each range) `media.conform` for those at another sample rate, through `gnubase.ui.command` so that the
pool shows how they go. The engine's refusal shows in the dialog, which stays open.

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

Of the Import Options dialog: Range cuts nothing, so events and parts that start in it come whole, and musical positions
move by the same time rather than by bars and beats when Bars+Beats is the primary time format (p. 204); tracks inside
an imported folder are all imported or none; the media files are copied and converted after the import, as undo steps of
their own. Importing chord, signature and tempo tracks, VCA tracks and link
groups; the Export Selected Tracks dialog with Reference Media Files (the engine's `copyMedia` false); routing of
exported tracks to tracks that weren't exported; frozen tracks keep their freeze file only in their own project, so
they import unfrozen; shared copies of MIDI parts import as parts of their own. Presets: the media browser with
attributes, previews, the type subfolders, renaming and removing presets, factory presets, instrument plugin presets
and extracting the sound of an instrument (p. 241), loading only the inserts or the equalizer of a preset (p. 243),
drum maps and Input Transformer settings in presets, pattern banks, quick control presets
(pp. 243-244), dragging presets onto tracks, and the inspector's Load Track Preset button.

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
  destination inside a replaced folder, naming both tracks, and puts new tracks below a track inside it below the copy,
  also in a folder around it, and gives an instrument track the settings and a new version with the part (or replaces or
  adds to its parts), keeping its buttons, in one undo step, and refuses tracks of another type and sources that aren't
  imported; a marker track replaces one of a project with 10, as the active one; merged versions keep the IDs they
  shared and get names of their own, the new versions of the tracks imported onto getting the ID their sources' active
  versions share, also when the sources don't play the versions they share, and the versions of new tracks getting IDs
  of their own, which they share with a track imported onto and a saved project keeps; without automation, importing
  events or settings onto a track resets its automation, while importing neither leaves it and records no undo step;
  musical tracks merge onto linear ones with their versions and automation; Absolute, Relative and Cursor Position and
  Range, in samples at the project's rate, place the parts, and events before the project start are refused; the
  project's end moves after what is imported, also after an audio event's end at the project's rate, an empty audio
  part and an automation point, up to 24 hours; `track.importSources` lists, for each track, the sample rates of the
  media files its events play, each once from the lowest, also those of the versions that aren't active and of a
  sampler's sample, and not one of a media file nothing plays, and with a range only those of the events that start in
  it, at the project's rate, while the sampler's stays; it refuses a range that doesn't end after its start. `tests/backup_tests.cpp`: `media.prepareArchive` with `media` copies only those.
  `tests/mixer_tests.cpp`: a lower fader top brings volume automation down, and the project saved then opens with
  GNUbase's own data. In the app: File > Export > Selected Tracks and File > Import > Tracks through the file dialogs
  and the Import Options dialog, which imports below the selected track, imports onto the track of the same name with
  Select Matching, shows the engine's refusal and the tracks that can't be imported, shows the file's and the project's
  settings, folds and searches the tracks, leaves a track inside a matched folder at New Track, keeps the upper of two
  file tracks of one name on the project's track and leaves a track that goes along with its folder at New Track,
  resets the destinations, offers Convert Sample Rate for a checked track's media files at another rate, and takes it
  back when that track goes and when no media file of the file is at another rate than the project's, counts a track
  that goes along with a checked folder though its own box isn't checked, takes it back for a range that leaves the
  media file out and offers it again with Use Locators, and imports a
  range from the locators' timecodes at Relative Position; the engine's refusals of an
  export in the project error dialog and the tracks it left out in the dialog titled Export Selected Tracks; and the
  track preset dialog's Save, Load (with only matching presets listed) and Add.
