// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gnubase::engine
{
	// What the performance monitor shows (#24, #45). Loads are the share of a block's duration spent processing it:
	// 1 means processing took exactly as long as the block plays, above 1 the device would drop out.
	struct PerformanceData
	{
		static constexpr size_t g_maxThreads = 64;

		double load = 0.0;                             // the processing load, averaged over about a third of a second
		double peak = 0.0;                             // the highest load of a single block since the last reset
		uint64_t overloads = 0;                        // blocks with a load above 1 since the last reset
		uint64_t cycles = 0;                           // blocks processed since the last reset
		size_t threads = 0;                            // processing threads, the audio thread first
		std::array<double, g_maxThreads> threadLoad{}; // per thread, averaged like load
	};

	// Turns the time of every processed block into PerformanceData. Audio thread; nothing allocates.
	class PerformanceMeter
	{
	public:
		// One block: how long processing took, how long the block plays, and how long each thread worked on it.
		void cycle(double busySeconds, double blockSeconds, const double* threadSeconds, size_t threads) noexcept;

		// Starts a new measurement: clears the peak, the overloads and the cycle count.
		void reset() noexcept;

		// Goes on with the peak, the overloads and the cycle count of an earlier measurement, e.g. of the device
		// before it restarted.
		void resume(const PerformanceData& earlier) noexcept;

		const PerformanceData& data() const { return m_data; }

	private:
		PerformanceData m_data;
	};
}
