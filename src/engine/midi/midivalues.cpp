// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/midivalues.h"

namespace gnubase::engine::midi
{
	uint32_t scaleUp(uint32_t value, int fromBits, int toBits)
	{
		// Values up to the center shift; above it the bits below the top one repeat to fill the new low bits, so the
		// maximum reaches the new maximum.
		const int scaleBits = toBits - fromBits;
		const uint64_t shifted = static_cast<uint64_t>(value) << scaleBits;
		const uint32_t center = 1U << (fromBits - 1);
		if (value <= center)
			return static_cast<uint32_t>(shifted);
		const int repeatBits = fromBits - 1;
		uint64_t repeat = value & ((1U << repeatBits) - 1U);
		repeat = scaleBits > repeatBits ? repeat << (scaleBits - repeatBits) : repeat >> (repeatBits - scaleBits);
		uint64_t result = shifted;
		while (repeat != 0)
		{
			result |= repeat;
			repeat >>= repeatBits;
		}
		return static_cast<uint32_t>(result);
	}

	uint32_t scaleDown(uint32_t value, int fromBits, int toBits)
	{
		return value >> (fromBits - toBits);
	}
}
