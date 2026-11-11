# Track presets and track import and export

Track presets, which keep the sound of one or more tracks, track archives, which keep tracks with their events, and
importing tracks from archives and other projects (#164, reference manual pp. 199-206, 238-244). Presets and archives
are DAWproject files of their own, written and read with the project file's serialization
([project-files.md](project-files.md)), so they keep exactly what a project keeps of a track, and another application
opens an archive as a project. The commands are in `src/engine/document/trackpresets.cpp`; MCP and Lua have them as
they are.

## Engine

- **Saving a preset.** `track.savePreset` `{tracks, name}` saves the sound of tracks as `Track Presets/<name>.dawproject`
  in the settings folder: of one track, or of several as a multi-track preset (p. 241). Audio, instrument, MIDI, group
  and effect tracks have presets; other types are refused. The tracks keep the track list's order, whatever the order
  of `tracks`. The file has the tracks with their channels and inserts,
  without events or automation, and the project's busses, tempo, time signatures and fader top. It isn't an undo step.
  A preset name must be a file name, as a template's must.
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
  ids in the file, names, types and folders, for choosing what to import. Listing extracts no media files, nor does
  `track.presets`: the engine reads the file without them.
- **Importing tracks** (pp. 200-205). `track.import` `{path, tracks, track, events, settings, automation}` copies tracks
  of an archive or project file into the project as new tracks, in one undo step: `tracks` (default: every top-level
  track it can import), a listed folder with the tracks inside it, in the file's order, below `track` in its folder or
  at the bottom (Import Position for New Tracks); marker tracks go to the top level, and the project's marker track
  limit holds. Tracks that can't be imported yet stay behind, inside a folder or, without `tracks`, at the top level,
  and the result lists them as `skipped`; listed themselves, they are refused. `events` brings events, parts, markers and track versions at their positions (Absolute Position),
  `settings` the channel settings and inserts, `automation` the automation; all default to true. Without `settings`
  the channels start from the defaults, keeping their format, routing and instrument. Mute, solo, listen, monitor and
  record enable are off, and automation lanes read, as a new lane does: the reference doesn't import these states
  (p. 200). Routing between imported tracks stays; sends and outputs to other channels go to the tracks the project
  had before the import with the same name and type (not to other copies, which could route them in a loop), outputs
  to busses to its busses with the same name, or else the main output (p. 200). Clips come with the events, in the pool's top folder, and media files the project has already are
  shared; drum maps the project has with the same name and sounds too. Media files embedded in the file are extracted
  next to it, into `<name> media`, as `project.load` does. Linear positions from a file at another sample rate are
  rescaled to keep their time, as `project.set` does; faders, send levels and volume automation above the project's
  fader top come down to it, from presets too.
- Tempo, signature, arranger, transpose and chord tracks, which a project has one of, and VCA tracks with their link
  groups can't be exported or imported yet.
- `track.duplicate` copies tracks with code of its own (`copyTrack` in `projectmodel.cpp`) rather than the copier
  of `track.import`: a duplicate keeps what an import leaves out (buttons, routing to the same channels, shared MIDI
  parts, clips, freeze files, chord track following, VCA tracks), and copies within one project.

## UI

| Item | Command | Where | What it does |
|---|---|---|---|
| Tracks from Project or Track Archive... | `file.importTracks` | File > Import | the OS's open dialog, then `track.import` of every track below the first selected track, or at the bottom; the engine's refusal, or the tracks it left out, show in the project error dialog |
| Selected Tracks... | `file.exportTracks` | File > Export | the OS's save dialog (`.dawproject` added to a name without an extension), then `track.export` of the selected tracks, copying the media; disabled without a selection; the engine's refusal, or the tracks it left out, show in the project error dialog |
| Add Track from Preset... | `project.addTrackFromPreset` | track list's context menu | the track preset dialog: `track.addFromPreset` below the first selected track |
| Save Track Preset... | `track.saveTrackPreset` | track list's context menu | the track preset dialog: `track.savePreset` of the selected tracks; disabled without a selection |
| Load Track Preset... | `track.loadTrackPreset` | track list's context menu | the track preset dialog: `track.loadPreset` onto the selected tracks; disabled without a selection |

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

The Import Options dialog (p. 202): choosing tracks, importing onto existing tracks (replacing or adding events, a new
track version, replacing group and effect tracks), Relative and Cursor Position, Range, Select Matching, the project
settings comparison, copying media to the project folder and converting their sample rate; importing chord, signature
and tempo tracks, VCA tracks and link groups; the Export Selected Tracks dialog with Reference Media Files (the
engine's `copyMedia` false); routing of exported tracks to tracks that weren't exported; frozen tracks keep their
freeze file only in their own project, so they import unfrozen; shared copies of MIDI parts import as parts of their
own. Presets: the media browser with attributes, previews, the type subfolders, renaming and removing presets, factory
presets, instrument plugin presets and extracting the sound of an instrument (p. 241), loading only the inserts or the
equalizer of a preset (p. 243), drum maps and Input Transformer settings in presets, sampler track presets, pattern banks, quick
control presets (pp. 243-244), dragging presets onto tracks, and the inspector's Load Track Preset button.

## Checks

- `tests/trackpresets_tests.cpp`: `track.savePreset` and `track.loadPreset` give tracks of the preset's types its
  sound in one undo step, not the buttons or the routing, and refuse other types, counts and orders, unknown presets,
  folders and bad names; multi-track presets; `track.presets` lists names and types; `track.addFromPreset` adds below
  a track or at the bottom; `track.export` and `track.import` round-trip a folder with its group channel, an audio
  track with its event, media file, insert, send and automation, a MIDI track playing an instrument track and an
  effect track, in one undo step, and import some tracks only, without events, settings or automation, with sends that
  find the project's track of the same name; `track.import` from a project file below a track, with marker tracks at
  the top level; automation reads after an import; listing a file's tracks extracts no media; folders go along
  without the VCA tracks inside them, which `skipped` lists; routing by name finds only the tracks the project had.
  `tests/mixer_tests.cpp`: a lower fader top brings volume automation down, and the project saved then opens with
  GNUbase's own data. In the app: File > Export > Selected Tracks and File > Import > Tracks through the file dialogs,
  with the engine's refusals and the tracks left out in the project error dialog, and the track preset dialog's Save,
  Load (with only matching presets listed) and Add.
