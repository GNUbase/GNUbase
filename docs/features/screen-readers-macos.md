# Screen readers on macOS

Findings of the screen reader spike (#60, [accessibility.md](accessibility.md)): how VoiceOver can read and drive
GNUbase's UI on macOS. Nothing here is implemented yet.

## The OS's side

- **API.** AppKit's NSAccessibility protocol. A view that draws its own controls returns them as accessibility
  elements (`NSAccessibilityElement` objects) from `accessibilityChildren`; each has a role (`NSAccessibilityButtonRole`,
  `CheckBoxRole`, `PopUpButtonRole`, `ListRole`, `RowRole`, `TextFieldRole`, `SliderRole`, `MenuItemRole`, ...), a
  label, a value, an enabled state, a frame in screen coordinates and a parent.
- **Actions** are methods on the element: `accessibilityPerformPress`, `accessibilityPerformIncrement` and
  `accessibilityPerformDecrement` for knobs and faders, `setAccessibilityValue`, `setAccessibilityFocused`.
- **Notifications.** `NSAccessibilityPostNotification` with `NSAccessibilityFocusedUIElementChangedNotification`,
  `ValueChangedNotification`, `LayoutChangedNotification` and the like tells VoiceOver what changed.
- **Threads.** AppKit asks on the main thread; GNUbase's UI thread is the main thread on macOS, as SDL requires.
- **Coordinates.** Screen coordinates start at the bottom left of the main screen, in points, not pixels: a border box
  in window pixels is divided by the window's backing scale and flipped.

## Getting there from SDL3 and RmlUi

- SDL3 gives each window's `NSWindow` (`SDL_PROP_WINDOW_COCOA_WINDOW_POINTER`), whose content view is SDL's. That view
  has to answer the accessibility methods, so it is subclassed at run time or an accessibility element is put in front
  of it.
- The tree comes from the RmlUi documents, as on Windows ([screen-readers-windows.md](screen-readers-windows.md)):
  roles from tags and classes, labels from text, `title` attributes and `<label for>`, states from attributes and
  pseudo-classes, frames from border boxes. Press is `Element::Click`, increment and decrement step value widgets as
  their arrow keys do.
- The menu bar is GNUbase's own until the system menu bar comes (main-menu.md); once it does, VoiceOver reads it as
  AppKit's.

## Options

1. **AccessKit** (MIT or Apache-2.0). Its macOS adapter implements NSAccessibility from the tree the application
   pushes, and has a subclassing adapter for a view another library created, as SDL's is. It has to be built for both
   architectures of GNUbase's universal binary.
2. **NSAccessibility of our own** in Objective-C++: no new dependency, macOS only.

**Recommendation:** AccessKit, as on the other OSes. Tools to check it: VoiceOver (Cmd+F5) and Xcode's Accessibility
Inspector.
