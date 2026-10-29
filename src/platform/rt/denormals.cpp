// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/rt/denormals.h"

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>

namespace
{
	constexpr uint64_t g_flushToZero = 0x8000;    // MXCSR FTZ
	constexpr uint64_t g_denormalsAreZero = 0x40; // MXCSR DAZ
	constexpr uint64_t g_disableDenormals = g_flushToZero | g_denormalsAreZero;
	constexpr uint64_t g_statusBits = 0x3F; // MXCSR IE, DE, ZE, OE, UE, PE: sticky exception flags, not control

	uint64_t readMode() noexcept
	{
		return _mm_getcsr();
	}

	void writeMode(uint64_t mode) noexcept
	{
		_mm_setcsr(static_cast<unsigned int>(mode));
	}
}
#elif defined(_M_ARM64)
#include <intrin.h>

namespace
{
	constexpr uint64_t g_disableDenormals = uint64_t{1} << 24; // FPCR FZ
	constexpr uint64_t g_statusBits = 0;                       // FPCR holds control bits only
	constexpr int g_fpcr = 0x5A20;                             // FPCR system register, ARM64_SYSREG(3, 3, 4, 4, 0)

	uint64_t readMode() noexcept
	{
		return static_cast<uint64_t>(_ReadStatusReg(g_fpcr));
	}

	void writeMode(uint64_t mode) noexcept
	{
		_WriteStatusReg(g_fpcr, static_cast<__int64>(mode));
	}
}
#elif defined(__aarch64__)
namespace
{
	constexpr uint64_t g_disableDenormals = uint64_t{1} << 24; // FPCR FZ
	constexpr uint64_t g_statusBits = 0;                       // FPCR holds control bits only

	uint64_t readMode() noexcept
	{
		uint64_t mode = 0;
		asm volatile("mrs %0, fpcr" : "=r"(mode));
		return mode;
	}

	void writeMode(uint64_t mode) noexcept
	{
		asm volatile("msr fpcr, %0" : : "r"(mode));
	}
}
#else
#error "ScopedDenormalsDisabled: unsupported CPU architecture"
#endif

namespace gnubase::rt
{
	uint64_t floatMode() noexcept
	{
		return readMode() & ~g_statusBits;
	}

	void setFloatMode(uint64_t mode) noexcept
	{
		writeMode(mode);
	}

	ScopedDenormalsDisabled::ScopedDenormalsDisabled() noexcept : m_previousMode(readMode())
	{
		writeMode(m_previousMode | g_disableDenormals);
	}

	ScopedDenormalsDisabled::~ScopedDenormalsDisabled()
	{
		writeMode(m_previousMode);
	}
}
