# Screen readers on Windows

Findings of the screen reader spike (#60, [accessibility.md](accessibility.md)): how Narrator, NVDA and JAWS can read
and drive GNUbase's UI on Windows. Nothing here is implemented yet.

## The OS's side

- **API.** UI Automation (UIA) is the accessibility API of current Windows; Narrator uses only UIA, NVDA and JAWS use
  UIA and, for older applications, MSAA (`IAccessible`). A new provider implements UIA only.
- **Entry point.** A screen reader asks a window for its accessibility root with the message `WM_GETOBJECT`; the
  provider answers `UiaRootObjectId` with `UiaReturnRawElementProvider` and its root `IRawElementProviderSimple`.
  Each control is a fragment (`IRawElementProviderFragment`) with a parent, children, a bounding rectangle in screen
  pixels and properties: control type (button, check box, combo box, list, list item, edit, slider, menu item, window),
  name, enabled, focused, keyboard focusable. Patterns add what the control does: Invoke (buttons), Toggle (check
  boxes, toggle buttons), Value and RangeValue (text fields, knobs, faders, value fields), Selection and
  SelectionItem (lists), ExpandCollapse (drop-down lists, tree rows, menus).
- **Events.** The provider raises `UIA_AutomationFocusChangedEventId` when the focus moves and property-changed events
  for toggles, values and names, otherwise a screen reader reads stale state.
- **Threads.** UIA calls providers from its own threads, not only the window's thread.

## Getting there from SDL3 and RmlUi

- SDL3 gives each window's `HWND` (`SDL_PROP_WINDOW_WIN32_HWND_POINTER` in the window's properties).
  `WM_GETOBJECT` is sent, not posted, so SDL's message hook (`SDL_SetWindowsMessageHook`), which sees queued messages,
  never gets it: the window procedure has to be subclassed (`SetWindowSubclass`) to answer it.
- The tree comes from the RmlUi documents of the window, as `ui::Automation::dom` reads them for MCP: tag, id,
  attributes, form control values, pseudo-classes (`:checked`, `:disabled`, `:focus`) and border boxes in window
  pixels, which become screen pixels with the window's position. Roles follow from tags and classes (`button`,
  `input.checkbox`, `select`, `listview` with `listitem`, `menuitem`, `valuefield`, `knob`, `fader`, `dialog`); names
  from the text, a `title` attribute (icon-only buttons have one for their tooltip) or a `<label for>`.
- Actions map onto what keys and clicks already do: Invoke is `Element::Click`, focus is `Element::Focus`, values go
  through the widgets' `change` events, so the engine binding sees the same commands as from the mouse.
- The event display, a canvas, has no elements per track or event; reading it needs a model of its own, later.

## Options

1. **AccessKit** (MIT or Apache-2.0, so GPLv3-compatible), a cross-platform accessibility library in Rust with C
   bindings. Its Windows adapter implements UIA, including the threading, from a tree the application pushes; its
   subclassing adapter attaches to a window that another library created, as SDL's are. The same tree serves macOS
   and Linux. The cost is a Rust build, or prebuilt libraries, for every platform GNUbase builds for, Windows ARM64
   included.
2. **A UIA provider of our own** in C++/COM: no new dependency, but several thousand lines for the patterns and the
   threading, for Windows only.

**Recommendation:** AccessKit for all three OSes, with the tree built from the RmlUi documents on the UI thread and
updated when documents change, focus moves or values change. Tools to check it: Narrator, NVDA (free), Accessibility
Insights for Windows and the SDK's Inspect.
