# File import

Audio and MIDI files are imported into the project window (#92, reference manual pp. 357-366) with File > Import, or
by dropping them from the OS's file manager onto the event display.

## Audio files

`audioFile.import` (`src/engine/document/audiofilecommands.cpp`) imports audio files in every format the engine reads:
WAV (also RF64 and Wave64), AIFF, FLAC, Ogg Vorbis and MP3. Each file becomes a media file, a clip named after it and an
audio event at `position` (sample frames from the project start, rebased to the track's time base), on a new audio
track named after the file with its channel format (stereo beyond 8 channels). With `oneTrack` the files go one after
another on one new track; with `track`, one after another on that audio track. The options of the reference's Import
Options dialog:

- **copy** copies files from outside the project folder into its media folder (`Audio` next to the project file) as one
  job, the one `media.prepareArchive` and `media.add` with copy use (`startMediaCopy` in `src/engine/media/backup.h`):
  the import adds the files where they are, and when the copy is done the media are pointed at the copies as an undo
  step of their own.
- **convert** writes files at another sample rate than the project's, and MP3 and Ogg Vorbis files, into the media
  folder as 32-bit float WAV files at the project's sample rate (`convertSampleRate` in `resampler.h`), the format
  `media.convert` writes. FLAC stays as it is. There is no project bit depth to convert to yet.
- **splitChannels** writes each channel of a file with several channels into the media folder as a mono WAV file named
  after the file and its speaker (`take-L.wav`, `take-R.wav`; numbers beyond 8 channels), each on a new mono track. It
  converts in the same pass when both are on, and doesn't go with `track` or `oneTrack`.

Copying, converting and splitting need a saved project. Files are read, converted and split on the command thread, as
`media.convert` does; the import is one undo step ("Import audio files") made of the `media.add`, `clip.add`,
`track.add` and `audioEvent.add` a client would send, so it emits their events. If a file fails, nothing is imported
and the files the import wrote are removed. Undo leaves written files on disk, as for `media.convert`.

## MIDI files

`midiFile.import` imports standard MIDI files of type 0, 1 and 2, with tempo and markers ([midi-files.md](midi-files.md)).

## In the project window

- **File > Import > Audio File...** (`file.importAudio`) opens the OS's file dialog for audio files, several at once,
  and **MIDI File...** (`file.importMidi`) for a MIDI file (`app::AppUi`). What the user chooses goes to
  `ui::EventDisplay::importFiles` (`src/ui/fileimport.cpp`), at the project cursor.
- **Drag and drop**: files dropped on the event display (the shell's `filedrop` event) go to the track of the lane
  under the mouse if it is an audio track, else to new tracks, at the time under the mouse (dropped outside the lanes:
  at the project cursor).
- MIDI files (`.mid`, `.midi`, `.smf`) are imported at once with `midiFile.import`'s defaults. Audio files open the
  **Import Options** dialog (`resources/ui/import-options.rml`): Copy Files to Project Folder, Convert to Project
  Sample Rate, Split Channels and Import Files on One Track. The first three are disabled while the project has no
  file (`project.location`), the last for a single file and for files dropped on a track. The dialog remembers its
  choices while GNUbase runs; OK sends `audioFile.import`.

## Not yet

The preferences that skip the dialog (On Import Audio Files, Do not ask again) and the MIDI file import options
(reference manual pp. 364-365), creating a new project for a MIDI file, the name format of split files, import of
audio CDs, audio from video and REX files, and the track selection: imports go onto new tracks unless dropped on one,
until the track list (#81) selects tracks.

## Checks

`tests/audioimport_tests.cpp` imports files of every supported format onto new tracks at a position and undoes it in
one step, lays files out one after another with `oneTrack` and `track`, converts a 44.1 kHz WAV file and an MP3 file
into the media folder and leaves a 48 kHz file where it is, splits stereo and 5.1 files into mono files per speaker,
copies with one job, and rejects what it can't do, removing converted files when a later file fails.
`tests/fileimport_tests.cpp` drives the app's UI: the File > Import items, the Import Options dialog with its disabled
options, Cancel and Import Files on One Track at the project cursor, an audio file dropped on an audio track's lane,
and MIDI files of type 0 and 1 dropped below the tracks.
