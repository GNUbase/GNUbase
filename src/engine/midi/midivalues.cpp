// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/midivalues.h"

#include <algorithm>
#include <cmath>

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

	namespace
	{
		constexpr double g_center = g_pitchBendCenter;
		constexpr double g_maximum = 4294967295.0;

		uint32_t rounded(double value)
		{
			return static_cast<uint32_t>(std::llround(std::clamp(value, 0.0, g_maximum)));
		}
	}

	double bendToSemitones(uint32_t bend, double range)
	{
		return (static_cast<double>(bend) - g_center) / g_center * range;
	}

	uint32_t semitonesToBend(double semitones, double range)
	{
		return rounded(g_center + (semitones / range * g_center));
	}

	double controllerToUnit(uint32_t value)
	{
		return static_cast<double>(value) / g_maximum;
	}

	uint32_t controllerFromUnit(double value)
	{
		return rounded(value * g_maximum);
	}
}
