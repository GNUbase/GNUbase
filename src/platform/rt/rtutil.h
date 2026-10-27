// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace gnubase::rt::detail
{
	// Data written by different threads is kept this far apart to avoid false sharing.
	inline constexpr size_t g_cacheLineSize = 64;

	constexpr size_t roundUpToPowerOfTwo(size_t value)
	{
		size_t result = 1;
		while (result < value)
			result <<= 1;
		return result;
	}
}
