# Widgets

GNUbase's widget library (#54): the controls that documents use, with the behaviour users know from the reference
(reference manual pp. 118, 1598 and 1618–1621). The code is in `src/ui/widgets.h`; `app::AppUi` registers the widgets,
so every document in the app and in UI tests has them.

- **Look.** Widgets only change their state: attributes, pseudo-classes such as `:disabled`, and state classes such as
  `.checked`. The shared style sheets in `resources/styles` lay them out and the active theme draws each state from its
  skins ([themes.md](themes.md)), so no widget sets a color, font or skin. Code sets only geometry from data, such as
  where a fader's cap sits.
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

## Checks

`tests/widgets_tests.cpp` drives each widget in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) through
`ui::Automation`: buttons clicked with the mouse and the keyboard, disabled buttons that ignore clicks, and toggle
buttons that turn on and off, look different when on, and follow a data model.
