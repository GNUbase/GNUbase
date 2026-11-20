// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/analysis/statistics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gnubase::engine::analysis
{
	namespace
	{
		constexpr double g_hold = 0.020;    // seconds below the close threshold that close a section
		constexpr double g_rmsPart = 0.010; // seconds: Max. RMS windows start this far apart
		constexpr int g_mantissaBits = 24;  // a float's
		constexpr int g_floatDepth = 32;

		double linear(double decibels)
		{
			return std::pow(10.0, decibels / 20.0);
		}

		int64_t frames(double seconds, double sampleRate)
		{
			return std::max<int64_t>(0, std::llround(seconds * sampleRate));
		}

		// The bits a sample needs: the fewest b for which sample * 2^(b - 1) is a whole number, 32 beyond 24.
		int bitsOf(double sample)
		{
			int exponent = 0;
			const double mantissa = std::frexp(std::abs(sample), &exponent); // from 0.5 to 1
			auto bits = static_cast<uint32_t>(std::ldexp(mantissa, g_mantissaBits));
			int zeros = 0;
			for (; (bits & 1U) == 0 && zeros < g_mantissaBits; bits >>= 1U)
				++zeros;
			const int needed = g_mantissaBits + 1 - exponent - zeros;
			return needed > g_mantissaBits ? g_floatDepth : std::max(1, needed);
		}
	}

	SilenceDetector::SilenceDetector(double sampleRate, int channels, const SilenceSettings& settings)
		: m_sampleRate(sampleRate), m_channels(std::max(channels, 1)), m_settings(settings),
		  m_open(linear(settings.openThreshold)), m_close(linear(settings.closeThreshold)),
		  m_hold(frames(g_hold, sampleRate)), m_minimumOpen(frames(settings.minimumOpen, sampleRate)),
		  m_minimumClosed(frames(settings.minimumClosed, sampleRate))
	{
	}

	void SilenceDetector::feed(const float* frames, size_t count)
	{
		const auto channels = static_cast<size_t>(m_channels);
		for (size_t i = 0; i < count; ++i, ++m_frame)
		{
			double level = 0.0;
			for (size_t c = 0; c < channels; ++c)
				level = std::max(level, static_cast<double>(std::abs(frames[(i * channels) + c])));
			if (m_start < 0)
			{
				if (level >= m_open && m_frame >= m_reopen)
					m_start = m_last = m_frame;
			}
			else if (level >= m_close)
			{
				m_last = m_frame;
			}
			else if (m_frame - m_last > m_hold && m_frame >= m_start + m_minimumOpen)
			{
				close(m_frame);
			}
		}
	}

	void SilenceDetector::close(int64_t limit)
	{
		const int64_t end = std::min(limit, std::max(m_last + 1, m_start + m_minimumOpen));
		m_sections.push_back({m_start, end});
		m_reopen = end + m_minimumClosed;
		m_start = -1;
	}

	std::vector<Section> SilenceDetector::finish()
	{
		if (m_start >= 0)
			close(m_frame);

		std::vector<Section> result;
		const int64_t preRoll = frames(m_settings.preRoll, m_sampleRate);
		const int64_t postRoll = frames(m_settings.postRoll, m_sampleRate);
		for (const Section& section : m_sections)
		{
			const Section wide = {
				std::max<int64_t>(0, section.start - preRoll), std::min(m_frame, section.end + postRoll)};
			if (!result.empty() && wide.start <= result.back().end)
				result.back().end = std::max(result.back().end, wide.end);
			else
				result.push_back(wide);
		}
		return result;
	}

	PitchEstimator::PitchEstimator(double sampleRate)
		: m_rate(sampleRate), m_decimation(std::max<size_t>(1, static_cast<size_t>(sampleRate / 12000.0)))
	{
		m_rate /= static_cast<double>(m_decimation);
		m_pending.reserve(2048);
	}

	void PitchEstimator::keepWindow()
	{
		if (m_pending.size() < static_cast<size_t>(std::ceil(2.0 * m_rate / 40.0)))
			return;
		double sum = 0.0;
		double squares = 0.0;
		for (const double sample : m_pending)
		{
			sum += sample;
			squares += sample * sample;
		}
		const double mean = sum / static_cast<double>(m_pending.size());
		const double energy = (squares / static_cast<double>(m_pending.size())) - (mean * mean);
		if (energy <= std::max(m_energy, 1e-10))
			return;
		m_energy = energy;
		m_best = m_pending;
	}

	void PitchEstimator::feed(const float* samples, size_t count, size_t stride)
	{
		for (size_t i = 0; i < count; ++i)
		{
			m_sum += samples[i * stride];
			if (++m_count < m_decimation)
				continue;
			m_pending.push_back(m_sum / static_cast<double>(m_decimation));
			m_count = 0;
			m_sum = 0.0;
			if (m_pending.size() == 2048)
			{
				keepWindow();
				m_pending.clear();
			}
		}
	}

	std::optional<double> PitchEstimator::finish()
	{
		keepWindow();
		if (m_best.empty())
			return std::nullopt;
		// Cumulative mean normalized squared differences: take the first periodic minimum, rather than the
		// loudest harmonic. Each lag compares the same number of samples; DC cancels in the differences.
		const size_t half = m_best.size() / 2;
		const size_t last = std::min(half - 1, static_cast<size_t>(m_rate / 40.0) + 1);
		std::vector<double> differences(last + 1, 0.0);
		std::vector<double> normalized(last + 1, 1.0);
		double sum = 0.0;
		for (size_t lag = 1; lag <= last; ++lag)
		{
			for (size_t i = 0; i < half; ++i)
			{
				const double delta = m_best[i] - m_best[i + lag];
				differences[lag] += delta * delta;
			}
			sum += differences[lag];
			if (sum > 0.0)
				normalized[lag] = differences[lag] * static_cast<double>(lag) / sum;
		}
		for (size_t lag = 2; lag < last; ++lag)
		{
			if (normalized[lag] >= 0.1 || normalized[lag] > normalized[lag - 1] ||
				normalized[lag] > normalized[lag + 1])
				continue;
			const double curve = differences[lag - 1] - (2.0 * differences[lag]) + differences[lag + 1];
			const double shift = curve > 0.0 ? 0.5 * (differences[lag - 1] - differences[lag + 1]) / curve : 0.0;
			const double pitch = m_rate / (static_cast<double>(lag) + std::clamp(shift, -0.5, 0.5));
			if (pitch >= 40.0 && pitch <= 2000.0)
				return pitch;
			return std::nullopt; // don't turn an unsupported high pitch into a lower subharmonic
		}
		return std::nullopt;
	}

	StatisticsMeter::StatisticsMeter(double sampleRate, int channels)
		: m_part(std::max<int64_t>(1, frames(g_rmsPart, sampleRate)))
	{
		for (int c = 0; c < std::max(channels, 1); ++c)
			m_channels.emplace_back(sampleRate);
		for (Channel& channel : m_channels)
		{
			channel.statistics.minSample = std::numeric_limits<double>::infinity();
			channel.statistics.maxSample = -std::numeric_limits<double>::infinity();
		}
	}

	void StatisticsMeter::feed(const float* frames, size_t count)
	{
		if (count == 0)
			return;
		const size_t channels = m_channels.size();
		const auto window = static_cast<int64_t>(m_channels.front().parts.size()) * m_part;
		for (size_t c = 0; c < channels; ++c)
			m_channels[c].pitch.feed(frames + c, count, channels);
		for (size_t i = 0; i < count; ++i)
		{
			++m_frames;
			const bool partEnds = m_frames % m_part == 0;
			for (size_t c = 0; c < channels; ++c)
			{
				Channel& channel = m_channels[c];
				ChannelStatistics& statistics = channel.statistics;
				const double x = frames[(i * channels) + c];
				statistics.minSample = std::min(statistics.minSample, x);
				statistics.maxSample = std::max(statistics.maxSample, x);
				statistics.peak = std::max(statistics.peak, std::abs(x));
				if (x != 0.0)
					statistics.bitDepth = std::max(statistics.bitDepth, bitsOf(x));
				channel.sum += x;
				channel.squares += x * x;
				channel.part += x * x;
				if (!partEnds)
					continue;
				std::rotate(channel.parts.begin(), channel.parts.begin() + 1, channel.parts.end());
				channel.parts.back() = channel.part;
				channel.part = 0.0;
				if (m_frames >= window)
				{
					double squares = 0.0;
					for (const double part : channel.parts)
						squares += part;
					statistics.maxRms = std::max(statistics.maxRms, std::sqrt(squares / static_cast<double>(window)));
				}
			}
		}
	}

	std::vector<ChannelStatistics> StatisticsMeter::finish()
	{
		std::vector<ChannelStatistics> result;
		for (Channel& channel : m_channels)
		{
			ChannelStatistics statistics = channel.statistics;
			if (m_frames == 0)
			{
				statistics.minSample = statistics.maxSample = 0.0;
			}
			else
			{
				const auto count = static_cast<double>(m_frames);
				statistics.dcOffset = channel.sum / count;
				statistics.averageRms = std::sqrt(channel.squares / count);
				if (m_frames < static_cast<int64_t>(channel.parts.size()) * m_part)
					statistics.maxRms = statistics.averageRms;
			}
			statistics.pitch = channel.pitch.finish();
			result.push_back(statistics);
		}
		return result;
	}
}
