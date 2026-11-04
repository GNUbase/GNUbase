# Accessibility

GNUbase's accessibility baseline (#60): the core dialogs work with the keyboard alone, the control with the keyboard
focus shows it in every theme, the UI scales, a high contrast theme is one setting away, and a spike on screen readers
says how each OS can get to the UI. Everything here is reachable through the engine API and MCP like the rest of the
UI: keys through MCP's `ui_key` (`ui::Automation::key`), the theme and the scale through `settings.set`.

## Keyboard

- **Dialogs.** Tab and Shift+Tab move the focus between a dialog's controls in document order, around the ends; a
  dialog is a modal document of its own, so the focus stays in it. Buttons, text fields, check boxes, radio buttons,
  drop-down lists, sliders, list views and value fields take the focus (`tab-index: auto` in
  `resources/styles/base.rcss`); disabled ones don't. A dialog opens with the focus on its `autofocus` control, such as
  Save as Template's name field, or else on the document, so that the first Tab goes to its first control.
- **Keys on the focused control.** Space and Enter click a button and Space toggles a check box (RmlUi). A drop-down
  list opens and closes with Enter, its arrows step through the options while it is open, and Escape closes it without
  choosing. The arrow keys move a list view's selection and step knobs, faders, sliders and value fields; Enter edits a
  value field as text, Enter again takes the number ([widgets.md](widgets.md)).
- **Enter and Escape.** Enter clicks the dialog's primary button from a text field, a check box or a list, but a
  focused button clicks itself, so Enter on Cancel cancels. Escape closes an open drop-down list or menu first, then the
  dialog with the result `cancel` (`ui::Dialog` in `src/ui/popupwidgets.cpp`).
- **Docking windows.** Tab and Shift+Tab move the focus between the zones of the project window, as in the reference
  (#379, [docking.md](docking.md)), and the zone with the focus is framed. The menus take Up, Down and Enter once open;
  moving between the menus of the menu bar with the keyboard and opening them with Alt are #445, the setup panes #460.
- **Key commands.** Every key command can have keys of its own ([key-commands.md](key-commands.md)), also commands
  without default keys such as `project.addTrack`, which opens the add track dialog.

## Visible focus

RmlUi gives a control the pseudo-class `:focus-visible` when the keyboard focused it, not the mouse. Every theme draws
its focus ring (`focus-ring.svg`) over the control's own skin for each of its states, so a checked toggle or check box
still looks checked while it has the focus. `tools/skin/generate.py` makes these rules: each rule that gives a
focusable widget a skin gets a `:focus-visible` twin with the ring added, after all other rules so that the ring also
shows under the mouse. Knobs, faders and sliders, whose parts have the skins, get the ring around them; text fields
and drop-down lists show their focus skin; menu items light up as under the mouse.

## Scaling

The user setting `ui.scale` scales the whole UI on top of each monitor's own scale ([settings.md](settings.md),
[themes.md](themes.md)): documents are laid out in `dp` and the skins are SVGs rasterized at each scale, so text and
graphics stay sharp at 200 % and more.

## High contrast

The theme `high-contrast` (`ui.theme`, [themes.md](themes.md)) has white text and borders on black, yellow and cyan
accents, and a focus ring twice as thick as the other themes'. It comes from `tools/skin/generate.py` like the other
themes. Its text colors, accents and track colors meet WCAG 2's enhanced contrast of 7:1 on its backgrounds, and its
borders at least 3:1 (WCAG 1.4.6 and 1.4.11).

## Screen readers

GNUbase draws its UI itself with RmlUi, so no OS sees its controls until GNUbase describes them. The spike's findings,
per OS: [Windows](screen-readers-windows.md), [macOS](screen-readers-macos.md), [Linux](screen-readers-linux.md). In
short: one cross-platform library, AccessKit, speaks each OS's accessibility API and fits GNUbase best; the
accessibility tree comes from the RmlUi documents on the UI thread, much as `ui::Automation::dom` already reads them
for MCP. Implementing it is a ticket of its own.

## Not yet

A screen reader implementation (above); the menu bar and the setup panes with the keyboard (#445, #460); keyboard
focus in the zones' own panels beyond the zone, such as moving between the tracks of the track list, which come with
those panels; a focus ring outside a control's box rather than on its edge.

## Checks

- `tests/accessibility_tests.cpp` drives the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) with keys only: the
  add track dialog through all its kinds of controls, with Tab around the ends both ways, a drop-down list opened,
  stepped and closed, and Escape closing an open list before the dialog, adding tracks with Enter on the focused
  button; Unsaved Changes cancelled with Enter on the focused Cancel and discarded with Space on Discard; Save as
  Template named in its focused field and saved with Enter; and the start screen creating a project from a template.
  The focused controls show their focus.
- `tests/theme_tests.cpp`: every theme draws the focus ring for each focusable widget in each of its states, and the
  high contrast theme's colors meet the contrast ratios above.
- `tests/window_tests.cpp`: the high contrast theme's page of widgets against its golden image.
