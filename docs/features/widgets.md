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
  track buttons `button.toggle` (`.mute`, `.solo`, `.record`, `.monitor`) are looks from the theme. Buttons in a
  `.toolbar` (the project window's, the mixer's, the pool's) and in the transport controls (`.transport`) are for the
  mouse, as in the reference (reference manual pp. 295-311): a click focuses them, but Tab skips them and Space and
  Enter go on to the key commands, so Space still starts and stops playback after a click on Cycle or Snap (#456).
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
- **Gestures.** `change` also sends `gesture`, an integer: the changes of one drag, of wheel turns until the mouse
  leaves the widget, of an arrow key until it is released, and a typed value each have one, and the next gesture of any
  widget has a higher one. Documents make a gesture one undo step with it (`EngineBinding::sendStep`,
  [edit-history.md](edit-history.md)).
- `disabled` gives `:disabled` and ignores the user.

The widgets:

- `<knob>`: the theme's `.knob` with its `.knob-pointer` turned from -135 to 135 degrees.
- `<fader>`: vertical, the `.fader` track with its `.fader-cap`.
- `<slider>`: horizontal, `.slider-track` filled up to the `.slider-thumb` (`.slider-fill`).
- `<valuefield>`: the value as text, with `decimals` (2) and a `unit`, drawn like a display. A double click, or Enter
  while it has the focus, edits it as text (state class `.editing`): Enter, or leaving the field, takes the number
  typed, clamped to the range; Escape, text that isn't a number, or nothing typed keeps the value, so that a value with
  more decimals than the field shows isn't rounded to them. Typing the number shown takes it. With `autofocus`, it is
  edited, all of it selected, whenever it gets a visible focus: when its document shows it focused, or Tab moves to it,
  but not on a click. Keys typed before the next update go into it too. In a dialog it is then the main field, which
  works like a text field: Enter takes the number typed and clicks OK, Escape cancels. While the user types, the field's
  text field sends RmlUi's own `change` events with the text; the field's `change`, with the field as its target, has
  the number.

`<textdisplay>` shows text, its `value` attribute, such as a position in a time format; documents give it the look of
a display with the class `.display`. A double click edits it as text (`.editing`): Enter, or leaving the field, sends
`change` with the text typed as `value`, a string, and the document reads it and sets the value it takes; Escape
keeps the value, and so does a field nothing was typed into. Any typing counts, also typing the text the field
started with: a display may show a rounded value, or one that has moved on since, such as the position during
playback. The transport controls use it for positions ([transport-bar.md](transport-bar.md)).

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
  a right click on an element whose `contextmenu` attribute names it: the nearest one with the attribute, so an empty
  `contextmenu` keeps the menu of an element around it from opening on a control. With a modifier key held (Ctrl,
  Alt, Shift or Cmd), the menu that its `modifiercontextmenu` attribute names opens instead, if it has the attribute,
  as the event display's and the key editor's toolboxes do ([tools.md](tools.md)); an empty one opens none. It opens inside the window, moved left or up where it would
  stick out. Put menus straight into the body, so that no container clips them.
- A click on an item is a click on that item, for the document to handle, and closes the menu. Escape and a click
  outside close it too. A `disabled` item gets `:disabled` and ignores clicks; `type="toggle"` makes an item checkable,
  as for buttons.
- A `<menu>` inside an item is its submenu, shown beside the item while the mouse is over it (pure RCSS:
  `menuitem:hover > menu`); a click on that item keeps the menu open. The menu gives such an item a `.menu-arrow`
  part when it opens, first in the item so that it floats right; the themes draw it with their dropdown arrow, turned
  to point right.
- Keys: Down and Up move the focus between the items that are shown and not disabled, round the ends, skipping items
  hidden with `display: none` such as those a search leaves out. Tab and Shift+Tab (with Ctrl, Alt or Cmd too) do the
  same and also reach the menu's other controls, such as a search field, so that the focus doesn't leave an open menu
  for a control outside it while the menu stays open (#524). Enter clicks the focused item. Right opens the focused
  item's submenu (with the state class `.open`) and focuses its first item, unless all its items are disabled; Left
  closes the submenu and goes back to its item. A submenu that keys opened closes when the mouse goes to another item of
  its menu, and with its menu. Escape closes every open menu. A menu that closes with the focus in it gives the focus to
  its item, if it is a submenu, or else to its document.
- `<menubar>` holds the titles of menus: elements with a `menu` attribute, usually buttons. While one title's menu is
  open, the mouse over another title opens that one's menu instead; in a title's menu (and its submenus) Right and
  Left open the next and previous title's menu where they don't go into or out of a submenu, with its first item
  focused, skipping hidden and disabled titles and going round. Alt pressed and released on its own opens the menu of
  the first menu bar shown in the window, again with its first item focused, closing other menus, and closes it when
  one of its menus is open. Alt with another key, a modifier, a mouse button or the wheel doesn't, nor in a modal
  document, nor when the window loses or gets the keyboard while Alt is held: SDL then releases the keys held (Alt+Tab)
  or, on Linux, presses them right after the focus event (a window manager's Alt+J), and the window system drops that
  Alt. On macOS Alt does nothing, since the system menu bar takes the keys there.

## Tooltips

An element's `title` attribute is its tooltip: when the mouse has rested 0.6 seconds over the element, or over one
inside it without a title of its own, the document's `<tooltip>` shows the text 20 dp under the mouse, inside the
window, with the state class `.shown`. It goes when the mouse moves to an element without a title or leaves the window,
and on a click, a key or the wheel. Every document gets its tooltip element when it loads; it lets the mouse through.

## Notifications

`<notification>` shows a message for a moment, such as the tool that a key command chose ([tools.md](tools.md)):
setting its attribute `text`, also to the text it has, shows the text with the state class `.shown` for `duration`
seconds (its attribute, 1.5 by default), then hides it. It lets the mouse through; the document places it, and a
class such as `.tooltip` gives it the theme's look.

## Dialogs

`<dialog>` is a dialog in a document of its own, which the app loads and shows, usually modal
(`ElementDocument::Show(Rml::ModalFlag::Modal)`). It opens in the middle of the window, and dragging its
`.dialog-titlebar` moves it. Escape closes it with the result `cancel`, or first an open drop-down list; Enter clicks
its `button.primary`, also from a text field and a value field with `autofocus`, but not from a multi-line text field
or another value field being edited, and not when the focused control takes Enter itself: a button clicks itself, a
drop-down list opens and closes, another value field starts editing. A click on an element with a `result` attribute
closes it with that result. Tab and Shift+Tab move the focus between its controls, around the ends
([accessibility.md](accessibility.md)). Closing sends the event `close` (`ui::g_dialogCloseEvent`) with the parameter
`result`, then closes the document, once: a dialog that is closing doesn't close again. The app closes a dialog the
same way with `ui::closeDialog`, e.g. to cancel the open project dialog when the user quits.

How it works: an RmlUi plugin (`RmlUi::registerPlugin`) gives every document a tooltip and listens to it in the capture
phase, before its elements: for `contextmenu` and `menu` attributes, titles, clicks outside open menus, Escape, the
mouse over menu bar titles, Alt, and its dialog's keys, which text fields would otherwise keep to themselves.

## Tabs

RmlUi's `<tabset>` with its `<tab>`s and `<panel>`s: a click on a tab shows its panel and gives the tab `:selected`.
The theme draws them; the widget library adds nothing.

## List, tree and table views

`<listview>` is a list, tree or table of rows that the user selects: its `<listitem>`s, or the rows (`<tr>`) of the
`<table>`s in it, except those of their `<thead>`s. A list view can hold several tables, also at different depths, such
as the edit history's done and undone steps with a separator between them ([edit-history.md](edit-history.md)): its
rows are in the document's order. Each row has a `value` attribute (without commas).

- **Selection.** A click selects a row. With the `multiple` attribute, Ctrl-click (Cmd-click on macOS) adds or removes
  a row and Shift-click selects the rows from the last one clicked. Up and Down move the selection, Shift extends it,
  Page Up and Page Down move it by one row less than the view shows, Home and End go to the first and last row.
  Selected rows have the state class `.selected`. The view's `value` attribute is the selected rows' values separated
  by commas, and `change` sends it, so `data-value` binds the selection both ways, also when `data-for` makes the rows.
  Rows that code adds, removes or replaces with new elements, all of them or only some, keep the selection that the
  view's `value` gives, also with as many rows and the same value; the keys and Shift-click then go from the first
  selected row. A click or a key before the next update finds the view caught up as well: with the new rows selected
  and shaped as a tree, and with a `value` that code set. Not yet: a row that keeps its element but gets another
  `value`, as when `data-for` reorders its items. Clicks and keys in a form control in a row, such as a text field,
  are the control's and don't change the selection.
- **Scrolling while dragging.** With the `dragscroll` attribute, a drag of an element in the view (`drag: drag`)
  scrolls the view while the mouse is above or below it, by ten times as far as the mouse is past the edge each second,
  also while the mouse is held still. The drag's end, the button going up or the view losing the focus stop it. The
  edit history's separator uses it.
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
the page; value fields take typed numbers, also the number they show, and leave bad ones and values with more
decimals than they show when nothing was typed. Text displays send the text typed with Enter, also the text they
started with, and nothing with Escape or when nothing was typed. Meters show their level and peak at the right heights.
Menus open under their button and at the mouse, inside the window, show submenus on hover, close on an item, a click
elsewhere and Escape, skip disabled items, and work with the keys; items with a submenu show an arrow, and Right and
Left go into and out of the submenu, but not into one with nothing to focus; Tab and Shift+Tab, also with Ctrl or Alt,
step through a menu's and a submenu's items and round their ends without leaving the open menu, also in the project
window's menu bar (`tests/docking_tests.cpp`), and with Down and Up skip a hidden item, also those that the parameter
selector's search hides, where Tab reaches its search field too (`tests/automationtracks_tests.cpp`). In a menu bar the
mouse and Right and Left move between the open menus, also from a submenu, skipping a disabled title, and Alt alone
opens the first menu,
closing a context menu, and closes it, leaving the focus out of the closed menu (not on macOS, nor with another key,
a click, a mouse button going up or the wheel, nor when SDL releases Alt as the window loses the keyboard or
presses it as the window gets it, nor behind a modal dialog). A right click with each modifier key opens the
`modifiercontextmenu`, and the `contextmenu` where an element has no other.
Tooltips show after a rest and go with the mouse. Notifications show for their duration, again when set again.
Dialogs open in the middle, move by their title bar, and close with Enter, Escape and their buttons, with the result,
and with `ui::closeDialog`, once, also when closed again from its own close event; a value field with autofocus is edited as its dialog opens, even by keys typed before the next update, and Enter
there clicks OK, while another value field keeps Enter.
Tabs show their panels. List views select with clicks, Ctrl, Shift and the keys, and follow their data; trees expand and
collapse by click and keys, with indents by level; tables select body rows only, and the keys go through several tables
in the document's order. Rows rebuilt by code keep the selection given by the view's `value`: all rows rebuilt twice
between two updates, a single row replaced, and rows rebuilt or a `value` set just before a key or a click, after which
Down skips a collapsed parent's children, Right and Left expand and collapse, and Shift+Down, Shift-click and
Ctrl-click extend the rebuilt selection; a drag held past a view's edge scrolls it with `dragscroll` only; splitters
move the boundary in a row and in a column, never below nothing. The color picker picks from its field, its hue strip
and its numbers, draws the old and the new color, and follows its data.
