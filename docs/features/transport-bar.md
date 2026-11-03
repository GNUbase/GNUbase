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
| Locators | Go to Left/Right Locator Position, the left and right locator | `transport.locate` to `leftLocator` and `rightLocator`; `transport.set` `left`, `right` |
| Locator Range Duration | Locators to Selection, the range's length | `edit.locatorsToSelection` (the event display's selection); `transport.set` `right` |
| Punch Points | Punch In, Punch Out | `transport.toggle` `punchIn`, `punchOut` |
| Transport Controls | Go to Previous Marker/Zero, Go to Next Marker/Project End, Rewind, Forward, Cycle, Stop, Start, Record | `locate.previousMarker`, `locate.nextMarker`, `wind.rewind`, `wind.forward`, `transport.toggle` `cycle`, `transport.stop`, `transport.play`, `transport.toggle` `record` |
| Time Displays | primary and secondary time format menus and displays, Exchange Time Formats | the project cursor; `transport.locate`; `time.exchangeFormats` |
| Tempo & Time Signature | Activate Tempo Track, tempo, Tap Tempo, time signature | `project.set` `tempoTrackActive`, `tempo`, `signature`; `tempo.tap` ([tempo-track.md](tempo-track.md)) |
| Click | Activate Metronome Click | `transport.toggle` `metronome` |

The sections keep the width of their controls. A window too narrow for all of them, such as the transport bar of a
project window under about 1180 dp wide, cuts off the last sections at its right edge instead of drawing sections over
each other.

- **Buttons** have a `command` with `parameters` (JSON), which run as a key would (`KeyCommands::run`): engine
  commands through the UI's engine client, the UI's own commands in `AppUi`. Buttons with `state` are checked, the
  state class `.checked`, while the transport's value is true; they follow `transport.changed`, whoever changed it.
  Buttons with `hold` run their command when the mouse button goes down and end it, as a key held, when it goes up,
  the mouse leaves the button or the window loses the keyboard focus.
- **Winding** (pp. 297, 300): Rewind and Forward, and Fast Rewind and Fast Forward on the keys, move the cursor by a
  step at once (1 second, 4 for the fast ones), as a click or a key pressed and released does, and while their button
  or key stays down wind on after 0.3 seconds at four steps a second until it goes up (`ui::Winding`, which sends
  `transport.locate` `by` each frame). A key ends winding when it goes up, whatever modifiers are still down, and so
  does the window losing the keyboard focus ([key-commands.md](key-commands.md)).
- **The mouse wheel** changes a display's value: up for later, by a beat in Bars+Beats, a frame in Timecode and fps
  (User), a second in Seconds and Samples, never before the project start. On the locator range it moves the right
  locator, not before the left one. A display being edited ignores the wheel, and a scroll sideways moves nothing.
- **Displays** (`time`) show the project cursor, a locator or the locator range in the time format of their slot
  (`format`: `primary`, `secondary`, `timeDisplay`). While playing they read the playback position once a frame, as
  the event display does. They are `<textdisplay>`s ([widgets.md](widgets.md)): a double click edits one, and Enter
  takes what was typed in its format (`ui::parseTime`): "3.2" is bar 3, beat 2, "1:30" a minute and a half,
  "5:12" five seconds and twelve frames; what is left out counts from the start. Text that isn't a position changes
  nothing.
- **The locator range** shows the length from the left to the right locator: in bars and beats counted from zero,
  "1.1.0.0" for a bar and a beat, in the other formats as a time (`ui::formatLength`). A length typed moves the right
  locator.
- **Tempo and signature** show the project's first tempo and time signature and set them with `project.set`, one undo
  step each; later tempo and signature changes stay. The tempo field is a `<valuefield>`: a drag on it, or wheel turns
  without a pause of 0.4 seconds, set the tempo once, at the gesture's end, so that the gesture is one undo step;
  meanwhile the field shows where the gesture took it. A signature is typed as "3/4"; the engine refuses denominators
  other than 1, 2, 4, 8 and 16.
- **Time formats** are the user setting `ui.timeFormats` (`primary`, `secondary`, `timeDisplay`: `barsBeats`,
  `seconds`, `timecode`, `samples` or `userFrames`; Bars+Beats, Timecode and Bars+Beats by default), so every transport
  control shows the same and keeps it after a restart. The menus' items have `slot` and `format`, and the item of the
  slot's format is checked.

## Sections and setup panes

The transport bar's sections are the docking system's set of sections `transport` ([docking.md](docking.md)):
Locators, Locator Range Duration, Punch Points, Transport Controls, Time Displays, Tempo & Time Signature and Click.
A right click on the transport bar opens its setup pane (the reference's Set up Transport Bar, p. 306), a toggle
button per section that shows or hides it, and Show All. The transport panel is a docking window of its own,
`transportPanel`, with the same pane for its own sections (Set up Transport Panel, p. 295). Both are kept in
`ui.layout`, under `windows`, `project` and `transportPanel`, `sections`, `transport`.

## Windows

- **Transport panel** (p. 295): F2 (`window.transportPanel`) shows and hides a window of its own,
  `resources/ui/transport-panel.rml`, with the same controls. Closing the window closes the panel.
- **Time display window** (pp. 312–313): `window.timeDisplay` shows and hides `resources/ui/time-display.rml`, the
  project cursor in large digits, in the format of the slot `timeDisplay`, which its context menu picks. The main
  menu's Studio menu will run it. The context menu also has Show Beat Count Only (`timeDisplay.beatCountOnly`): the
  beat in its bar, "1" to "4" in 4/4, for a visual metronome; and Increase and Decrease Window Size
  (`timeDisplay.increaseSize`, `timeDisplay.decreaseSize`): five sizes, from 320 by 80 to 1280 by 320, 480 by 120 by
  default. Both are the user setting `ui.timeDisplay` (`beatCountOnly`, `size` 0 to 4), so the window opens as it
  was. The display has the state class `.size-0` to `.size-4`; the theme sizes its digits for each, from its token
  `timeDisplaySizes` ([themes.md](themes.md)).

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

The sections that come with their features: the record modes (#96, #97), retrospective record, pre-roll and post-roll
and the punch positions (#102), the punch and click behaviour itself (#101, #102), external sync, count-in and the click pattern (#101), the markers section (#91), input and output activity and
the audio performance meter. Also: Start from selection; the left and right dividers, which keep the sections outside
them shown in a window too narrow for all (pp. 307, 310); the time display window's transparency (a studio setup page,
p. 313).

## Checks

`tests/transport_tests.cpp`: `transport.toggle` with each setting, `transport.locate` `by`, and `project.set` `tempo`
and `signature` with undo and refused values. `tests/timeformat_tests.cpp`: positions typed in every format read back
as the engine converts them, partial and invalid text, lengths in bars and beats across a tempo change, and the time
context from `project.get`. `tests/transportbar_tests.cpp` drives the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)):
every button of the transport bar, a window too narrow for the transport bar cutting off its last sections, the
switches following another client's changes, the marker buttons with and
without markers, Locators to Selection by button and key, the time format menus, Exchange Time Formats by button and
key, `ui.timeFormats` from the engine, positions, locators and a length typed into the displays, the tempo and
signature fields with undo, every key command, the transport panel opened by F2, by an enter command and again after
its window was closed, and the time display window with its context menu; Rewind, Forward and Fast Forward winding
while their key or button is held and stopping when it goes up, also after Shift; Nudge Cursor on bar, beat and second
grids and at the project start; the wheel on the position, locator and range displays in three formats; a drag and
wheel turns on the tempo field as one undo step each; Show Beat Count Only and the window sizes, kept for the window
opened again; and the setup panes of the transport bar and the panel, each with its own sections, and Show All.
`tests/keycommands_tests.cpp` checks held keys, `tests/theme_tests.cpp` the themes' time display sizes.
