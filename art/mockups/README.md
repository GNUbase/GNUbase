# UI mockups

Pictures of how GNUbase's user interface should look in the default `gnubase` theme: its colors, fonts, widgets and
visual details. They show the direction, not a pixel-exact specification, and they set only the look: which elements
there are, where they sit, what is shown by default and how they behave stay as the reference manual describes. An
element that a mockup shows and the manual doesn't is not added because of the mockup. The dark, light and high
contrast themes keep their own look.

| File | Shows | Made how, under which license |
|---|---|---|
| `main-window.webp` | The project window in the gnubase theme: toolbar with transport and time display, info line, inspector in the left zone, track list and event display with marker, folder, MIDI and audio tracks, media rack in the right zone, mixer in the lower zone, status line | Added in #1311, so under the project's license (`CONTRIBUTING.md`). How it was made is not recorded yet: its author is to state it here (#1329) |

Origin and license: a mockup is made for GNUbase and holds no third-party material: no screenshots, icons, fonts or
artwork of other applications, and nothing copied from the reference manual. Where it uses free material from elsewhere,
that material's source and license are named in the table. Unless its row names another free license, a mockup is
under the project's license, like everything in the repository (GPL-3.0-or-later, see `README.md` and
`CONTRIBUTING.md`); a contributor who adds one agrees to that, as with code.

Adding a mockup:
- One file per window or panel, named after it in lowercase with hyphens (`mixer-window.webp`, `key-editor.webp`).
- PNG or WebP, stored in Git LFS like all images (`.gitattributes`).
- Add a row to the table above, with how the picture was made (the tool, and whether by hand or generated) and under
  which license it is.
- Make sure it holds no third-party material (see above); if it does, name the material's source and license in the row.
- Use generic feature names in file names and in this list, as everywhere in the repository.
