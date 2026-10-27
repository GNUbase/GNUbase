// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace gnubase::rt
{
	// Treats subnormal floats as zero (flush to zero, denormals are zero) for the lifetime of the object and restores
	// the previous mode afterwards. Use it around DSP code: subnormal numbers can make processing many times slower.
	class ScopedDenormalsDisabled
	{
	public:
		ScopedDenormalsDisabled() noexcept;
		~ScopedDenormalsDisabled();
		ScopedDenormalsDisabled(const ScopedDenormalsDisabled&) = delete;
		ScopedDenormalsDisabled& operator=(const ScopedDenormalsDisabled&) = delete;

	private:
		uint64_t m_previousMode;
	};
}
