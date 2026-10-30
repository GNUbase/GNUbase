# SVG graphics

Icons and other UI graphics are SVG files, so they stay sharp at every UI scale and on HiDPI screens (#210). RmlUi's
SVG plugin draws them; it renders through LunaSVG, built from its submodule `ext/lunasvg` with the vector graphics
library PlutoVG that comes with it.

- **Files.** The icons are in `resources/icons`, whose README describes their style and lists them: 24 × 24 dp,
  monochrome, every stroke and fill `currentColor`. Each theme's graphics (buttons, toggles, knobs, faders, ...) are in
  `resources/themes/<theme>`, in the theme's own colors, at the sizes its `manifest.json` gives
  (`tools/skin/generate.py` makes them). LunaSVG draws shapes, paths, gradients, clip paths and masks. GNUbase's
  graphics have no text: LunaSVG is built without loading system fonts, so text would have no font. It is built so
  that an SVG can't make it read other files either: an image inside an SVG must be a `data:` URI.
- **Use.** A document shows a graphic with the `<svg>` element, `<svg src="icons/transport-play.svg"/>`, or as a
  background with the decorator `decorator: svg(file)`. The element's size comes from the style sheet in `dp`, else
  from the file's `width` and `height`, taken as dp. Themes draw their 9-slice skins with `svg-ninepatch`
  ([themes.md](themes.md)). A decorator finds its file relative to the style sheet that names it, or, in a `style`
  attribute, relative to the document.
- **Rasterizing.** LunaSVG rasterizes a graphic at the size it has on screen in pixels: a 24 dp icon is 24 pixels at
  100 %, 36 at 150 % and 48 at 200 %, whatever the reason for the dp ratio (the monitor's scale, the user's UI scale).
  When the size changes, RmlUi lays the element out again and the graphic is rasterized again at the new size.
- **Cache.** The SVG plugin keeps one parsed document per file, one raster per file and pixel size, and one quad per
  file, size and color. Every element showing the same file at the same size shares them, and a raster is made the
  first time it is drawn. Steady frames rasterize nothing, however many icons are on screen; a raster that no element
  uses any more is released.
- **Theme colors.** The quad that draws a raster has the element's `image-color` (times its `opacity`), and the GPU
  multiplies the raster by it. LunaSVG draws `currentColor` black by default, which no color can change, so GNUbase's
  RmlUi fork makes it draw `currentColor` white, unless the SVG sets its `color` itself. A monochrome icon then shows in
  its `image-color`: a theme gives icons their colors in its style sheets (`image-color: #e8eaed`), and states such as
  `:hover` or `.muted` change the color without a new raster. Graphics with colors of their own keep them as long as
  `image-color` stays white, its default.
- **Checks.** `tests/svg_tests.cpp`, on the fake window system, whose renderer keeps the textures RmlUi generates:
  - every icon and theme graphic loads through LunaSVG and rasterizes at its size, with something to see;
  - an icon is rasterized at its pixel size at 100 %, 150 % and 200 %, and so is a graphic drawn as a decorator;
  - icons rasterize `currentColor` white, while graphics with their own colors, or with their own `color`, keep them;
  - a decorator finds its file next to the style sheet that names it, and a 9-slice skin keeps its corners at their
    size and is rasterized once per scale ([themes.md](themes.md));
  - 672 icons on screen (every icon in three sizes and four colors) are rasterized once per icon and size; 100 steady
    frames and a change of color rasterize nothing, a new UI scale rasterizes each again once and releases the old
    rasters.

  `tests/window_tests.cpp` draws icons in several sizes and colors and theme graphics through SDL_GPU at 100 %, 150 %
  and 200 % UI scale, compares them with golden images on every platform, and checks that a solid icon shows exactly
  its `image-color`.
