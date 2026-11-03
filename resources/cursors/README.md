# GNUbase mouse pointers

Style: 24 × 24 viewBox and size, the shapes of the matching tool icons in `resources/icons`, drawn twice: a 4 px white outline under 1.75 px black lines, rounded, no fill, so that a pointer shows on light and dark backgrounds alike. Each file draws its shapes once in `<defs>` and uses them for both layers.

Style sheets name a pointer with RmlUi's `cursor` property, `cursor: tool-draw;` ([docs/features/svg.md](../../docs/features/svg.md)). `cursors.json` gives each one's hotspot, the point it points with, in the units of the SVG's `width` and `height` (pixels at 100 %); a pointer needs an entry there.

- `resize.svg` — Resize an event at its edge (hotspot in the middle).
- `tool-draw.svg` — Draw tool (the pencil's tip).
- `tool-erase.svg` — Erase tool (the eraser's lower edge).
- `tool-glue.svg` — Glue tool (the middle).
- `tool-line.svg` — Line tool (the line's start).
- `tool-mute.svg` — Mute tool (the middle).
- `tool-play.svg` — Play tool (the speaker).
- `tool-range.svg` — Range selection tool (the middle of the I-beam).
- `tool-split.svg` — Split tool (where the blades cross).
- `tool-zoom.svg` — Zoom tool (the lens's middle).
