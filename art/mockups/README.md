# UI mockups

Pictures of how GNUbase's user interface should look in the default `gnubase` theme: its colors, fonts, widgets and
visual details. They show the direction, not a pixel-exact specification, and they set only the look: which elements
there are, where they sit, what is shown by default and how they behave stay as the reference manual describes. An
element that a mockup shows and the manual doesn't is not added because of the mockup. The dark, light and high
contrast themes keep their own look.

| File | Shows |
|---|---|
| `main-window.webp` | The project window in the gnubase theme: toolbar with transport and time display, info line, inspector in the left zone, track list and event display with marker, folder, MIDI and audio tracks, media rack in the right zone, mixer in the lower zone, status line |

Adding a mockup:
- One file per window or panel, named after it in lowercase with hyphens (`mixer-window.webp`, `key-editor.webp`).
- PNG or WebP, stored in Git LFS like all images (`.gitattributes`).
- Add a row to the table above.
- Use generic feature names in file names and in this list, as everywhere in the repository.
