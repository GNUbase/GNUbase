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

## Checks

`tests/widgets_tests.cpp` drives each widget in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)) through
`ui::Automation`: buttons clicked with the mouse and the keyboard, disabled buttons that ignore clicks, and toggle
buttons that turn on and off, look different when on, and follow a data model. Knobs, faders, sliders and value
fields are dragged (also with Shift), turned with the wheel, stepped with the arrow keys and reset with Ctrl-click,
their parts placed where the value says; data-value binds them both ways; the wheel turns them instead of scrolling
the page; value fields take typed numbers and leave bad ones. Meters show their level and peak at the right heights.
