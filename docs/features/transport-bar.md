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
| Transport Controls | Go to Previous Marker/Zero, Go to Next Marker/Project End, Rewind, Forward, Cycle, Stop, Start, Record | `locate.previousMarker`, `locate.nextMarker`, `transport.locate` `by` -1 and 1 seconds, `transport.toggle` `cycle`, `transport.stop`, `transport.play`, `transport.toggle` `record` |
| Time Displays | primary and secondary time format menus and displays, Exchange Time Formats | the project cursor; `transport.locate`; `time.exchangeFormats` |
| Tempo & Time Signature | tempo, time signature | `project.set` `tempo`, `signature` |
| Click | Activate Metronome Click | `transport.toggle` `metronome` |

- **Buttons** have a `command` with `parameters` (JSON), which run as a key would (`KeyCommands::run`): engine
  commands through the UI's engine client, the UI's own commands in `AppUi`. Buttons with `state` are checked, the
  state class `.checked`, while the transport's value is true; they follow `transport.changed`, whoever changed it.
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
  step each; later tempo and signature changes stay. A signature is typed as "3/4"; the engine refuses denominators
  other than 1, 2, 4, 8 and 16.
- **Time formats** are the user setting `ui.timeFormats` (`primary`, `secondary`, `timeDisplay`: `barsBeats`,
  `seconds`, `timecode`, `samples` or `userFrames`; Bars+Beats, Timecode and Bars+Beats by default), so every transport
  control shows the same and keeps it after a restart. The menus' items have `slot` and `format`, and the item of the
  slot's format is checked.

## Windows

- **Transport panel** (p. 295): F2 (`window.transportPanel`) shows and hides a window of its own,
  `resources/ui/transport-panel.rml`, with the same controls. Closing the window closes the panel.
- **Time display window** (pp. 312–313): `window.timeDisplay` shows and hides `resources/ui/time-display.rml`, the
  project cursor in large digits, in the format of the slot `timeDisplay`, which its context menu picks. The main
  menu's Studio menu will run it.

## Key commands

The reference's defaults (p. 1570–1572). The engine's commands are in the default key map, bound everywhere:

| Keys | Command | Reference |
|---|---|---|
| Num/ | `transport.toggle` `cycle` | Cycle |
| Num* | `transport.toggle` `record` | Record |
| C | `transport.toggle` `metronome` | Activate Metronome |
| I, O | `transport.toggle` `punchIn`, `punchOut` | Activate Punch In, Activate Punch Out |
| Num-, Num+ | `transport.locate` by -1, 1 seconds | Rewind, Forward |
| Shift+Num-, Shift+Num+ | `transport.locate` by -4, 4 seconds | Fast Rewind, Fast Forward |

The UI's own, which `AppUi` adds:

| Keys | Command | Reference |
|---|---|---|
| F2 | `window.transportPanel` | Transport Panel |
| . | `time.exchangeFormats` | Exchange Time Formats |
| Shift+L, Shift+R | `enter.leftLocator`, `enter.rightLocator` | Enter Left/Right Locator |
| Shift+P | `enter.position` | Enter Project Cursor Position |
| Shift+D | `enter.length` | Enter Locator Range Duration |
| Shift+T, Shift+C | `enter.tempo`, `enter.signature` | Enter Tempo, Enter Time Signature |
| P (in the project window) | `edit.locatorsToSelection` | Locators to Selection |

`locate.previousMarker`, `locate.nextMarker` and `window.timeDisplay` have no keys. The enter commands edit the display
in the transport bar; while it is hidden, they open the transport panel and edit it there, instead of the reference's
transport pop-up window.

## Not yet

The sections that come with their features: the record modes (#96, #97), retrospective record, pre-roll and post-roll
and the punch positions (#102), the punch and click behaviour itself (#101, #102), the tempo track switch and tap tempo
(#155), external sync, count-in and the click pattern (#101), the markers section (#91), input and output activity and
the audio performance meter. Also: winding while Rewind or Forward is held, which now step a second per click or key;
nudging the cursor (Mod+Num-, Mod+Num+); Start from selection; the setup panes that show and hide sections and the
dividers; the time display window's beat count and window size; changing a display's value with the wheel.

## Checks

`tests/transport_tests.cpp`: `transport.toggle` with each setting, `transport.locate` `by`, and `project.set` `tempo`
and `signature` with undo and refused values. `tests/timeformat_tests.cpp`: positions typed in every format read back
as the engine converts them, partial and invalid text, lengths in bars and beats across a tempo change, and the time
context from `project.get`. `tests/transportbar_tests.cpp` drives the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)):
every button of the transport bar, the switches following another client's changes, the marker buttons with and
without markers, Locators to Selection by button and key, the time format menus, Exchange Time Formats by button and
key, `ui.timeFormats` from the engine, positions, locators and a length typed into the displays, the tempo and
signature fields with undo, every key command, the transport panel opened by F2, by an enter command and again after
its window was closed, and the time display window with its context menu.
