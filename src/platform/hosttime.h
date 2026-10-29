// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace gnubase
{
	// Nanoseconds of the host's monotonic clock. Audio blocks and MIDI messages are stamped with it, so MIDI can be
	// placed in blocks. Backends convert their driver's timestamps to it.
	using HostTime = int64_t;

	inline HostTime hostTime() noexcept
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
			.count();
	}
}
