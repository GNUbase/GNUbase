# Ruler tracks

Ruler tracks show a ruler in a time format of their own, independent of the main ruler and of the rulers and position
displays in other windows (#447, reference manual pp. 157-159). Adding them, with the add track dialog's Ruler page, is
in [add-track.md](add-track.md).

## Engine

- A ruler track (`type` `ruler`) has a `timeFormat`: `barsBeats` (the default), `seconds`, `timecode`, `samples` or
  `userFrames`, the event display ruler's formats ([event-display.md](event-display.md)).
- **Time offset** (p. 159): `timeOffset` is a position in the track's time base, ticks on a musical track (a new
  ruler track's) and samples on a linear one, or `"cursor"`, the project cursor's position (Set Time Offset to Project
  Cursor Position); `timeOffsetOn` switches it on. Both default to off at the project start. `track.add` and
  `track.set` take them, only for ruler tracks; setting the offset doesn't switch it on. A new time base keeps the
  offset's time, as for the track's markers ([time-base.md](time-base.md)).
- GNUbase's entry keeps them with the track; files from before have the offset off at the project start.

## UI

- **The ruler on the lane** (`ui::RulerLanes`, `src/ui/rulertracks.h`): a `<ruler class="ruler-lane">` over each ruler
  track's lane in the event display, with the attributes `track` and `format`, following the project and the view;
  `.hidden` while the lane isn't shown, e.g. in a folded folder.
- **What it counts from** (`ui::rulerTrackTime`). With the offset off, the ruler follows the project setup's start
  time and bar offset ([project-setup.md](project-setup.md)), as the main ruler does. With it on, it counts from the
  offset instead: 0:00:00.000, 00:00:00:00 and sample 0 at the offset, negative times before it, and bar 1 at the bar
  the offset is in. Ruler tracks ignore the project setup's time format: they always show their own (p. 159).
- **Track controls** (p. 159) in the track list, the user setting `ui.trackControls` for `ruler` (all four by
  default, in the track controls settings dialog too, [track-list.md](track-list.md)):
  `select[control=timeFormat]` (Time Format), `button[control=timeOffsetOn]` (Time Offset On/Off, `.checked` when on),
  `textdisplay[control=timeOffset]` (Time Offset: the offset as a position in the track's time format; a double click
  types one, which the track list reads in that format, and text that isn't a position changes nothing) and
  `button[control=setTimeOffset]` (Set Time Offset to Project Cursor Position). They fit the ruler tracks' default
  controls area of 140 dp (`ui.trackWidths`) on two lines.
- **Add Track** (p. 158): the Ruler page's Time Offset field takes a position in the chosen time format; one typed adds
  the tracks with the offset on. The dialog's Lua reads it with `gnubase.ui.parseTime{format, text}`, which returns
  `seconds` and `ticks` of a position typed as the project's position displays show it, or nothing.

## Not yet

A time offset within a bar for bars and beats (the ruler counts from the bar the offset is in), and ruler tracks among
the editors' global tracks.

## Tests

`tests/document_tests.cpp`: the offset and its switch in `track.add` and `track.set`, `"cursor"`, undo, a new time base
and what they refuse; `tests/dawproject_tests.cpp` saves and loads a ruler track with an offset.
`tests/rulertracks_tests.cpp`: what a ruler track counts from with the offset off and on, in each format and on a
linear track; the rulers on the ruler tracks' lanes on the fake window system; and in the app's UI a golden screenshot
(`tests/golden/ui/ruler-tracks.png`) of a ruler track in each format, without and with an offset, and the track list's
controls changing the time format, the offset and its switch. `tests/addtrack_tests.cpp`: the Ruler page's Time Offset.
