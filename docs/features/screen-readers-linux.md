# Screen readers on Linux

Findings of the screen reader spike (#60, [accessibility.md](accessibility.md)): how Orca can read and drive GNUbase's
UI on Linux. Nothing here is implemented yet.

## The OS's side

- **API.** AT-SPI2, D-Bus interfaces on the accessibility bus, whose address the session bus's `org.a11y.Bus` gives.
  The application registers its root with the registry and exports an object per control with the interfaces
  `Accessible` (role, name, states, parent and children), `Component` (extents, focus), `Action` (press), `Value`
  (knobs, faders, value fields), `Text` and `EditableText` (text fields) and `Selection` (lists).
- **Toolkits.** GTK reaches AT-SPI through ATK and its bridge, Qt implements the D-Bus interfaces itself. An application
  without either can do the same as Qt; ATK would add a GTK-era dependency for nothing.
- **Events.** Signals such as `Object:StateChanged:focused`, `Object:PropertyChange` and `Object:ChildrenChanged` tell
  Orca what changed.
- **Display servers.** AT-SPI doesn't depend on X11 or Wayland, but on Wayland an application doesn't know where its
  window is on the screen, so extents in screen coordinates are only right relative to the window; Orca copes, screen
  magnifiers less so.
- **Sandboxes.** A Flatpak needs access to `org.a11y.Bus`.

## Getting there from SDL3 and RmlUi

- Nothing in SDL3 is needed but the windows' positions, where X11 gives them; the D-Bus connection runs on a thread of
  its own and answers from a copy of the tree that the UI thread updates.
- The tree comes from the RmlUi documents, as on Windows ([screen-readers-windows.md](screen-readers-windows.md)):
  roles from tags and classes, names from text, `title` attributes and `<label for>`, states from attributes and
  pseudo-classes, extents from border boxes. Press is `Element::Click`; values go through the widgets' `change`
  events.

## Options

1. **AccessKit** (MIT or Apache-2.0). Its Unix adapter implements AT-SPI directly over D-Bus, without ATK or GTK, from
   the tree the application pushes, on its own thread. It needs a session bus at run time and nothing else.
2. **AT-SPI of our own** over a D-Bus library such as sd-bus or libdbus: no Rust, but a large surface for Linux only.

**Recommendation:** AccessKit, as on the other OSes; when no accessibility bus is running it costs nothing. Tools to
check it: Orca and Accerciser.
