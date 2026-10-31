// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/performance.h"

#include <algorithm>

namespace gnubase::engine
{
	namespace
	{
		constexpr double g_averageSeconds = 0.3; // how long the load average takes to follow a change
	}

	void PerformanceMeter::cycle(
		double busySeconds, double blockSeconds, const double* threadSeconds, size_t threads) noexcept
	{
		if (blockSeconds <= 0.0)
			return;
		const double load = busySeconds / blockSeconds;
		const double weight = std::min(1.0, blockSeconds / g_averageSeconds);
		m_data.load += (load - m_data.load) * weight;
		m_data.peak = std::max(m_data.peak, load);
		if (load > 1.0)
			++m_data.overloads;
		++m_data.cycles;

		m_data.threads = std::min(threads, PerformanceData::g_maxThreads);
		for (size_t t = 0; t < m_data.threads; ++t)
			m_data.threadLoad[t] += ((threadSeconds[t] / blockSeconds) - m_data.threadLoad[t]) * weight;
	}

	void PerformanceMeter::reset() noexcept
	{
		m_data.peak = 0.0;
		m_data.overloads = 0;
		m_data.cycles = 0;
	}

	void PerformanceMeter::resume(const PerformanceData& earlier) noexcept
	{
		m_data.peak = earlier.peak;
		m_data.overloads = earlier.overloads;
		m_data.cycles = earlier.cycles;
	}
}
