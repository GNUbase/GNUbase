# Colors

Track and event colors, the project's color set and colors for new tracks (#94, reference manual pp. 112–119, 214,
1621). Colors are `#rrggbb` strings; a track without one has the default color, an event or part without one follows
its track's.

## In the engine

- **Track colors** are the track's `color` (`track.add`, `track.set`; null is the default color). Folders can pass
  theirs on to the tracks inside with the user setting `editing.applyFolderColor` ([folder-tracks.md](folder-tracks.md)).
- **Event and part colors** (pp. 114–115): audio events and MIDI parts have a `color` too, null by default, which
  means they follow their track's color. `event.set` with `color` gives selected events and parts a color of their
  own, also locked ones, so they no longer follow the track; `color` null makes them follow it again (Set Event Color
  to Track). Pieces of a split event, copies and duplicates keep the color, and so do the parts Dissolve Part and
  Bounce MIDI make (a bounced part has the first part's).
- **The color set** (pp. 115–118) is the project's list of named colors that colors are picked from: `colors` in
  `project.get`, at least one `{name, color}`. New projects have the factory set of 16 colors
  (`project.factoryColors`). `project.set` `colors` replaces the set, one undo step, and `project.changed` announces
  it. `project.new` gives a new project the user setting `editing.defaultColors` (p. 118), a set as `project.set`
  takes it, or the factory set while it's empty, as it is at first; a template keeps its own.
- **Colors for new tracks** (pp. 214, 1621): the user setting `editing.autoTrackColor` chooses the color `track.add`
  gives a track when no `color` is given: `default` (the default color, the default), `previous` (the color of the
  track above it in the list: the one before it among its siblings, or the last track inside that one if it's a
  folder, or the folder it's added to), `previousPlusOne` (the color after that one in the color set, around at the
  end; the set's first color when that one has none or one outside the set), `lastApplied` (the color `event.set`
  last gave events or parts, not part of undo or the file and kept when another project is opened or a new one made)
  or `random` (a color of the set). With `editing.applyFolderColor`, a folder's color wins inside it. Add Track,
  importing audio files and the other ways to add a track go through `track.add`; the MIDI tracks Dissolve Part and
  Merge MIDI in Loop add get the same color (`newTrackColor` in `src/engine/document/projectmodel.h`).
- **DAWproject:** a track's color is its `color` attribute, an event's or part's own color its clip's; project.xml
  without GNUbase's entry gives them back. The color set and which events follow their track are GNUbase's
  (`gnubase.json`); a project from project.xml alone has the factory set.

## In the project window

- **Track list** ([track-list.md](track-list.md)): each row's `.track-color` strip, at its left edge, has the track's
  color as `background-color` and in its attribute `color` (neither for the default color, which shows nothing; the
  themes select strips with a color by the attribute, #1314).
- **Event display** ([event-display.md](event-display.md)): `<event-lanes>` fills each event and part with its own
  color, or else its track's, and with the style sheet's `color` when neither has one (`TimeSpan::color`,
  [canvas.md](canvas.md)); folder data blocks and the overview line show the same colors. Muted and selected events
  are drawn over them by the style sheets as before. Audio events' waveforms are in the same color where the theme
  tints own colors; the waveforms in an audio part are in the part's color, its own or else its track's, never in
  their events' colors. The default theme shows events tinted: a translucent body in the color, the outline and the
  waveform in a lighter shade of it, and selected events lit over it (#1312); the other themes fill events opaque.
  The tint outlines colored events also where the theme gives uncolored ones no outline, and waveforms keep the
  events' inset off their lanes' edges themselves (#1332, [canvas.md](canvas.md)).
- **Select Color for Selected Tracks or Events** (pp. 112–115): the toolbar's `#select-color` opens `#color-menu`,
  which `ui::ColorMenu` (`src/ui/colormenu.h`) fills from the color set each time it opens, a swatch and the name per
  color. With events selected in the display, a color goes to them (`event.set`) and the first item, Set Event Color
  to Track, makes them follow their track again; otherwise the color goes to the selected tracks, one undo step
  "Colorize Tracks", and the first item is Set Track Color to Default. Each item has the attribute `color`, the color
  it gives or empty.
- **Colorizing single tracks** (p. 113): an Alt-click on the left area of a track in the track list, its color strip
  or its row left of its name, or on the header of the inspector's Basics section opens the same menu at the mouse as
  the Choose Track Color pane. Its color goes to that one track, also with events selected ("Colorize Tracks"); the
  inspector's is the track it shows, the first selected one, and the Alt-click doesn't close the section. An
  inspector in a window of its own has the pane too: its window's document gets a `#color-menu` of its own (#2026).
- **The color tool** (pp. 114-115, [tools.md](tools.md)): `button[tool=color]` in the toolbar's tools, the toolbox
  and the context menu's Tools (`tool.color`, no key). Its color is the display's, not the project's
  (`EventDisplay::toolColor`, the root's attribute `tool-color`): at first the color set's first, shown in the
  button's `.tool-color` strip, which `ui::ColorMenu` colors. A click on an event or part gives it that color
  (`event.set`), or the selection if the event is in it; Alt and a click takes the event's color, its own or else
  its track's (none for the default color); Ctrl/Cmd and a click selects the event and opens the menu there as the
  Choose Event Color pane. The mouse wheel over the button steps the color through the color set, around at its
  ends; a color outside the set steps to its first.
- **Project Colors Setup** (Project menu, the UI command `project.colorsSetup`, pp. 115–119):
  `resources/ui/project-colors.rml`, the color set as a list with swatches, a name field and the color picker
  ([widgets.md](widgets.md)) for the selected color, Insert Color, Duplicate Color, Remove Color (not the last one),
  Reset Color (the factory color in its place) and Reset to Factory Settings, on the Color Set tab. The Presets tab
  (p. 117) makes a new set in its place: Number of Basic Colors (8, 16, 24 or 32), Number of Color Tints (1, 2 or 4:
  the color, its Light tint, and a Dark and a Pale one for 4) and Sort Colors by (basic color: each color with its
  tints; color tint: all colors of a tint, then the next), with the new set's swatches under them. 16 basic colors
  are the factory set; another number spreads as many around the factory set's hues, mixing neighbours, with its grey
  last, and names a color after the nearer hue (" 2" for the second of a name). A change of these settings replaces
  the set being edited, which the Color Set tab then shows. Missing Current Project Colors (p. 118), under the new
  set, shows the colors of the project's set that the set being edited lacks, each with the attribute `color` and in
  `replacement` the position of the set's color replacing it: the color it was changed to in place on the Color Set
  tab (picker, Reset Color), else the color nearest to it (a color removed, or the set replaced by a preset; Insert
  and Duplicate add colors that replace none). The list follows the picker as it changes. The mouse over a missing
  color highlights its replacement (`.replacement`). Apply sends `project.set` `colors` and gives the tracks, events
  and parts in a missing color the one replacing it, one undo step (`history.transaction`, "Change project"); Cancel
  drops the changes. The Options tab (p. 118): Save Color Set as Default (`#colors-save-default`) saves the set being
  edited, not yet applied, in `editing.defaultColors` for new projects; Reset Color Set to Default
  (`#colors-reset-default`) puts that set, or the factory set while none is saved, in place of the set being edited.
- **The MIDI editors' Event Colors** (pp. 1133-1134, 1269): the key, drum and in-place editors' toolbar button
  `#key-event-colors` opens a menu that sets the editor view's `eventColors` (`editor.set`, view state): `default`,
  the style sheets' note colors; `part`, each note in its part's color, its own or else its track's; `soundSlot`, in
  the color of the sound slot of its track's articulation map that it plays (`midi::soundSlots`,
  [articulation-maps.md](articulation-maps.md)). Notes without such a color, selected notes and those of parts that
  aren't editable keep the style sheets' look. A colored note has the color as `background-color` and in its
  attribute `color`.
- **Styling.** The swatches and the color strip get `background-color` from code because the color is user data
  (CLAUDE.md, UI conventions); `tests/stylerules_tests.cpp` lists those files, and checks the documents' Lua too.

## Not yet

The Options tab's Select Colors by Name (p. 118), colorizing track
controls and mixer channels with their track's color (p. 1621, with the mixer, #106, and the inspector, #89), the Event
Colors menu's Velocity, Pitch, Channel, Grid Match, Voice and Scale/Chords with their Setup dialogs (p. 1134), and the
list editor's event colors.

## Checks

- `tests/color_tests.cpp`: `editing.defaultColors` for `project.new`, a bad color refused and empty for the factory
  set; `event.set` colors audio events and parts, also locked ones, null follows the track again,
  split pieces keep it, undo, bad colors and the empty string refused; the factory set, `project.set` `colors` with
  its event, undo and redo, and refused sets; every `editing.autoTrackColor` mode, `lastApplied` across `project.new`,
  a given color winning, and a folder's color with `editing.applyFolderColor`; Dissolve Part and Bounce MIDI keeping
  part colors, and the tracks Dissolve Part and Merge MIDI in Loop add colored as new tracks. In the app's project
  window: the track list's strip and the event lanes' colors following track and event colors; the color menu on
  selected tracks and events, both reset items and the undo step; the Choose Track Color pane from the strip, the row
  left of the name and the inspector's Basics header, for that one track, also of an inspector in a window of its
  own; the color tool's button, its strip and wheel, and its Ctrl-click pane coloring a part; the setup dialog's
  remove, duplicate, recolor, insert, rename, reset color, apply, reset to factory settings and cancel, and its
  options: Save Color Set as Default saving the edited set, which the next new project gets, and Reset Color Set to
  Default taking it; its presets: 8 basic colors with unique names, tints in both sort orders, 128 colors, 16 in one
  tint as the factory set and 24 applied; the missing project colors of 8 basic colors, the mouse over one
  highlighting its replacement, and Apply replacing them in tracks and parts but not other colors, undone in one step;
  a color recolored in the picker listed as missing and replaced in its track by its new color.
- `tests/keyeditor_tests.cpp`: `eventColors` as view state, refused values; `tests/keyeditor_ui_tests.cpp`: the
  Event Colors menu's Part and Default, a selected note keeping its look; `tests/articulationmaps_ui_tests.cpp`: Sound
  Slot, a slot without a color, and the slot following a note's attributes.
- `tests/eventediting_tests.cpp`: the color tool coloring an event and the selection, Alt taking an event's own
  color, its track's, or none from a track with the default color, the wheel's steps and Ctrl asking for the pane.
- `tests/canvas_tests.cpp`: an event with a color of its own is filled with it, outlined by `stroke-color`, or by
  the tinted color in `stroke-width` with `own-color-tint`, also without a `stroke-color`; a waveform in the tinted
  color, `event-inset` inside its padding.
  In the default theme selected events are lit over their tint, in their events' boxes.
- `tests/eventdisplay_tests.cpp`: audio events' waveforms are in their events' colors, or else their tracks'; an audio
  part's waveforms are in the part's color, its own or else its track's; with a track's lanes shown, an event's
  waveforms on the track and on its lane are in its color.
- `tests/theme_tests.cpp`: only the default theme tints own colors and insets events; the other themes leave events
  as they were.
- `tests/uiapp_tests.cpp`: the golden image `event-colors.png` shows a colored audio event with its waveform and
  colored parts plain, selected and muted in the default theme.
- `tests/dawproject_tests.cpp`: track, event and part colors and the color set come back from the file, and the
  colors from project.xml alone.
