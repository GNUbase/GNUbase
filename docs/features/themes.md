# Themes

GNUbase's look is a theme that can be switched while GNUbase runs: `gnubase` (the default), `dark` and `light` to start
with (#53), and `high-contrast` ([accessibility.md](accessibility.md), #60). Code never styles the UI. C++ and Lua change an element's classes and pseudo-classes, and style sheets decide
how each state looks (CLAUDE.md, "UI conventions"). The code is in `src/ui/themes.h`, `src/ui/uisettings.h` and
`Shell::setTheme`.

- **Design system.** The shared style sheets in `resources/styles` lay out every widget in `dp`: sizes, padding, the
  parts of drop-down lists, sliders, scroll bars and tabs. RmlUi has no default style sheet, so they also make `div`,
  `p` and the like blocks. They hold no colors, fonts, font sizes or skins (`tests/theme_tests.cpp` checks the sizes).
- **Themes.** A theme is a folder in `resources/themes` with its skins (SVG), `manifest.json`, `tokens.json` and
  `theme.rcss`. `tools/skin/generate.py` makes all of them from one table of tokens per theme (colors, radii, border
  widths, font sizes and families), so the skins and the style sheet always agree; change the generator and run it
  again rather than editing its output (`resources/themes/README.md`); CI's format job runs it into a temporary folder
  and fails when the output differs from `resources/themes`, naming the files that differ. `theme.rcss` gives each
  widget state its colors, fonts and skin, all inside `@media (theme: <name>)`. The manifest's `theme` is the theme's
  id, its folder's name, which `ui.theme` stores, and its `displayName` the name the UI shows for it, so a document
  never names a theme: `ui::Themes` reads both and shows the id of a theme whose manifest has no name
  (`Themes::themes`), and the preferences dialog lists the themes it gets from `gnubase.ui.themes()`
  ([preferences.md](preferences.md)).
- **Skins.** Widgets draw the theme's 9-slice SVGs with `decorator: svg-ninepatch(<file>, <top> <right> <bottom>
  <left>)`, a decorator in GNUbase's RmlUi fork. The skin is rasterized once per dp ratio at its own size (a 24 dp
  skin is 48 pixels at 200 %), whatever the size of the widgets that use it, and drawn as nine patches: the corners
  keep their size, the edges stretch along one axis and the middle along both, as with CSS's `border-image`. Fixed
  graphics (check marks, the slider's thumb, the knob, the fader cap, the meter) use `decorator: svg(<file>)`, which
  rasterizes at the element's size ([svg.md](svg.md)). Both find a file relative to the style sheet that names it.
  A widget that the keyboard focused (`:focus-visible`) gets the theme's focus ring over the skin of its state, a
  second decorator that the generator adds to each such rule ([accessibility.md](accessibility.md)).
  The default theme's glows are part of its skins, so they cost no extra render passes.
- **Flat widgets.** The default theme follows the mockup `art/mockups/main-window.webp` (#1311, #1328); the elements and
  layout stay as they are. Its toolbar and transport buttons have no frame: muted until the mouse is over them, a soft
  fill under it, the accent when checked, Start in the accent and Record in the danger color; the primary time display
  is bold. The menu bar and toolbars are in the window's dark tone. Menu buttons that show a value look like drop-down
  fields with an arrow, tabs are names over an accent line, zones are rounded panels over the window's background, the
  event display's lanes are on the panels' surface with the track list a step lighter, track list rows are raised, a
  selected list item is tinted and outlined in the second accent, inspector section headers are bold names on a tile
  after a disclosure arrow, value and text fields and drop-down lists are raised fields lighter than their panel,
  without the accent line, mute, solo and record buttons are filled boxes, mixer channel strips are raised panels with
  light fader caps over thin slots and knobs without the fixed accent arc, and the transport bar's time displays are
  plain digits. The generator's `flat_rules` gives every theme the same selectors (the check below): the default theme
  its flat look, the others what their general rules already give, so they keep their framed look. Their drop-down
  button and disclosure skins are the plain button's. Rules for buttons in bars skip toggle and primary buttons
  (`:not(.toggle):not(.primary)`), which keep their skins.
- **Every document gets them.** While a `ui::Themes` exists, every document RmlUi loads gets the shared style sheets
  and every theme's style sheet ahead of its own style (an RmlUi plugin, on document load). Documents name neither a
  theme nor a shared style sheet, and their own rules, which only lay out, come last. Live reload in Debug builds picks
  up changed style sheets: it clears RmlUi's style sheet cache and loads the documents again.
- **Switching.** `Shell::setTheme(name)` activates the theme's media query in every window's RmlUi context, and in
  windows opened later. Documents restyle at once, without loading again.
- **Settings.** The user setting `ui.theme` names the theme, `ui.scale` the UI scale (settings.md). `ui::UiSettings`
  applies both when the UI starts and follows `settings.changed`, so `settings.set` from MCP or Lua switches the theme
  of the running UI. Engine events arrive on the engine's thread; the UI applies them on its own thread in the shell's
  next step, through `Shell::post`, which wakes the event loop (`WindowSystem::wake`). A theme that doesn't exist, such
  as one a newer GNUbase saved, falls back to `gnubase`.

## Class names

The theme styles RmlUi's controls by their tags and states, and GNUbase's containers and widget parts by class:

- **Text:** `body` has the theme's font and colors; `h1`, `h2`, `.text-title`, `.text-display`, `.text-label`,
  `.text-small`, `.text-muted`, `.text-accent`, and `.counter` for time and counter displays in the counter font.
- **Icons:** `svg.icon` (resources/icons) in the text color, `.accent`, `.muted` or `.danger`; icons in buttons follow
  the button's state.
- **Buttons:** `button` with `:hover`, `:active`, `:disabled`, `:focus-visible` and the state classes `.checked` and
  `.suspended` (a switch that is on but paused, such as auto-scroll while editing); `button.primary`; `button.icon-only`; toggle buttons `button.toggle` with `.mute`, `.solo`, `.record` or `.monitor`
  and `.checked` when on.
- **Fields and choices:** `input.text`, `input.password`, `textarea` (`:focus`, `:disabled`), `select` (`:hover`;
  `:focus` while open or focused), `input.checkbox` (`:checked`, `.mixed`), `input.radio`, `input.range`, `progress`,
  and the scroll bars.
- **Containers:** `.dialog` (and `dialog`), `.dialog-titlebar`, `.panel`, `.panel-header`, `.toolbar`, `.display`,
  `.tooltip` (and `tooltip`), `.menu` with `.menu-item` and `.menu-separator` (and `menu`, `menuitem` with `:hover`,
  `:focus-visible`, `:disabled` and `.checked`, `menuseparator`), `.list` with `.list-item` (`.selected`) (and
  `listview` with `listitem` and table rows, `.tree-toggle`), `tabset` with its tabs.
- **Mixer and editor parts**, which the widget library's elements are made of ([widgets.md](widgets.md)): `.knob`,
  `.knob-pointer`, `.fader`, `.fader-cap`, `.slider-track`, `.slider-fill`, `.slider-thumb`, `valuefield`, `.meter`,
  `.meter-fill`, `.meter-peak`, the color picker's `.color-field`, `.color-hue` and their thumbs, `.splitter-grip-vertical`, `.splitter-grip-horizontal`, `.dropdown-arrow`,
  `.focus-ring`.
- **Canvas elements** ([canvas.md](canvas.md)) by their tags: `event-lanes`, `waveform`, `curve` and `ruler`, with
  `color` and the canvas properties `stroke-color`, `stroke-width`, `own-fill-opacity`, `own-color-tint` and
  `event-inset`.
- **Event display** ([event-display.md](event-display.md)): `.event-display` with `.overview`, `.overview-view`,
  `.project-cursor`, `.cross-hair-vertical` and `.cross-hair-horizontal`.

## Checks

- `tests/uithemes_tests.cpp`: the themes are found, a theme style sheet with rules outside its media query is an
  error, documents get the theme's colors, fonts and skins for each state and the design system's layout with their
  own last, and changed style sheets apply when documents load again.
- `tests/shell_tests.cpp`: a theme applies in every window, also in windows opened later, without loading documents
  again; work posted from another thread runs on the UI thread in the next step.
- `tests/uisettings_tests.cpp`: `ui.theme` and `ui.scale` apply at start and follow `settings.set`, and an unknown
  theme falls back to the default.
- `tests/svg_tests.cpp`: a 9-slice skin keeps its corners at their size at 100, 150 and 200 %, and is rasterized once
  per scale for any number of widgets of any size; decorators find their files next to the style sheet.
- `tests/theme_tests.cpp`: every theme's style sheet uses all its skins and styles the same widgets and states as the
  others, draws the focus ring on every focusable widget in each of its states, gives the key editor's piano keyboard
  white keys lighter than its black keys, dims the audio part editor's events that their part hides entirely, and the
  high contrast theme meets WCAG's contrast ratios. The default theme's bar buttons, tabs and inspector headers are
  flat, the other themes' framed, their arrow skins the plain button's, and no bar rule reaches toggle or primary
  buttons; the default theme's lanes and track list are off the window's background, the other themes' lanes on it,
  and its mixer strips and transport displays follow the mockup.
- `tests/stylerules_tests.cpp`: C++ and Lua code (`src`, `resources`, `tests/lua`) never sets a style property other
  than geometry from data (position, size, transform), and documents and shared style sheets never name a theme.
- `tests/window_tests.cpp`: a page of widgets (`tests/data/ui/widgets.rml`) in every theme, and in the default theme
  at dp ratio 2, compared with golden images on every platform; switching back to a theme gives the same pixels.
