# File import

Audio and MIDI files are imported into the project window (#92, reference manual pp. 357-366) with File > Import, or
by dropping them from the OS's file manager onto the event display.

## Audio files

`audioFile.import` (`src/engine/document/audiofilecommands.cpp`) imports audio files in every format the engine reads:
WAV (also RF64 and Wave64), AIFF, FLAC, Ogg Vorbis and MP3. Each file becomes a media file, a clip named after it and an
audio event at `position` (sample frames from the project start, rebased to the track's time base), each on an audio
track of its own: the first on `track` if given, the others on new audio tracks named after them with their channel
format (stereo beyond 8 channels), all at `position`. With `oneTrack` the files go one after another on one track:
`track`, or a new one. Files that are split (below) aren't placed by either: `track` takes the first file that isn't.
Before #480, `track` alone put every file on that track; `oneTrack` with `track` does that now, so the command's
version is 2.

MP3 and Ogg Vorbis files are always converted, whatever the options say (reference manual p. 360): each is written as
a 32-bit float WAV file at its own sample rate into the media folder, and the media file, clip and event use the WAV
file, while the clip and track keep the name of the file imported. FLAC files stay as they are. A project without a
file has no media folder yet, so its MP3 and Ogg Vorbis files go where its recordings go: `Recordings` in the settings
folder, or the temporary folder without one (`unsavedMediaFolder` in `src/engine/media/backup.h`,
[recording.md](recording.md)), and are referenced from there. Saving the project doesn't move them, as it doesn't move
recordings; once it is saved, `media.prepareArchive` copies them into the project, like every media file from outside
its folder. They stay in `Recordings` when the project is closed without saving, as recordings do. That keeps one
place and one rule for the files an unsaved project makes. Converting only once the project is saved would play the
compressed file until then and make saving a long conversion; a temporary folder of their own that saving moves into
the project would be a second such place, with recordings left out.

The options of the reference's Import Options dialog:

- **copy** copies files from outside the project folder into its media folder (`Audio` next to the project file) as one
  job, the one `media.prepareArchive` and `media.add` with copy use (`startMediaCopy` in `src/engine/media/backup.h`):
  the import adds the files where they are, and when the copy is done the media are pointed at the copies as an undo
  step of their own.
- **convert** writes files at another sample rate than the project's into the media folder as 32-bit float WAV files
  at the project's sample rate (`convertSampleRate` in `resampler.h`), the format `media.convert` writes; MP3 and Ogg
  Vorbis files then go to the project's sample rate as well. There is no project bit depth to convert to yet.
- **splitChannels** writes each channel of a file with several channels into the media folder as a mono WAV file named
  after the file and its speaker (numbers beyond 8 channels), each on a new mono track at `position`. **nameFormat**
  (the dialog's Name Format, #477) says what goes between the two, for other applications that expect their own
  names: `dash` (`take-L.wav`, `take-R.wav`, the default), `underscore` (`take_L.wav`), `dot` (`take.L.wav`) or
  `space` (`take L.wav`). It converts in the same pass when both are on. Mono files aren't split and go where `track`
  and `oneTrack` say.

With **clipsOnly** the files only go into the pool, as clips without tracks or events (the pool's Import Medium,
[pool.md](pool.md)). `copy`, `convert` and `splitChannels` need a saved project. Files are read, converted and split on the command thread, as
`media.convert` does; the import is one undo step ("Import audio files") made of the `media.add`, `clip.add`,
`track.add` and `audioEvent.add` a client would send, so it emits their events. If a file fails, nothing is imported
and the files the import wrote are removed. Undo leaves written files on disk, as for `media.convert`.

## MIDI files

`midiFile.import` imports standard MIDI files of type 0, 1 and 2, with tempo and markers ([midi-files.md](midi-files.md)).

## In the project window

- **File > Import > Audio File...** (`file.importAudio`) opens the OS's file dialog for audio files, several at once,
  and **MIDI File...** (`file.importMidi`) for a MIDI file (`app::AppUi`). What the user chooses goes to
  `ui::EventDisplay::importToSelection` (`src/ui/fileimport.cpp`), at the project cursor, with the track list's
  selection (#480).
- **The selected track** (reference manual p. 360): audio files go onto the selected track, and onto new tracks only
  when no track is selected. The manual describes one file and one selected track; for the other cases GNUbase
  follows the pool's Insert into Project (pp. 745-746), which inserts clips onto one track or each onto a different
  track:
  - With several tracks selected, the first of them, top to bottom, takes the files (p. 746). An audio track inside a
    folded folder stays selected and takes them too.
  - With several files, the selected track takes the first one and the others go onto new tracks, all at the project
    cursor; with Import Files on One Track (p. 358) they all go onto the selected track, one after another. That
    several files go onto several tracks unless the option is on is GNUbase's reading of the option, not something
    the manual says.
  - A selected track that isn't an audio track (MIDI, instrument, folder, group, effect) takes no audio: the files go
    onto new tracks, as without a selected track (p. 746: the clip goes onto the selected track "or on a new audio
    track").
  - Split Channels puts every channel of a file with several onto a new mono track of its own (p. 358), whatever is
    selected; mono files still go onto the selected track.
  - New tracks go to the end of the project, as Add Track's do; below the selected track comes with #448.
- **Drag and drop**: files dropped on the event display (the shell's `filedrop` event) go to the track of the lane
  under the mouse if it is an audio track, on a take lane of a track whose lanes show on that lane
  ([comping.md](comping.md)), else to new tracks, at the time under the mouse (dropped outside the lanes:
  at the project cursor). Agents and UI tests drop files with `ui_drop` (`ui::Automation::drop`).
- MIDI files (`.mid`, `.midi`, `.smf`) are imported at once with `midiFile.import`'s defaults. Audio files open the
  **Import Options** dialog (`resources/ui/import-options.rml`): Copy Files to Project Folder, Convert to Project
  Sample Rate, Split Channels and Import Files on One Track. The first three are disabled while the project has no
  file (`project.location`), the last for a single file and for files dropped on a track, which all go on it. Split
  Channels has the Name Format pop-up (`nameFormat`), disabled with it. OK sends `audioFile.import`.

## Import preferences

The user settings of the Editing > Audio preferences (reference manual pp. 357-359, #477) decide whether the dialog
opens:

- `editing.onImportAudio`: `openOptionsDialog` (the default) opens the Import Options dialog for every audio import
  in the project window, by File > Import > Audio File, by dropping files and by the pool's Import Medium;
  `useSettings` imports at once with the options in the settings below.
- `editing.importAudioCopy`, `editing.importAudioConvert`, `editing.importAudioSplitChannels`,
  `editing.importAudioNameFormat` and `editing.importAudioOneTrack`: the options. Copy, Convert and Split Channels
  apply once the project has a file, as in the dialog, and Import Files on One Track where the dialog would offer it.
  The manual's preferences have no Import Files on One Track; GNUbase keeps it with the others, so that Do not ask
  again (below) doesn't drop that choice.
- The dialog starts from these settings, and OK keeps the options it could choose in them (the disabled ones stay as
  they were). The dialog and Use Settings so share one set of options, which also survives a restart; before #477 the
  dialog only remembered its choices while GNUbase ran.
- **Do not ask again** in the dialog sets `editing.onImportAudio` to `useSettings` after keeping the dialog's
  choices, so the next imports run without it. `settings.set` or `settings.reset` of `editing.onImportAudio` brings
  it back, as does Open Options Dialog on the preferences' Editing > Audio page (below).
- The preferences dialog's **Editing > Audio** page ([preferences.md](preferences.md)) shows On Import Audio Files
  and, under On Import Audio Files Settings (p. 359), Copy Files to Project Folder, Convert to Project Sample Rate,
  Split Channels and Name Format, labelled and listed as in the Import Options dialog. Import Files on One Track
  isn't there, as it isn't among the manual's preferences; the dialog alone changes it.

The event display reads the settings with `settings.list` for each import, so a change through MCP or Lua applies to
the next import.

## Not yet

The MIDI file import options (reference manual pp. 364-365), creating a new project for a MIDI file, import of audio
CDs, audio from video and REX files. New tracks below the selected track (#448). Converting as a job: files are
converted on the command thread, which waits for a long MP3 file (as for `media.convert`). Removing the files an unsaved
project made (recordings and converted imports) when it is closed without saving.

## Checks

`tests/audioimport_tests.cpp` imports files of every supported format (WAV, RF64, Wave64, AIFF, FLAC, Ogg Vorbis and
MP3) onto new tracks at a position and undoes it in
one step, lays files out one after another with `oneTrack` and `track`, converts MP3 and Ogg Vorbis files without
`convert` to WAV files with the signal they decode to (into the media folder, also with `copy` and `clipsOnly`, and in
an unsaved project into `Recordings`, where they stay after saving until `media.prepareArchive` copies them, and
removed again when a later file fails) and leaves FLAC files as they are, converts a
44.1 kHz WAV file and an MP3 file
into the media folder and leaves a 48 kHz file where it is, splits stereo and 5.1 files into mono files per speaker,
names split files as each `nameFormat` says, copies with one job, puts the first file on a given track and the others on new tracks (one undo step), places only
the files that aren't split with `track`, and rejects what it can't do, removing converted files when a later file
fails.
`tests/fileimport_tests.cpp` drives the app's UI through the automation API: the File > Import items, the Import Options dialog with its disabled
options, Cancel and Import Files on One Track at the project cursor; File > Import > Audio File with the file dialog
answered as the user would, onto the selected musical audio track (the first file, all of them with Import Files on
One Track, the first of several selected tracks, one inside a folded folder), onto new tracks with no track or a MIDI
track first in the selection, and with Split Channels a stereo file onto new tracks and a mono file onto the selected
track, each one undo step; audio files dropped on an audio track's lane, one and two of them, and MIDI files of type 0
and 1 dropped below the tracks. For the import preferences: Use Settings imports without the dialog with the stored
options (on one track in an unsaved project; split with a name format and copied in a saved one), the dialog starts
from the settings and keeps its choices in them, Do not ask again skips the dialog next time and after a restart
until the setting is reset, and each Name Format chosen in the dialog names the split files. The preferences'
Editing > Audio page shows the settings, Use Settings imports with what it sets (stereo split as its Name Format
says, a mono file keeping its original path with Copy off), and its Open Options Dialog brings the dialog back,
starting from the same settings.
