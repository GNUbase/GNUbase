# UI mockups

Pictures of how GNUbase's user interface should look: the target for the default theme's layout, colors and widgets.
They show the direction, not a pixel-exact specification. Where a mockup and the reference manual differ in what a
panel contains or does, the manual decides; where they differ in how it looks, the mockup does.

| File | Shows |
|---|---|
| `main-window.webp` | The project window with the default theme: toolbar with transport and time display, info line, inspector in the left zone, track list and event display with marker, folder, MIDI and audio tracks, media rack in the right zone, mixer in the lower zone, status line |

Adding a mockup:
- One file per window or panel, named after it in lowercase with hyphens (`mixer-window.webp`, `key-editor.webp`).
- PNG or WebP, stored in Git LFS like all images (`.gitattributes`).
- Add a row to the table above.
- Use generic feature names in file names and in this list, as everywhere in the repository.
