// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/analysis/onsets.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::analysis
{
	namespace
	{
		constexpr double g_floor = 0.001;      // -60 dBFS: rises are measured from at least here
		constexpr double g_minRise = 6.0;      // dB
		constexpr double g_startAbove = 1.122; // 1 dB: where the rise starts
		constexpr double g_release = 0.05;     // seconds, the envelopes' time constant
		constexpr double g_riseTime = 0.005;   // seconds
		constexpr double g_onsetWindow = 0.02; // seconds

		double decibels(double level)
		{
			return 20.0 * std::log10(std::max(level, 1e-9));
		}
	}

	OnsetDetector::OnsetDetector(double sampleRate, int channels)
		: m_channels(std::max(channels, 1)), m_release(std::exp(-1.0 / (g_release * sampleRate))),
		  m_delay(std::max<int64_t>(1, std::llround(g_riseTime * sampleRate))),
		  m_window(std::max<int64_t>(1, std::llround(g_onsetWindow * sampleRate))),
		  m_last(static_cast<size_t>(m_channels), 0.0F)
	{
		for (Band& band : m_bands)
			band.history.assign(static_cast<size_t>(m_delay + 1), 0.0);
	}

	void OnsetDetector::feed(const float* frames, size_t count)
	{
		const auto size = m_delay + 1;
		for (size_t i = 0; i < count; ++i, ++m_frame)
		{
			// The frame's peak, and that of its difference to the frame before (half of it, so that full scale
			// stays 1).
			std::array<double, 2> levels = {0.0, 0.0};
			for (size_t c = 0; c < m_last.size(); ++c)
			{
				const float sample = frames[(i * m_last.size()) + c];
				levels[0] = std::max(levels[0], static_cast<double>(std::abs(sample)));
				levels[1] = std::max(levels[1], 0.5 * std::abs(static_cast<double>(sample) - m_last[c]));
				m_last[c] = sample;
			}
			const auto slot = static_cast<size_t>(m_frame % size);
			std::array<double, 2> before = {0.0, 0.0}; // the envelopes m_delay + 1 frames ago
			std::array<double, 2> rises = {0.0, 0.0};
			for (size_t b = 0; b < m_bands.size(); ++b)
			{
				Band& band = m_bands[b];
				before[b] = std::max(band.history[slot], g_floor);
				band.envelope = std::max(levels[b], band.envelope * m_release);
				band.history[slot] = band.envelope;
				rises[b] = decibels(std::max(band.envelope, g_floor)) - decibels(before[b]);
			}
			const double rise = std::max(rises[0], rises[1]);
			if (m_frame <= m_open)
			{
				Onset& last = m_onsets.back();
				last.peak = std::max(last.peak, levels[0]);
				last.rise = std::max(last.rise, rise);
				continue;
			}
			if (rise < g_minRise)
				continue;
			// The sound started where the band that rose first went more than 1 dB above where it was.
			const size_t b = rises[0] >= rises[1] ? 0 : 1;
			int64_t start = m_frame;
			for (int64_t frame = std::max<int64_t>(0, m_frame - m_delay); frame <= m_frame; ++frame)
				if (m_bands[b].history[static_cast<size_t>(frame % size)] > before[b] * g_startAbove)
				{
					start = frame;
					break;
				}
			m_onsets.push_back({start, levels[0], rise});
			m_open = start + m_window;
		}
	}

	std::vector<Onset> OnsetDetector::finish()
	{
		std::vector<Onset> onsets = m_onsets;
		for (Onset& onset : onsets)
			onset.peak = decibels(onset.peak);
		return onsets;
	}
}
