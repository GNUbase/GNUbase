// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace gnubase::ui
{
	// Pseudo-localization (docs/features/fonts.md): UTF-8 text that still reads as English, with an accented look-alike
	// for each ASCII letter, a third longer and in brackets, so that text that isn't translated, text that is cut off
	// and missing glyphs show at a glance. Placeholders in braces, such as {name}, stay as they are; so does empty
	// text.
	std::string pseudoLocalize(std::string_view text);
}
