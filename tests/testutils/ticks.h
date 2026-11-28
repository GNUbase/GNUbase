// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace gnubase::test
{
	// Ticks for a number of samples at the default 120 BPM and 48 kHz: 1792 ticks per 25 samples (1,720,320 to the
	// quarter note of 24000 samples), exact for multiples of 25 samples.
	inline int64_t ticks(int64_t samples)
	{
		REQUIRE(samples % 25 == 0);
		return samples / 25 * 1792;
	}
}
