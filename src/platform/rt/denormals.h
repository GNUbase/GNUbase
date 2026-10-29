// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace gnubase::rt
{
	// The calling thread's floating-point control mode: rounding and the handling of subnormal numbers (MXCSR
	// without its exception flags on x86-64, FPCR on ARM64). WorkerPool runs a job in its caller's mode on every
	// thread.
	uint64_t floatMode() noexcept;
	void setFloatMode(uint64_t mode) noexcept;

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
