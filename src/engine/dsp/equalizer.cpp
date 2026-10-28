// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/equalizer.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::dsp
{
	namespace
	{
		int order(Slope slope)
		{
			switch (slope)
			{
			case Slope::Db6:
				return 1;
			case Slope::Db12:
				return 2;
			case Slope::Db24:
				return 4;
			case Slope::Db36:
				return 6;
			case Slope::Db48:
				return 8;
			}
			return 2;
		}

		// The Q of section k of an even-order Butterworth filter: its poles' angles.
		double butterworthQ(int order, int section)
		{
			const double angle = ((2.0 * section) + 1.0) * g_pi / (2.0 * order);
			return 1.0 / (2.0 * std::cos(angle));
		}
	}

	Equalizer::BandSettings Equalizer::defaultBand(int band)
	{
		BandSettings settings;
		switch (band)
		{
		case 0:
			settings = {false, FilterType::LowCut, 40.0, 0.0, 0.71, dsp::Slope::Db12};
			break;
		case 1:
			settings = {true, FilterType::LowShelf, 100.0, 0.0, 0.71, dsp::Slope::Db12};
			break;
		case 2:
			settings = {true, FilterType::Bell, 500.0, 0.0, 1.0, dsp::Slope::Db12};
			break;
		case 3:
			settings = {true, FilterType::Bell, 2000.0, 0.0, 1.0, dsp::Slope::Db12};
			break;
		case 4:
			settings = {true, FilterType::HighShelf, 8000.0, 0.0, 0.71, dsp::Slope::Db12};
			break;
		default:
			settings = {false, FilterType::HighCut, 16000.0, 0.0, 0.71, dsp::Slope::Db12};
			break;
		}
		return settings;
	}

	Equalizer::Equalizer()
	{
		for (int b = 0; b < g_bands; ++b)
		{
			m_bands[static_cast<size_t>(b)] = defaultBand(b);
			update(b);
		}
	}

	void Equalizer::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		for (int b = 0; b < g_bands; ++b)
			update(b);
		reset();
	}

	void Equalizer::reset() noexcept
	{
		for (auto& channel : m_states)
			for (auto& band : channel)
				band.fill(BiquadState{});
	}

	void Equalizer::setParameter(uint32_t id, double value) noexcept
	{
		if (id == Output)
		{
			m_outputDb = clampParameter(value, -24.0, 24.0);
			m_output = dbToGain(m_outputDb);
			return;
		}
		if (id >= g_parameterCount)
			return;
		const int band = static_cast<int>((id - 1) / BandParameterCount);
		BandSettings& settings = m_bands[static_cast<size_t>(band)];
		switch (static_cast<BandParameter>((id - 1) % BandParameterCount))
		{
		case On:
			settings.on = clampParameter(value, 0.0, 1.0, true) != 0.0;
			break;
		case Type:
			settings.type = static_cast<FilterType>(static_cast<int>(clampParameter(value, 0.0, 4.0, true)));
			break;
		case Frequency:
			settings.frequency = clampParameter(value, 20.0, 20000.0);
			break;
		case Gain:
			settings.gain = clampParameter(value, -24.0, 24.0);
			break;
		case Q:
			settings.q = clampParameter(value, 0.1, 12.0);
			break;
		case Slope:
			settings.slope = static_cast<dsp::Slope>(static_cast<int>(clampParameter(value, 0.0, 4.0, true)));
			break;
		default:
			return;
		}
		update(band);
	}

	void Equalizer::update(int band) noexcept
	{
		const BandSettings& s = m_bands[static_cast<size_t>(band)];
		auto& sections = m_sections[static_cast<size_t>(band)];
		int& count = m_sectionCount[static_cast<size_t>(band)];
		const int previous = count;
		count = 0;
		if (s.on)
		{
			switch (s.type)
			{
			case FilterType::Bell:
				if (s.gain != 0.0)
					sections[static_cast<size_t>(count++)] = Biquad::peaking(m_sampleRate, s.frequency, s.q, s.gain);
				break;
			case FilterType::LowShelf:
				if (s.gain != 0.0)
					sections[static_cast<size_t>(count++)] = Biquad::lowShelf(m_sampleRate, s.frequency, s.q, s.gain);
				break;
			case FilterType::HighShelf:
				if (s.gain != 0.0)
					sections[static_cast<size_t>(count++)] = Biquad::highShelf(m_sampleRate, s.frequency, s.q, s.gain);
				break;
			case FilterType::LowCut:
			case FilterType::HighCut:
			{
				const bool high = s.type == FilterType::LowCut;
				const int n = order(s.slope);
				if (n == 1)
				{
					sections[static_cast<size_t>(count++)] = high ? Biquad::highPass1(m_sampleRate, s.frequency)
																  : Biquad::lowPass1(m_sampleRate, s.frequency);
					break;
				}
				for (int k = 0; k < n / 2; ++k)
				{
					const double q = butterworthQ(n, k);
					sections[static_cast<size_t>(count++)] = high ? Biquad::highPass(m_sampleRate, s.frequency, q)
																  : Biquad::lowPass(m_sampleRate, s.frequency, q);
				}
				break;
			}
			}
		}
		// Sections that start to be used start from silence, not from what they held when last used.
		for (int k = std::min(previous, count); k < g_maxSections; ++k)
			for (auto& channel : m_states)
				channel[static_cast<size_t>(band)][static_cast<size_t>(k)] = BiquadState{};
	}

	void Equalizer::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		const int count = std::min(channels, g_maxChannels);
		for (int c = 0; c < count; ++c)
		{
			auto& states = m_states[static_cast<size_t>(c)];
			float* samples = audio[c];
			for (uint32_t i = begin; i < end; ++i)
			{
				double x = samples[i];
				for (size_t b = 0; b < static_cast<size_t>(g_bands); ++b)
					for (size_t k = 0; k < static_cast<size_t>(m_sectionCount[b]); ++k)
						x = states[b][k].process(m_sections[b][k], x);
				samples[i] = static_cast<float>(flushDenormal(x * m_output));
			}
			for (auto& band : states)
				for (BiquadState& state : band)
					state.flush();
		}
	}
}
