# SPDX-License-Identifier: GPL-3.0-or-later
# Crops the render spike's OpenGL 3 screenshots of the features document into one image per tile: the references that
# tests/window_tests.cpp compares the SDL_GPU renderer's tiles with (#285). Run it on screenshots from Mesa's OpenGL:
#
#   gnubase-render-spike --backend gl3 --scale 1 --screenshot DIR/gl3-features-1x.png
#   gnubase-render-spike --backend gl3 --scale 2 --screenshot DIR/gl3-features-2x.png
#   python3 reference-tiles.py DIR tests/golden/ui/effects
#
# Layout (spike.rcss): body padding 12dp, tiles 200x130dp with 8dp margins, four a row in the spike's 960dp window.
import sys
from PIL import Image
names = ["shapes", "linear", "radial", "shadow", "blur", "drop", "clip", "mask", "transform", "meters", "waveform",
         "waveform-image"]
source_dir, out_dir = sys.argv[1], sys.argv[2]
for ratio in (1, 2):
    image = Image.open(f"{source_dir}/gl3-features-{ratio}x.png").convert("RGBA")
    for i, name in enumerate(names):
        row, column = divmod(i, 4)
        x, y = 20 + 216 * column, 20 + 146 * row
        box = (x * ratio, y * ratio, (x + 200) * ratio, (y + 130) * ratio)
        image.crop(box).save(f"{out_dir}/{name}-{ratio}x.png")
