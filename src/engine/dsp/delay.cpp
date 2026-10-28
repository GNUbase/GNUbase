// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/delay.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gnubase::engine::dsp
{
	const std::array<Delay::NoteValue, 16>& Delay::noteValues()
	{
		static const std::array<NoteValue, 16> values = {{
			{"1/32T", 1.0 / 12.0},
			{"1/32", 0.125},
			{"1/32D", 0.1875},
			{"1/16T", 1.0 / 6.0},
			{"1/16", 0.25},
			{"1/16D", 0.375},
			{"1/8T", 1.0 / 3.0},
			{"1/8", 0.5},
			{"1/8D", 0.75},
			{"1/4T", 2.0 / 3.0},
			{"1/4", 1.0},
			{"1/4D", 1.5},
			{"1/2T", 4.0 / 3.0},
			{"1/2", 2.0},
			{"1/2D", 3.0},
			{"1/1", 4.0},
		}};
		return values;
	}

	void Delay::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		m_length = static_cast<size_t>(std::ceil(g_maxSeconds * sampleRate)) + 1;
		for (std::vector<float>& line : m_lines)
			line.assign(m_length, 0.0f);
		update();
		reset();
	}

	void Delay::reset() noexcept
	{
		for (std::vector<float>& line : m_lines)
			std::fill(line.begin(), line.end(), 0.0f);
		m_write = 0;
	}

	void Delay::setParameter(uint32_t id, double value) noexcept
	{
		switch (id)
		{
		case Time:
			m_timeMs = clampParameter(value, 1.0, g_maxSeconds * 1000.0);
			break;
		case Sync:
			m_sync = clampParameter(value, 0.0, 1.0, true) != 0.0;
			break;
		case Note:
			m_note = static_cast<int>(clampParameter(value, 0.0, static_cast<double>(noteValues().size() - 1), true));
			break;
		case Feedback:
			m_feedback = clampParameter(value, 0.0, 100.0) / 100.0;
			break;
		case Mix:
			m_mix = clampParameter(value, 0.0, 100.0) / 100.0;
			break;
		case PingPong:
			m_pingPong = clampParameter(value, 0.0, 1.0, true) != 0.0;
			break;
		default:
			return;
		}
		update();
	}

	void Delay::setTempo(double bpm) noexcept
	{
		if (bpm <= 0.0 || bpm == m_tempo)
			return;
		m_tempo = bpm;
		if (m_sync)
			update();
	}

	void Delay::update() noexcept
	{
		const double seconds =
			m_sync ? noteValues()[static_cast<size_t>(m_note)].quarters * 60.0 / m_tempo : m_timeMs / 1000.0;
		const double frames = std::round(std::min(seconds, g_maxSeconds) * m_sampleRate);
		const double longest = m_length > 1 ? static_cast<double>(m_length - 1) : 1.0;
		m_frames = static_cast<uint32_t>(std::clamp(frames, 1.0, longest));
	}

	uint32_t Delay::tail() const
	{
		constexpr auto endless = std::numeric_limits<uint32_t>::max();
		if (m_feedback >= 1.0)
			return endless;
		double repeats = 1.0;
		if (m_feedback > 0.0)
			repeats += std::ceil(std::log(std::pow(10.0, -90.0 / 20.0)) / std::log(m_feedback));
		return static_cast<uint32_t>(std::min(repeats * m_frames, static_cast<double>(endless - 1)));
	}

	void Delay::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		if (m_length == 0) // not prepared
			return;
		const int count = std::min(channels, 2);
		const auto feedback = static_cast<float>(m_feedback);
		const auto wet = static_cast<float>(m_mix);
		const auto dry = static_cast<float>(1.0 - m_mix);
		float* const left = m_lines[0].data();
		float* const right = m_lines[1].data();
		for (uint32_t i = begin; i < end; ++i)
		{
			const size_t read = (m_write + m_length - m_frames) % m_length;
			if (m_pingPong && count == 2)
			{
				const float l = audio[0][i];
				const float r = audio[1][i];
				const float echoLeft = left[read];
				const float echoRight = right[read];
				left[m_write] = flushDenormal((0.5f * (l + r)) + (feedback * echoRight));
				right[m_write] = flushDenormal(feedback * echoLeft);
				audio[0][i] = (dry * l) + (wet * echoLeft);
				audio[1][i] = (dry * r) + (wet * echoRight);
			}
			else
			{
				for (int c = 0; c < count; ++c)
				{
					float* const line = m_lines[static_cast<size_t>(c)].data();
					const float x = audio[c][i];
					const float echo = line[read];
					line[m_write] = flushDenormal(x + (feedback * echo));
					audio[c][i] = (dry * x) + (wet * echo);
				}
			}
			m_write = m_write + 1 == m_length ? 0 : m_write + 1;
		}
	}
}
