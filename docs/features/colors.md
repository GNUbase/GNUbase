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
  to Track). Pieces of a split event, copies and duplicates keep the color.
- **The color set** (pp. 115–118) is the project's list of named colors that colors are picked from: `colors` in
  `project.get`, at least one `{name, color}`. New projects have the factory set of 16 colors
  (`project.factoryColors`). `project.set` `colors` replaces the set, one undo step, and `project.changed` announces
  it.
- **Colors for new tracks** (pp. 214, 1621): the user setting `editing.autoTrackColor` chooses the color `track.add`
  gives a track when no `color` is given: `default` (the default color, the default), `previous` (the color of the
  track above it in the list: the one before it among its siblings, or the last track inside that one if it's a
  folder, or the folder it's added to), `previousPlusOne` (the color after that one in the color set, around at the
  end; the set's first color when that one has none or one outside the set), `lastApplied` (the color `event.set`
  last gave events or parts in this session, not part of undo or the file) or `random` (a color of the set). With
  `editing.applyFolderColor`, a folder's color wins inside it. Add Track, importing audio files and every other way to
  add a track go through `track.add`.
- **DAWproject:** a track's color is its `color` attribute, an event's or part's own color its clip's; project.xml
  without GNUbase's entry gives them back. The color set and which events follow their track are GNUbase's
  (`gnubase.json`); a project from project.xml alone has the factory set.

## In the project window

- **Track list** ([track-list.md](track-list.md)): each row's `.track-color` strip, at its left edge, has the track's
  color as `background-color` and in its attribute `color` (empty for the default color, which shows nothing).
- **Event display** ([event-display.md](event-display.md)): `<event-lanes>` fills each event and part with its own
  color, or else its track's, and with the style sheet's `color` when neither has one (`TimeSpan::color`,
  [canvas.md](canvas.md)); folder data blocks and the overview line show the same colors. Muted and selected events
  are drawn over them by the style sheets as before. Audio events' waveforms are in the same color where the theme
  tints own colors. The default theme shows events tinted: a translucent body in the color, the outline and the
  waveform in a lighter shade of it, and selected events lit over it (#1312); the other themes fill events opaque.
  The tint outlines colored events also where the theme gives uncolored ones no outline, and waveforms keep the
  events' inset off their lanes' edges themselves (#1332, [canvas.md](canvas.md)).
- **Select Color for Selected Tracks or Events** (pp. 112–115): the toolbar's `#select-color` opens `#color-menu`,
  which `ui::ColorMenu` (`src/ui/colormenu.h`) fills from the color set each time it opens, a swatch and the name per
  color. With events selected in the display, a color goes to them (`event.set`) and the first item, Set Event Color
  to Track, makes them follow their track again; otherwise the color goes to the selected tracks, one undo step
  "Colorize Tracks", and the first item is Set Track Color to Default. Each item has the attribute `color`, the color
  it gives or empty.
- **Project Colors Setup** (Project menu, the UI command `project.colorsSetup`, pp. 115–119):
  `resources/ui/project-colors.rml`, the color set as a list with swatches, a name field and the color picker
  ([widgets.md](widgets.md)) for the selected color, Insert Color, Duplicate Color, Remove Color (not the last one),
  Reset Color (the factory color in its place) and Reset to Factory Settings. Apply sends `project.set` `colors`;
  Cancel drops the changes.
- **Styling.** The swatches and the color strip get `background-color` from code because the color is user data
  (CLAUDE.md, UI conventions); `tests/stylerules_tests.cpp` lists those files, and checks the documents' Lua too.

## Not yet

The color tool and its wheel and Alt-click (p. 114), Alt-click on the track list or the inspector's header for one
track's color (p. 113), selecting colors by name, the color set's presets (8 to 32 colors with tints) and its options
(saving a set as the default for new projects), colorizing track controls and mixer channels with their track's
color (p. 1621, with the mixer, #106, and the inspector, #89), and the MIDI editors' event colors menu (p. 1133).

## Checks

- `tests/color_tests.cpp`: `event.set` colors audio events and parts, also locked ones, null follows the track again,
  split pieces keep it, undo, bad colors refused; the factory set, `project.set` `colors` with its event, undo and
  redo, and refused sets; every `editing.autoTrackColor` mode, a given color winning, and a folder's color with
  `editing.applyFolderColor`. In the app's project window: the track list's strip and the event lanes' colors
  following track and event colors; the color menu on selected tracks and events, both reset items and the undo step;
  the setup dialog's remove, duplicate, recolor, insert, apply, reset to factory settings and cancel.
- `tests/canvas_tests.cpp`: an event with a color of its own is filled with it, outlined by `stroke-color`, or by
  the tinted color in `stroke-width` with `own-color-tint`, also without a `stroke-color`; a waveform in the tinted
  color, `event-inset` inside its padding.
- `tests/theme_tests.cpp`: only the default theme tints own colors and insets events; the other themes leave events
  as they were.
- `tests/uiapp_tests.cpp`: the golden image `event-colors.png` shows a colored audio event with its waveform and
  colored parts plain, selected and muted in the default theme.
- `tests/dawproject_tests.cpp`: track, event and part colors and the color set come back from the file, and the
  colors from project.xml alone.
