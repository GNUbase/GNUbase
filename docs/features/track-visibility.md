# Track visibility

The project window's track visibility (#166, reference manual pp. 58, 80–82, 442, 455): the Visibility tab's Track
page, which shows and hides tracks in the track list, the Track Visibility Configurations on the toolbar, and the
mixers whose channel visibility follows the tracks'.

## In the engine

`trackVisibility.get` and `trackVisibility.set` (`src/engine/document/trackvisibility.h`) keep
`Project::trackVisibility`:

- `hidden`: the tracks the project window hides, by id, sorted. Any track type; the track list also hides the tracks
  inside a hidden folder. Hidden tracks play as usual.
- `sync`: the mixer window (`window1` to `window4`) whose channel visibility follows the tracks', or empty (Sync
  Visibility of Project and Mixer, p. 82): one window at a time. The lower zone's mixer (`lower`) always follows
  (p. 81).
- `configurations` and `configuration`: the track visibility configurations, each a name and its hidden tracks, and
  the active one's name; `modified` (read only) says whether the hidden tracks differ from it, which the toolbar shows
  with an asterisk. `configurations` replaces the list (Add, Update, Rename, Delete and reorder are all that);
  `configuration` makes one active and, without `hidden`, hides its tracks.

**Following the mixers.** In a mixer that follows, a track's channel is hidden while the track is
(`syncMixers`), and `mixer.set` on such a mixer hides and shows the tracks of its track channels (`followMixer`,
called from `mixer.set` in `mixerviews.cpp`), which the other mixer that follows takes too. Busses aren't tracks: each
mixer keeps its own busses' visibility, and tracks without a channel (marker, ruler, tempo, signature tracks, folders
without a group channel) keep theirs. Turning the sync on gives the window the tracks' visibility. Every change emits
`trackVisibility.changed`, and `mixer.changed` for each mixer view that changed.

Like the mixer views, it is a setting, not an undo step, saved with the project in GNUbase's extra entry
(`trackVisibility` in `project.get`), empty for files from before. Tracks that are gone are left out of what
`trackVisibility.get` returns, and refused by `trackVisibility.set`; names of configurations are unique and not empty.

## The UI

`ui::TrackVisibility` (`src/ui/trackvisibility.h`) is one more client of the engine: it reads the tracks with
`project.get` after every undo step and the visibility with `trackVisibility.get`, follows `trackVisibility.changed`
and sends `trackVisibility.set`.

```
#track-visibility-configurations          toolbar: the active configuration, " *" while modified; "Configurations" for none
#track-visibility-configurations-menu     menuitem[visibility-configuration=add|update|rename|delete], then menuitem[configuration=<name>]
.visibility-page[page=track]              the Visibility tab's Track page; .hidden while the other one shows
  #track-visibility-sync                  Sync Visibility of Project and Mixer: On/Off, .checked while on; menu #track-visibility-sync-menu
  .track-visibility-list
    button.track-visibility-item[track][level]   .checked while the track shows
.visibility-page[page=editor]             the Editor page: the lower zone editor's visibility (midi-editors.md)
.visibility-tabs > button.visibility-tab[page]   .checked for the page that shows
```

- **Visibility tab** (pp. 80–82): the project window's Visibility panel in the left zone has the pages Track and
  Editor, with their tabs at the bottom. A click on a track's check hides it with the tracks inside it, or shows it
  with them and the folders around it; Shift-click shows only that track. The = button's menu has Sync Project and
  Mixer 1 to 4; the checked one again switches the sync off.
- **Track list**: `ui::TrackList` follows `trackVisibility.changed` too. Rows of hidden tracks get the state class
  `.filtered`, which the style sheet hides, and the event display leaves their lanes out
  (`EventDisplay::setHiddenTracks`), like folded folders' tracks; Up and Down skip them.
- **Mixer windows** (p. 442): the left zone's Visibility tab has Sync Visibility of Project and Mixer
  (`#mixer-visibility-sync`), checked while the track visibility syncs with that window; a click switches it on for
  that window or off. The lower zone's mixer simply follows.
- **Track Visibility Configurations** (pp. 58, 455): Add Configuration asks for a name
  (`resources/ui/visibility-configuration.rml`) and keeps the tracks hidden now under it, as the active one; Update
  Configuration takes the hidden tracks into the active one; Rename Configuration asks for its new name; Delete
  Configuration deletes it. A configuration chosen in the menu hides its tracks. The toolbar shows them where it has
  room for them ([docking.md](docking.md)).

## Not yet

The Zones tab (locking channels to the left and right of the fader section), the track visibility agents and the
track type filter, the Number of Visible Tracks, selecting tracks in the Visibility tab and Return, folding folders
there, reordering tracks by dragging them there, Move Configuration to Position and the key commands for the first
eight configurations, the mixer windows' own channel visibility configurations, Undo and Redo Visibility Change, and
dividing the track list, whose upper part the sync leaves alone.

## Checks

- `tests/trackvisibility_tests.cpp`: hidden tracks sorted, refused for unknown ids, not undo steps, without tracks that
  are gone, saved and loaded with the configurations; the lower zone's mixer and the synced window following both
  ways, busses and tracks without a channel keeping their state, one window at a time; configurations added, applied,
  modified, updated, renamed and deleted, names checked, gone tracks left out.
- `tests/trackvisibility_ui_tests.cpp` drives the app with `ui::Automation`: the Track page's checks hiding tracks in
  the track list and the lower zone's mixer, a channel hidden there hiding its track, Shift-click, the Track and Editor
  tabs; the = menu syncing a mixer window, whose Visibility tab shows the link, hides tracks and switches it off; the
  toolbar's configurations menu adding through the dialog, showing the asterisk, updating, applying, renaming and
  deleting.
