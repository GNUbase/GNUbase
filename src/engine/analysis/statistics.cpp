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
		  m_hold(frames(g_hold, sampleRate))
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
				if (level >= m_open)
					m_start = m_last = m_frame;
			}
			else if (level >= m_close)
			{
				m_last = m_frame;
			}
			else if (m_frame - m_last > m_hold)
			{
				m_sections.push_back({m_start, m_last + 1});
				m_start = -1;
			}
		}
	}

	std::vector<Section> SilenceDetector::finish()
	{
		if (m_start >= 0)
			m_sections.push_back({m_start, m_last + 1});
		m_start = -1;

		std::vector<Section> joined;
		const int64_t minimumClosed = frames(m_settings.minimumClosed, m_sampleRate);
		for (const Section& section : m_sections)
		{
			if (!joined.empty() && section.start - joined.back().end < minimumClosed)
				joined.back().end = section.end;
			else
				joined.push_back(section);
		}
		std::vector<Section> result;
		const int64_t minimumOpen = frames(m_settings.minimumOpen, m_sampleRate);
		const int64_t preRoll = frames(m_settings.preRoll, m_sampleRate);
		const int64_t postRoll = frames(m_settings.postRoll, m_sampleRate);
		for (const Section& section : joined)
		{
			if (section.end - section.start < minimumOpen)
				continue;
			const Section wide = {
				std::max<int64_t>(0, section.start - preRoll), std::min(m_frame, section.end + postRoll)};
			if (!result.empty() && wide.start <= result.back().end)
				result.back().end = std::max(result.back().end, wide.end);
			else
				result.push_back(wide);
		}
		return result;
	}

	StatisticsMeter::StatisticsMeter(double sampleRate, int channels)
		: m_sampleRate(sampleRate), m_channels(static_cast<size_t>(std::max(channels, 1))),
		  m_part(std::max<int64_t>(1, frames(g_rmsPart, sampleRate)))
	{
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
			m_channels[c].spectrum.feed(frames + c, count, channels);
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
			if (statistics.peak > 0.0)
			{
				const Spectrum strongest = summarize(channel.spectrum.finish(), m_sampleRate, 1);
				if (!strongest.peaks.empty())
					statistics.pitch = strongest.peaks.front().frequency;
			}
			result.push_back(statistics);
		}
		return result;
	}
}
