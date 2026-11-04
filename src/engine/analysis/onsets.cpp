// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/analysis/onsets.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
		// Seconds: by then the envelopes have fallen by 87 dB, so whatever came before, up to +20 dBFS, is below
		// g_floor.
		constexpr double g_lead = 0.5;

		double decibels(double level)
		{
			return 20.0 * std::log10(std::max(level, 1e-9));
		}
	}

	OnsetDetector::OnsetDetector(double sampleRate, int channels)
		: m_channels(std::max(channels, 1)), m_release(std::exp(-1.0 / (g_release * sampleRate))),
		  m_delay(std::max<int64_t>(1, std::llround(g_riseTime * sampleRate))), m_window(window(sampleRate)),
		  m_last(static_cast<size_t>(m_channels), 0.0F)
	{
		for (Band& band : m_bands)
			band.history.assign(static_cast<size_t>(m_delay + 1), 0.0);
	}

	OnsetDetector::Step OnsetDetector::step(const float* frame)
	{
		// The frame's peak, and that of its difference to the frame before (half of it, so that full scale stays 1).
		std::array<double, 2> levels = {0.0, 0.0};
		for (size_t c = 0; c < m_last.size(); ++c)
		{
			const float sample = frame[c];
			levels[0] = std::max(levels[0], static_cast<double>(std::abs(sample)));
			levels[1] = std::max(levels[1], 0.5 * std::abs(static_cast<double>(sample) - m_last[c]));
			m_last[c] = sample;
		}
		const auto slot = static_cast<size_t>(m_frame % (m_delay + 1));
		Step result;
		result.level = levels[0];
		for (size_t b = 0; b < m_bands.size(); ++b)
		{
			Band& band = m_bands[b];
			result.before[b] = std::max(band.history[slot], g_floor);
			band.envelope = std::max(levels[b], band.envelope * m_release);
			band.history[slot] = band.envelope;
			result.rises[b] = decibels(std::max(band.envelope, g_floor)) - decibels(result.before[b]);
		}
		return result;
	}

	void OnsetDetector::feed(const float* frames, size_t count)
	{
		const auto size = m_delay + 1;
		for (size_t i = 0; i < count; ++i, ++m_frame)
		{
			const Step step = this->step(frames + (i * m_last.size()));
			const double rise = std::max(step.rises[0], step.rises[1]);
			if (m_frame <= m_open)
			{
				Onset& last = m_onsets.back();
				last.peak = std::max(last.peak, step.level);
				last.rise = std::max(last.rise, rise);
				continue;
			}
			if (rise < g_minRise)
				continue;
			// The sound started where the band that rose first went more than 1 dB above where it was.
			const size_t b = step.rises[0] >= step.rises[1] ? 0 : 1;
			int64_t start = m_frame;
			for (int64_t frame = std::max<int64_t>(0, m_frame - m_delay); frame <= m_frame; ++frame)
				if (m_bands[b].history[static_cast<size_t>(frame % size)] > step.before[b] * g_startAbove)
				{
					start = frame;
					break;
				}
			m_onsets.push_back({start, step.level, rise});
			m_open = start + m_window;
		}
	}

	Onset OnsetDetector::measure(double sampleRate, int channels, const float* frames, size_t count, size_t at)
	{
		OnsetDetector detector(sampleRate, channels);
		const size_t width = detector.m_last.size();
		const size_t end = std::min(count, at + static_cast<size_t>(detector.m_window));
		Onset onset;
		onset.frame = static_cast<int64_t>(at);
		onset.rise = -std::numeric_limits<double>::infinity();
		for (size_t i = 0; i < end; ++i, ++detector.m_frame)
		{
			const Step step = detector.step(frames + (i * width));
			if (i < at)
				continue;
			onset.peak = std::max(onset.peak, step.level);
			onset.rise = std::max({onset.rise, step.rises[0], step.rises[1]});
		}
		onset.peak = decibels(onset.peak);
		if (at >= end)
			onset.rise = 0.0; // nothing there
		return onset;
	}

	int64_t OnsetDetector::lead(double sampleRate)
	{
		return std::llround(g_lead * sampleRate);
	}

	int64_t OnsetDetector::window(double sampleRate)
	{
		return std::max<int64_t>(1, std::llround(g_onsetWindow * sampleRate));
	}

	std::vector<Onset> OnsetDetector::finish()
	{
		std::vector<Onset> onsets = m_onsets;
		for (Onset& onset : onsets)
			onset.peak = decibels(onset.peak);
		return onsets;
	}
}
