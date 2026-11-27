# Transport bar and transport panel

The transport controls (#90, reference manual pp. 295–316): the project window's transport bar, the transport panel
and the time display window. The code is `ui::TransportControls` (`src/ui/transport.h`); `app::AppUi` fills the
controls into the transport bar, one of the project window's strips ([docking.md](docking.md)), and into the windows.
The transport itself is the engine's ([playback.md](playback.md)): every control is an engine command or reads the
engine's state, so MCP and Lua reach all of it.

## Controls

`resources/ui/transport.rml` has the controls, in the transport bar's sections (p. 306); it is not a document of its
own, but what `AppUi` puts into the transport bar and into the transport panel's document. Its elements say what they
are by attributes, which `TransportControls` follows:

| Section | Controls | What they run or show |
|---|---|---|
| MIDI Auto Quantize | Automatic MIDI Record Quantize (p. 353), hidden by default | `quantize.set` `autoQuantize`, checked while `quantize.get` says it is on; it follows `quantize.changed` and a new project ([quantize.md](quantize.md)) |
| Locators | Go to Left/Right Locator Position, the left and right locator | `transport.locate` to `leftLocator` and `rightLocator`; `transport.set` `left`, `right` |
| Locator Range Duration | Locators to Selection, the range's length | `edit.locatorsToSelection` (the event display's selection); `transport.set` `right` |
| Punch Points | Punch In, Punch Out | `transport.toggle` `punchIn`, `punchOut` |
| Transport Controls | Go to Previous Marker/Zero, Go to Next Marker/Project End, Rewind, Forward, Cycle, Stop, Start, Record | `locate.previousMarker`, `locate.nextMarker`, `wind.rewind`, `wind.forward`, `transport.toggle` `cycle`, `transport.stop`, `transport.play`, `transport.toggle` `record` |
| Time Displays | primary and secondary time format menus and displays, Exchange Time Formats | the project cursor; `transport.locate`; `time.exchangeFormats` |
| Pre-roll & Post-roll | Activate Pre-roll, Pre-roll Amount, Activate Post-roll, Post-roll Amount (p. 318), hidden by default | `transport.toggle` `preRoll`, `postRoll`; `transport.set` `preRollAmount`, `postRollAmount` |
| Tempo & Time Signature | Activate Tempo Track, tempo, Tap Tempo, Tap Tempo Mode, time signature | `project.set` `tempoTrackActive`, `tempo`, `signature`; `tempo.set`; `tempo.tap`, `transport.tapTempoMode` ([tempo-track.md](tempo-track.md)) |
| Click | Activate Metronome Click | `transport.toggle` `metronome` |

The sections keep the width of their controls. The transport bar shows the sections it has room for, as the toolbar
does (`fit="room"`, [docking.md](docking.md)): in a project window under about 1240 dp wide the last sections go, the
last of the order first, and come back in a wider one. The Left and Right Divider (pp. 307, 310, 65), hidden by
default, keep the sections outside them shown: those before the left divider and those after the right one, which
stands before Click; the sections between them go instead. The setup pane shows, hides and moves them as sections, and
they show as a line. The transport panel takes the width of its sections and has no dividers; one sized too narrow for
them cuts off the last sections at its right edge instead of drawing sections over each other. The locator buttons L,
R and Sel take their letters' width rather than a text button's 64 dp, so all sections fit 1280 dp, a 1920-pixel
display at 150 % (#1450).

- **Buttons** have a `command` with `parameters` (JSON), which run as a key would (`KeyCommands::run`): engine
  commands through the UI's engine client, the UI's own commands in `AppUi`. Buttons with `state` are checked, the
  state class `.checked`, while the transport's value is true; they follow `transport.changed`, whoever changed it.
  Record also has `waiting="recordWaiting"`: the state class `.waiting` while a recording waits for the end of the
  pre-roll or the punch in (`transport.get` `recordWaiting`), which the flat GNUbase theme shows as Record filled with
  the record color at a quarter opacity and the dark, light and high-contrast themes as pressed; a click then cancels
  the wait (#2039). The Transport menu's Pre-roll & Post-roll submenu has Use Pre-roll and Use Post-roll
  (p. 303), `transport.toggle` with `preRoll` and `postRoll`, checked while they are on (#583).
  Buttons with `hold` run their command when the mouse button goes down and end it, as a key held, when it goes up,
  the mouse leaves the button or the window loses the keyboard focus. `ui::TransportControls` checks every control
  with a `command`, the time display's menu items too, before it attaches (#446): parameters that aren't a JSON object
  throw `std::invalid_argument`, and so does a command or parameters that `KeyCommands::check` refuses, which `AppUi`
  passes in. A window whose controls are refused closes again with an error logged, as one whose document doesn't
  load.
- **Winding** (pp. 297, 300): Rewind and Forward, and Fast Rewind and Fast Forward on the keys, move the cursor by a
  step at once (1 second, 4 for the fast ones), as a click or a key pressed and released does, and while their button
  or key stays down wind on after 0.3 seconds at four steps a second until it goes up (`ui::Winding`, which sends
  `transport.locate` `by` each frame). A key ends winding when it goes up, whatever modifiers are still down, and so
  does the window losing the keyboard focus ([key-commands.md](key-commands.md)). Each key or button winds as its own:
  one that goes up leaves the others that are still down winding, the one pressed last first, and the focus lost also
  ends the buttons held. Run from a menu item or a script (`KeyCommands::run`), they step once and don't wind on.
- **The mouse wheel** changes a display's value: up for later, by a beat in Bars+Beats, a frame in Timecode and fps
  (User), a second in Seconds and Samples, never before the project start or past the largest project position. On
  the locator range it moves the right locator, not before the left one; on the pre-roll and post-roll amounts it
  changes the amount, not below 0. A display being edited ignores the wheel, a scroll sideways moves nothing, and a
  turn moves at most a billion steps (#457). A touchpad or a high-resolution wheel sends small fractions of a notch:
  they add up, for each display and direction, and make a step when they reach a whole one; a pause of a second
  forgets them. The next turn goes on from the position the last one set, also when the transport.changed of an older
  turn reaches the display first: the displays leave what an answer or event of a request older than their newest
  change says (#569). Nudge Cursor and Increase and Decrease Window Size do the same. A wheel gesture on the tempo
  field is set when its controls go within the pause, e.g. the panel closing (#566).
- **Displays** (`time`) show the project cursor, a locator, the locator range or the pre-roll and post-roll amounts
  (`preRollAmount`, `postRollAmount`: lengths measured from the left and the right locator) in the time format of
  their slot (`format`: `primary`, `secondary`, `timeDisplay`). While playing they read the playback position once a
  frame, as the event display does. Each has its format's state class, such as `.format-barsBeats`, and in the transport bar and the
  panel the cursor's displays take the width of their format's widest text from it (`base.rcss`, in em of the display
  font, whose digits are all as wide, with room for RmlUi cutting the font size to whole pixels and FreeType rounding
  the advances), so the controls beside them stay in place while the transport runs, locates or scrubs; a format change
  may change it (#1335). Their content is one line of their font high, at least 16 dp, so a theme may give the primary
  display a larger font, as the default theme does (#1342). The widths hold every position from bar -999 to 9999 in any
  signature, every position under 100 hours in seconds, timecode and user frames, and 11 digits of samples (under 48
  hours at 384 kHz): the longest project from the latest start time (#1379). The locators and the signature keep the
  widths they have, since they don't move with the cursor; the tempo field follows the cursor in tempo track mode, also
  while playing, and keeps its width as every `valuefield` does (80 dp). The time display window fills its window. They
  are `<textdisplay>`s ([widgets.md](widgets.md)): a double click edits one, and Enter takes what was typed in its
  format (`ui::parseTime`): "3.2" is bar 3, beat 2, "1:30" a minute and a half, "5:12" five seconds and twelve frames;
  what is left out counts from the start. A minus before the bar number types a position before bar 1, which a negative
  bar offset shows: "-3.1.1.0" with a bar offset of -4 ([project-setup.md](project-setup.md)); lengths take none. Text
  that isn't a position changes nothing, and neither does leaving a display without typing. A position or length that
  ends past the largest project position (`time::g_maxPosition`, 2^48 samples) goes there, as `transport.locate` `by`
  stops there, and timecode hours past 2147483647 aren't a position (#457). The event display, info line and marker
  window use the same bound when they turn typed or dragged times into positions. Typing the position a display shows
  locates to it: onto the display's tick when the cursor was between ticks, and back to it during playback.
- **The locator range** shows the length from the left to the right locator: in bars and beats counted from zero,
  "1.1.0.0" for a bar and a beat, in the other formats as a time (`ui::formatLength`). A length typed moves the right
  locator.
- **Tempo and signature** show the tempo and the first time signature. The tempo is Current Tempo
  (`ui::currentTempo`, reference manual p. 1446): the fixed tempo, set with `project.set`, or in tempo track mode the
  tempo at the project cursor, also while playing, which changes the tempo event at or before the cursor (`tempo.set`;
  the first one in a project without tempo changes). The signature is set with `project.set`, and later signature
  changes stay. Each change is one undo step. The tempo field is a `<valuefield>`: a drag on it, or wheel turns
  without a pause of 0.4 seconds, set the tempo once, at the gesture's end, so that the gesture is one undo step;
  meanwhile the field shows where the gesture took it. It shows three decimals: editing it without typing leaves a tempo
  with more, such as 130.0001 BPM from a MIDI file, as it is. A signature is typed as "3/4"; the engine refuses
  denominators other than 1, 2, 4, 8 and 16.
- **Time formats.** The primary one is the project's time format (Bars+Beats by default), as the project setup dialog's
  Time Format (reference manual p. 317). Picking one in the primary menu (`project.setTimeFormat`) or exchanging formats
  changes the display without adding an undo step; an exchange changes both displays together. A format changed in
  Project Setup (`project.set {timeFormat}`) remains an undo step. The displays follow the project's format after undo
  and loading a project. The others are the user setting `ui.timeFormats` (`secondary`, `timeDisplay`: `barsBeats`,
  `seconds`, `timecode`, `samples` or `userFrames`; Timecode and Bars+Beats by default), kept after a restart; a
  `primary` in it, from before #537, is ignored. So every transport control shows the same. The menus' items have `slot`
  and `format`, and the item of the slot's format is checked.

## Sections and setup panes

The transport bar's sections are the docking system's set of sections `transport` ([docking.md](docking.md)):
MIDI Auto Quantize (hidden by default), Left Divider, Locators, Locator Range Duration, Punch Points, Transport
Controls, Time Displays, Tempo & Time Signature, Right Divider and Click; the panel's are the same without the dividers.
A right click on the transport bar opens its setup pane (the reference's Set up Transport Bar, p. 306), a toggle
button per section that shows or hides it, Show All, Reset All and presets ([docking.md](docking.md)); on Tap Tempo
it opens the Tap Tempo Mode menu instead. The transport panel is a docking window of its own, `transportPanel`, with
the same pane for its own sections (Set up Transport Panel, p. 295). Both are kept in `ui.layout`, under `windows`,
`project` and `transportPanel`, `sections`, `transport`.

## Windows

- **Transport panel** (p. 295): F2 (`window.transportPanel`) shows and hides a window of its own,
  `resources/ui/transport-panel.rml`, with the same controls. Closing the window closes the panel. A panel opened
  without saved bounds takes the width of its shown sections in the middle of the primary monitor, as far as it has room
  (#1424). It then follows that width wherever it is, as far as its monitor has room, until the user sizes the window:
  when the controls show the project and whenever it changes, since the locator and range displays grow with their text,
  such as a time format's or a far locator's (#1474), and when the setup pane shows or hides sections (#1527). Its
  window's entry in `ui.layout` keeps the width of the last fit as `fittedWidth`, saved with the bounds in one save; a
  panel opened with bounds of that width follows again, one the user sized keeps its bounds as they were, also a width
  set right before the window's close button, since a close request saves the bounds before the window goes. A panel
  that opens with bounds follows only once its controls show the project, when the engine answered their first queries
  (`ui::TransportControls::onShown`), so it isn't fitted to their defaults first; a new one is fitted to the defaults at
  once and then once more, again centred, for what they show (#1612), also where the window system moves windows later,
  such as SDL's on X11, and the panel is still where it was or already where the first fit asked for (#1701); one whose
  first fit found nothing to fit follows its sections from the width it opened with and is centred where it opened, and
  only this opening counts, not where a panel before it was (#1765, #1848). Reset All in the panel's setup pane makes a
  panel the user sized, or one with bounds from before #1527, follow its sections again. `ui::TransportPanelFit`
  (`src/ui/transportpanelfit.h`) fits the panel, with `ui::fitWidth` (`src/ui/displays.h`), which keeps an open panel on
  its monitor, moved left only as far as the monitor's right edge needs; it asks for the monitors only when the
  sections' width or the window's bounds changed since it last looked, not in every frame while the transport runs, and
  when the monitors change (`ui::Shell::onDisplaysChanged`), so that a panel as wide as its monitor's usable area gets
  narrower with it, at another resolution or with the taskbar moved to the side (#1701), and one that the shell moved
  onto the primary monitor, shrunk to its usable area, follows its sections on from the width the shell gave it (#1798).
  The shell keeps these tasks, which do nothing once the fit is gone (#1765). Its unit tests run on the fake window
  system (`tests/testutils/fakeui.h`), which can size windows later and counts how often the monitors were asked for.
- **Time display window** (pp. 312–313): `window.timeDisplay` shows and hides `resources/ui/time-display.rml`, the
  project cursor in large digits, in the format of the slot `timeDisplay`, which its context menu picks. The main
  menu's Studio menu will run it. The context menu also has Show Beat Count Only (`timeDisplay.beatCountOnly`): the
  beat in its bar, "1" to "4" in 4/4, for a visual metronome; and Increase and Decrease Window Size
  (`timeDisplay.increaseSize`, `timeDisplay.decreaseSize`): five sizes, from 320 by 80 to 1280 by 320 dp, 480 by 120 by
  default. Both are the user setting `ui.timeDisplay` (`beatCountOnly`, `size` 0 to 4), so the window opens as it
  was. The display has the state class `.size-0` to `.size-4`; the theme sizes its digits for each, from its token
  `timeDisplaySizes` ([themes.md](themes.md)). The window sizes are in dp, so that the digits fit at every UI scale:
  `AppUi` sizes the window with the window's dp ratio and the pixels of a coordinate of the window system, opens it at
  the saved size, and follows `ui.scale` (`Shell::onUiScale`): a window at a preset's size goes to that size at the
  new scale, one that the user sized keeps its proportions. A new size from `ui.timeDisplay`, not another option such
  as Show Beat Count Only, sets the size; a window with saved bounds opens with them (#566, #569).

## Key commands

The reference's defaults (p. 1570–1572). The engine's commands are in the default key map, bound everywhere:

| Keys | Command | Reference |
|---|---|---|
| Num/ | `transport.toggle` `cycle` | Cycle |
| Num* | `transport.toggle` `record` | Record |
| C | `transport.toggle` `metronome` | Activate Metronome |
| I, O | `transport.toggle` `punchIn`, `punchOut` | Activate Punch In, Activate Punch Out |

The UI's own, which `AppUi` adds:

| Keys | Command | Reference |
|---|---|---|
| F2 | `window.transportPanel` | Transport Panel |
| Num-, Num+ | `wind.rewind`, `wind.forward` (wind while held) | Rewind, Forward |
| Shift+Num-, Shift+Num+ | `wind.fastRewind`, `wind.fastForward` (wind while held) | Fast Rewind, Fast Forward |
| Mod+Num-, Mod+Num+ | `locate.nudgeCursorLeft`, `locate.nudgeCursorRight` | Nudge Cursor Left/Right |
| . | `time.exchangeFormats` | Exchange Time Formats |
| Shift+L, Shift+R | `enter.leftLocator`, `enter.rightLocator` | Enter Left/Right Locator |
| Shift+P | `enter.position` | Enter Project Cursor Position |
| Shift+D | `enter.length` | Enter Locator Range Duration |
| Shift+T, Shift+C | `enter.tempo`, `enter.signature` | Enter Tempo, Enter Time Signature |
| P (in the project window) | `edit.locatorsToSelection` | Locators to Selection |

Nudge Cursor Left and Right move the cursor by a step of the grid (`snap.set` `grid`) at the cursor, as the event
nudge commands move events ([snap.md](snap.md)), never before the project start; they run while the project window
shows. `locate.previousMarker`, `locate.nextMarker`, `window.timeDisplay` and the time display window's commands have
no keys. The enter commands edit the display
in the transport bar; while it is hidden, they open the transport panel and edit it there, instead of the reference's
transport pop-up window.

## Not yet

The sections that come with their features: the record modes (#96, #97), retrospective record, the punch positions
apart from the locators (p. 319) and the Transport menu's Pre-roll & Post-roll items that play around the selection
(p. 303), the click behaviour itself (#101), external sync, count-in and the click pattern (#101), the markers section
(#91), input and output activity and the audio performance meter. Also: Start from selection; the time display
window's transparency (a studio setup page, p. 313).

## Checks

`tests/history_tests.cpp` checks display choices across unrelated, merged and joined undo steps, while explicit
Project Setup format edits remain undoable. `tests/projectsetup_tests.cpp` checks the format command's events, snap
following, revision, persistence, validation and lack of an undo step.

`tests/transport_tests.cpp`: `transport.toggle` with each setting, `transport.locate` `by`, also huge amounts both ways
and from the largest position, and `project.set` `tempo` and `signature` with undo and refused values.
`tests/timeformat_tests.cpp`: positions typed in every format read back as the engine converts them, partial and invalid
text, timecode hours past 2147483647, lengths in bars and beats across a tempo change, and the time context from
`project.get`. `tests/transportbar_tests.cpp` drives the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)): every
button of the transport bar, all its controls inside a 1024-pixel window at a dp ratio of 0.8, a new transport panel as
wide as its sections, a window too narrow for the transport bar hiding its last sections whole, the Left and Right
Divider keeping the sections outside them, and the panel without dividers, the switches following
another client's changes, the marker buttons with and without markers, Locators to Selection by button and key, the time
format menus, Exchange Time Formats by button and key, `ui.timeFormats` and the project's time format from the engine,
also by undo and redo; in every theme and every format, at dp ratios of 1 and 0.85, each theme applied before it is
measured, the cursor's displays keeping their width and the controls to their right their place through positions with
more and fewer digits, up to each format's widest text the widths hold (bars -999 and 9999 in 99/1, just under 100
hours, 11 digits of samples), with their text inside; positions, locators and a length typed into the displays (also the
position a display shows, from between its ticks), the tempo and signature fields with undo, every key command, the
transport panel opened by F2, by an enter command and again after its window was closed, and the time display window
with its context menu; Rewind, Forward and Fast Forward winding while their key or button is held and stopping when it
goes up, also after Shift; Nudge Cursor on bar, beat and second grids and at the project start; the wheel on the
position, locator and range displays in three formats, and past the largest project position, where positions, locators
and a length typed stop too, also for turns far larger than a wheel's; a drag and wheel turns on the tempo field as one
undo step each, in fixed tempo mode and in tempo track mode; a tempo with more decimals than the field shows, kept when
the field is left untouched and set when its number is typed; the position typed back into the primary display during
playback; Show Beat Count Only and the window sizes, kept for the window opened again; and the setup panes of the
transport bar and the panel, each with its own sections, and Show All. A button or menu item with a command that doesn't
exist, parameters it doesn't take or parameters that aren't JSON makes `TransportControls` throw; without a check, only
the last. `tests/mainmenu_tests.cpp` opens the transport panel and the time display window from a copy of the resources
with a control that is refused: the window closes again, also when it is opened a second time, `transportPanel()` or
`timeDisplay()` stays null, and the other window still opens. `tests/keycommands_tests.cpp` checks held keys,
`tests/theme_tests.cpp` the themes' time display sizes.
