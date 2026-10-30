# Fonts

GNUbase bundles its fonts, so text looks the same on every system (#207). They are in `resources/fonts`, whose README
lists their versions, sources and licenses; the code is in `src/ui/fonts.h` and `src/ui/pseudolocale.h`.

- **Families.** `resources/fonts/fonts.json` lists each family with a role and its faces, and the fallback fonts:
  - **Inter** (`ui`): the user interface's text, in Regular, Italic, Medium (500), SemiBold (600) and Bold. It covers
    Latin with its extensions, Greek, Cyrillic and Vietnamese, and most of the symbols a DAW shows (→, ∞, ±, ½, −, ×).
    Its digits are proportional.
  - **JetBrains Mono** (`counter`): time and counter displays, in Regular and Bold. Its digits are all equally wide by
    default, so running numbers don't jitter. Without a text shaper (RmlUi's FreeType engine has none), OpenType
    features such as Inter's tabular figures don't apply, so the counter font's default digits have to be tabular.
  - **Noto Sans Symbols** (fallback): the symbols Inter lacks, such as ♭, ♯ and ♮ for keys and chords, at the size of
    text.
  Themes pick families by name in their style sheets (`font-family: Inter`); code never names a font.
- **Fallback chain.** RmlUi asks the fallback fonts, in the manifest's order, for each character the element's font
  lacks. A fallback font serves every weight. Chinese, Japanese and Korean need fallback fonts of about 20 MB, which
  come with the translations that need them (#172). Scripts that need shaping, such as Arabic, would need a shaping
  engine as well.
- **Loading.** The app finds its resources folder at startup (`app::findResources`): `resources` next to the program,
  as an installation has it, else the source tree's, for builds that run where they were built. `Fonts` reads the
  manifest, checks that every font exists and has the family the manifest names, and loads the families, then the
  fallback fonts, into RmlUi. A missing or broken font stops the app with exit code 3 and says which.
- **Rendering.** FreeType, built from its submodule, rasterizes each glyph at the size it is drawn at in pixels, so text
  is sharp at every dp ratio. GNUbase keeps FreeType's defaults: grayscale antialiasing, and the TrueType hinting
  instructions run by FreeType's interpreter, which hints only vertically (FreeType's "v40"). All bundled fonts are
  hinted TrueType files, so text is hinted the same way on every platform and doesn't depend on the system's font
  settings.
- **Pseudo-localization.** `ui::pseudoLocalize` turns English text into text that still reads as English, with an
  accented look-alike for each ASCII letter, a third longer and in brackets: "Save" becomes "[Šåṽé ~~]".
  Placeholders in braces stay as they are. Text that isn't translated, text that is cut off and missing glyphs show
  at a glance; the localization infrastructure (#52) can offer it as a language.
- **Checks.** `tests/fonts_tests.cpp`:
  - every font in `resources/fonts` is in the manifest, with its license next to it;
  - the families have every character of sample UI text, plain and pseudo-localized, and of pangrams in the
    alphabets of European languages, Greek, Cyrillic and Vietnamese (`Fonts::missing`);
  - the counter font's digits are equally wide.
  `tests/window_tests.cpp` compares screenshots of the fonts at dp ratios 1 and 2 with golden images on every
  platform.
