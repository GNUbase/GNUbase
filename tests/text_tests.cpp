// SPDX-License-Identifier: GPL-3.0-or-later
// Text helpers (src/platform/text.h), which the engine and the UI share.
#include "platform/text.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
	TEST_CASE("asciiLower folds ASCII letters only and keeps UTF-8 as it is", "[unit]")
	{
		using gnubase::text::asciiLower;
		CHECK(asciiLower("Song.DAWPROJECT") == "song.dawproject");
		CHECK(asciiLower("AZaz@[`{09 -_") == "azaz@[`{09 -_");
		CHECK(asciiLower("").empty());
		// Non-ASCII letters, and the bytes of their UTF-8, which std::tolower may change in a locale such as Latin-1.
		CHECK(asciiLower(u8"ÜBUNG Ä") == u8"Übung Ä");
		const std::string bytes = {'\xc3', '\x9c', '\xc4', 'A', '\x80', '\xff'};
		CHECK(asciiLower(bytes) == std::string{'\xc3', '\x9c', '\xc4', 'a', '\x80', '\xff'});
	}
}
