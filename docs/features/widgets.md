# Widgets

GNUbase's widget library (#54): the controls that documents use, with the behaviour users know from the reference
(reference manual pp. 118, 1598 and 1618–1621). The code is in `src/ui/widgets.h`; `app::AppUi` registers the widgets,
so every document in the app and in UI tests has them.

- **Look.** Widgets only change their state: attributes, pseudo-classes such as `:disabled`, and state classes such as
  `.checked`. The shared style sheets in `resources/styles` lay them out and the active theme draws each state from its
  skins ([themes.md](themes.md)), so no widget sets a color, font or skin. Code sets only geometry from data, such as
  where a fader's cap sits, and in the color picker the colors that are its data.
- **Data.** A widget keeps its state in attributes (`value`, `checked`, `disabled`) and reports what the user changed
  with a `change` event whose `value` parameter is the new value. So `data-value` and `data-checked` bind widgets to a
  data model ([architecture.md](../architecture.md#rmlui)), whose handler sends the engine command; the engine's
  events then set the model, and through it the widget (#50, [ui-engine-binding.md](ui-engine-binding.md)). A widget
  that nothing binds keeps the state the user gave it.
- **Automation.** Widgets are plain elements in the DOM, so MCP's UI tools and UI tests read their attributes, classes
  and boxes and drive them with clicks, drags, the wheel and keys ([ui-automation.md](ui-automation.md)).

## Buttons and toggles

- `<button>` is clicked with the mouse, or with Space or Enter while it has the focus. With the `disabled` attribute it
  gets `:disabled`, takes no focus, and clicks pass through it. `button.primary`, `button.icon-only` and the small
  track buttons `button.toggle` (`.mute`, `.solo`, `.record`, `.monitor`) are looks from the theme.
- `<button type="toggle">` is a toggle button: a click turns it on or off. Its state is the `checked` attribute, as
  for check boxes, shown by the state class `.checked`; it sends `change` with the new state as a bool, so
  `data-checked="muted"` binds it.

## Knobs, faders, sliders and value fields

Widgets with a number share their behaviour (reference manual p. 1598, "Editing - Controls"):

- **Attributes.** `value`, `min` (0), `max` (1), `default` (`min`) and `step` (a hundredth of the range). The value
  stays between `min` and `max`; `change` sends it as a float.
- **Drag** up or to the right for more: a knob or a value field changes over its whole range in 200 dp of mouse
  movement, from wherever it was grabbed (the reference's "Linear" knob mode); a fader's cap and a slider's thumb
  follow the mouse.
- **Fine adjust.** With Shift held, drags, wheel turns and arrow keys change the value ten times more finely.
- **Wheel and keys.** One wheel line or arrow key (Up and Right for more) changes it by a step. Over a widget the wheel
  turns it rather than scrolling what's around it. The widgets take the focus when clicked, and with Tab.
- **Reset.** Ctrl-click (Cmd-click on macOS) sets the default.
- `disabled` gives `:disabled` and ignores the user.

The widgets:

- `<knob>`: the theme's `.knob` with its `.knob-pointer` turned from -135 to 135 degrees.
- `<fader>`: vertical, the `.fader` track with its `.fader-cap`.
- `<slider>`: horizontal, `.slider-track` filled up to the `.slider-thumb` (`.slider-fill`).
- `<valuefield>`: the value as text, with `decimals` (2) and a `unit`, drawn like a display. A double click edits it
  as text (state class `.editing`): Enter, or leaving the field, takes the number typed, clamped to the range; Escape,
  or text that isn't a number, keeps the value. While the user types, the field's text field sends RmlUi's own
  `change` events with the text; the field's `change`, with the field as its target, has the number.

Code places the parts from the value (the pointer's rotation, the cap's and the thumb's position, the fill's width),
which is geometry from data; the shared style sheet lays out the rest. Value fields parse and print numbers in the C
locale, "-6.5", whatever the system's. Not yet: the reference's other knob and slider modes (circular, jump, ramp),
value fields that step on clicks, non-linear scales such as a fader's dB law, and the touch and release events that
automation writing will need.

## Meters

`<meter>` shows `value` and `peak` in dB between `min` (-60) and `max` (6). The level shows the lower part of the
theme's `.meter-fill`, which is as tall as the meter, so the gradient keeps its colors at every level; `.meter-peak`'s
top edge marks the peak, and a meter without one hides it. Engine meters reach the UI once a frame (#111) and set the
attributes, through a data model or directly.

## Menus and context menus

- `<menu>` holds `<menuitem>`s and `<menuseparator>`s and stays hidden until it opens, with the state class `.open`:
  under a button whose `menu` attribute names it by id (a second click on the button closes it), or at the mouse on
  a right click on an element whose `contextmenu` attribute names it. It opens inside the window, moved left or up
  where it would stick out. Put menus straight into the body, so that no container clips them.
- A click on an item is a click on that item, for the document to handle, and closes the menu. Escape and a click
  outside close it too. A `disabled` item gets `:disabled` and ignores clicks; `type="toggle"` makes an item checkable,
  as for buttons.
- A `<menu>` inside an item is its submenu, shown beside the item while the mouse is over it (pure RCSS:
  `menuitem:hover > menu`); a click on that item keeps the menu open.
- Keys: Down and Up move the focus between the items that aren't disabled, Enter clicks the focused one.

## Tooltips

An element's `title` attribute is its tooltip: when the mouse has rested 0.6 seconds over the element, or over one
inside it without a title of its own, the document's `<tooltip>` shows the text 20 dp under the mouse, inside the
window, with the state class `.shown`. It goes when the mouse moves to an element without a title or leaves the window,
and on a click, a key or the wheel. Every document gets its tooltip element when it loads; it lets the mouse through.

## Dialogs

`<dialog>` is a dialog in a document of its own, which the app loads and shows, usually modal
(`ElementDocument::Show(Rml::ModalFlag::Modal)`). It opens in the middle of the window, and dragging its
`.dialog-titlebar` moves it. Escape closes it with the result `cancel`; Enter clicks its `button.primary`, also from
a text field, but not from a multi-line one or a value field being edited; a click on an element with a `result`
attribute closes it with that result. Closing sends the event `close` (`ui::g_dialogCloseEvent`) with the parameter
`result`, then closes the document.

How it works: an RmlUi plugin (`RmlUi::registerPlugin`) gives every document a tooltip and listens to it in the capture
phase, before its elements: for `contextmenu` and `menu` attributes, titles, clicks outside open menus, Escape, and
its dialog's keys, which text fields would otherwise keep to themselves.

## Tabs

RmlUi's `<tabset>` with its `<tab>`s and `<panel>`s: a click on a tab shows its panel and gives the tab `:selected`.
The theme draws them; the widget library adds nothing.

## List, tree and table views

`<listview>` is a list, tree or table of rows that the user selects: its `<listitem>`s, or the rows (`<tr>`) of a
`<table>` in it, except those of the `<thead>`. Each row has a `value` attribute (without commas).

- **Selection.** A click selects a row. With the `multiple` attribute, Ctrl-click (Cmd-click on macOS) adds or removes
  a row and Shift-click selects the rows from the last one clicked. Up and Down move the selection, Shift extends it,
  Home and End go to the first and last row. Selected rows have the state class `.selected`. The view's `value`
  attribute is the selected rows' values separated by commas, and `change` sends it, so `data-value` binds the
  selection both ways, also when `data-for` makes the rows. Clicks and keys in a form control in a row, such as a text
  field, are the control's and don't change the selection.
- **Trees.** Rows with a `level` attribute (0 at the top) make a tree: the rows that follow a row with a higher level
  are its children, shown while it has the `expanded` attribute. The view gives tree rows a `.tree-indent`, sized from
  the level (16 dp a level), and a `.tree-toggle`, which the theme draws as an arrow; parents get the state classes
  `.parent` and, when expanded, `.expanded`, and rows inside a collapsed parent `.hidden`. A click on the toggle, or
  Right and Left on the selected row, expand and collapse it and send `toggle` (`ui::g_treeToggleEvent`) with the row's
  `value` and `expanded`. The rows are flat, so data models describe trees as lists with levels.
- **Tables.** `table`, `thead`, `tbody`, `tr` and `td` are laid out as tables; selected body rows look like selected
  list items. Not yet: sorting by a column and resizing columns.

## Splitters

`<splitter>` between two elements of a flex row or column moves the boundary between them when dragged: it sets the
width or height of the element before it, or with `resize="next"` of the element after it (without its padding and
borders), from its `min-width` or `min-height` to taking all of the other element down to that one's minimum, and
sends `change` with that size in dp, e.g. for a workspace to keep. In a column it has the state class `.column`; the
theme's grip shows which way it moves. The docking system's zones are sized with splitters ([docking.md](docking.md)).

## Color picker

`<colorpicker>` picks a color, its `value` attribute `#rrggbb` (reference manual pp. 118-119): a field picks the
saturation (left to right) and the value (bottom to top) for the hue that the hue strip beside it picks, both from a
click and through a drag; value fields edit hue (0-360), saturation and value (0-100), and red, green and blue
(0-255). Two swatches show the color the picker started with and the new one. Each change sets `value` and sends
`change` with it, so `data-value` binds the picker; the value fields inside send their own `change` events with
numbers. A grey keeps the hue it had, so turning the saturation up again doesn't jump to red. The field's gradients
and the hue strip are in the themes; code gives the field its hue and the swatches their colors, the one place where
code sets a color, since there the color is the user's data: `background-color` only, as CLAUDE.md's UI conventions
allow (`tests/stylerules_tests.cpp` lists the files that may). Dialogs put the
picker next to their OK and Cancel buttons. Not yet: the reference's context menu to copy, paste and reset colors.

## Checks

`tests/widgets_tests.cpp` drives each widget in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) through
`ui::Automation`: buttons clicked with the mouse and the keyboard, disabled buttons that ignore clicks, and toggle
buttons that turn on and off, look different when on, and follow a data model. Knobs, faders, sliders and value
fields are dragged (also with Shift), turned with the wheel, stepped with the arrow keys and reset with Ctrl-click,
their parts placed where the value says; data-value binds them both ways; the wheel turns them instead of scrolling
the page; value fields take typed numbers and leave bad ones. Meters show their level and peak at the right heights.
Menus open under their button and at the mouse, inside the window, show submenus on hover, close on an item, a click
elsewhere and Escape, skip disabled items, and work with the keys. Tooltips show after a rest and go with the mouse.
Dialogs open in the middle, move by their title bar, and close with Enter, Escape and their buttons, with the result.
Tabs show their panels. List views select with clicks, Ctrl, Shift and the keys, and follow their data; trees expand
and collapse by click and keys, with indents by level; tables select body rows only; splitters move the boundary in a
row and in a column, never below nothing. The color picker picks from its field, its hue strip and its numbers,
draws the old and the new color, and follows its data.
