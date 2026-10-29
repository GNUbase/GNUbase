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
			update(b, true);
		}
	}

	void Equalizer::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		m_glideFrames = static_cast<uint32_t>(std::max(1L, std::lround(g_smoothingSeconds * sampleRate)));
		m_output.setTime(g_smoothingSeconds, sampleRate);
		for (Band& band : m_filters)
		{
			band.octaves.setTime(g_smoothingSeconds, sampleRate);
			band.gain.setTime(g_smoothingSeconds, sampleRate);
			band.logQ.setTime(g_smoothingSeconds, sampleRate);
		}
		reset(); // designs the bands for the sample rate
	}

	void Equalizer::reset() noexcept
	{
		for (auto& channel : m_states)
			for (auto& band : channel)
				band.fill(BiquadState{});
		m_fresh = true;
		for (int b = 0; b < g_bands; ++b)
		{
			Band& band = m_filters[static_cast<size_t>(b)];
			band.octaves.finish();
			band.gain.finish();
			band.logQ.finish();
			band.length = 0;
			band.position = 0;
			band.count = -1; // forces a new design
			update(b, true);
		}
		m_output.finish();
		m_frame = 0;
	}

	void Equalizer::setParameter(uint32_t id, double value) noexcept
	{
		if (id == Output)
		{
			m_outputDb = clampParameter(value, -24.0, 24.0);
			m_output.set(dbToGain(m_outputDb), !m_fresh);
			return;
		}
		if (id >= g_parameterCount)
			return;
		const int band = static_cast<int>((id - 1) / BandParameterCount);
		BandSettings& settings = m_bands[static_cast<size_t>(band)];
		bool discrete = true;
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
			discrete = false;
			break;
		case Gain:
			settings.gain = clampParameter(value, -24.0, 24.0);
			discrete = false;
			break;
		case Q:
			settings.q = clampParameter(value, 0.1, 12.0);
			discrete = false;
			break;
		case Slope:
			settings.slope = static_cast<dsp::Slope>(static_cast<int>(clampParameter(value, 0.0, 4.0, true)));
			break;
		default:
			return;
		}
		update(band, discrete);
	}

	int Equalizer::design(const BandSettings& s, Sections& sections, bool keepFlat) const noexcept
	{
		sections.fill(Biquad{});
		if (!s.on)
			return 0;
		const bool flat = s.gain == 0.0 && !keepFlat;
		switch (s.type)
		{
		case FilterType::Bell:
			if (flat)
				return 0;
			sections[0] = Biquad::peaking(m_sampleRate, s.frequency, s.q, s.gain);
			return 1;
		case FilterType::LowShelf:
			if (flat)
				return 0;
			sections[0] = Biquad::lowShelf(m_sampleRate, s.frequency, s.q, s.gain);
			return 1;
		case FilterType::HighShelf:
			if (flat)
				return 0;
			sections[0] = Biquad::highShelf(m_sampleRate, s.frequency, s.q, s.gain);
			return 1;
		case FilterType::LowCut:
		case FilterType::HighCut:
		{
			const bool high = s.type == FilterType::LowCut;
			const int n = order(s.slope);
			if (n == 1)
			{
				sections[0] =
					high ? Biquad::highPass1(m_sampleRate, s.frequency) : Biquad::lowPass1(m_sampleRate, s.frequency);
				return 1;
			}
			for (int k = 0; k < n / 2; ++k)
			{
				const double q = butterworthQ(n, k);
				sections[static_cast<size_t>(k)] = high ? Biquad::highPass(m_sampleRate, s.frequency, q)
														: Biquad::lowPass(m_sampleRate, s.frequency, q);
			}
			return n / 2;
		}
		}
		return 0;
	}

	Equalizer::BandSettings Equalizer::current(int b) const noexcept
	{
		const Band& band = m_filters[static_cast<size_t>(b)];
		BandSettings settings = m_bands[static_cast<size_t>(b)];
		// Once there, the exact values, not their round trip through the logarithm.
		if (!band.octaves.settled())
			settings.frequency = std::exp2(band.octaves.value());
		if (!band.gain.settled())
			settings.gain = band.gain.value();
		if (!band.logQ.settled())
			settings.q = std::exp2(band.logQ.value());
		return settings;
	}

	void Equalizer::update(int b, bool discrete) noexcept
	{
		const BandSettings& s = m_bands[static_cast<size_t>(b)];
		Band& band = m_filters[static_cast<size_t>(b)];
		const auto clear = [this, b](int first, int last)
		{
			for (auto& channel : m_states)
				for (int k = std::max(first, 0); k < last; ++k)
					channel[static_cast<size_t>(b)][static_cast<size_t>(k)] = BiquadState{};
		};
		const bool glide = !m_fresh && m_glideFrames > 0;
		band.octaves.set(std::log2(s.frequency), glide);
		band.gain.set(s.gain, glide);
		band.logQ.set(std::log2(s.q), glide);

		if (!glide)
		{
			Sections sections;
			const int count = design(s, sections, false);
			if (count == band.count && sections == band.to && !band.morphing())
				return;
			// Sections that start to be used start from silence, not from what they held when last used.
			clear(std::min(band.count, count), g_maxSections);
			band.from = sections;
			band.to = sections;
			band.count = count;
			band.targetCount = count;
			band.position = 0;
			band.length = 0;
			return;
		}

		if (!discrete)
		{
			// The parameters glide, and process() designs the sections on the way. A bell or shelf that was left out
			// at 0 dB comes in as a pass-through.
			Sections sections;
			const int needed = design(s, sections, true);
			if (needed > band.count)
			{
				clear(band.count, needed);
				band.count = needed;
			}
			band.targetCount = std::max(band.targetCount, needed);
			return;
		}

		// On, type or slope: morph from where the band is now to the new design.
		if (band.morphing())
			for (size_t k = 0; k < static_cast<size_t>(band.count); ++k)
				band.from[k] = band.at(k);
		else
			band.from = band.to;
		const int count = design(current(b), band.to, true);
		clear(band.count, std::max(band.count, count));
		band.count = std::max(band.count, count);
		band.targetCount = count;
		band.position = 0;
		band.length = m_glideFrames;
	}

	void Equalizer::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		m_fresh = m_fresh && begin == end;
		const bool gliding = !m_output.settled() ||
			std::any_of(m_filters.begin(), m_filters.end(), [](const Band& band) { return band.gliding(); });
		if (gliding)
		{
			processGliding(audio, channels, begin, end);
			return;
		}
		// Settled: channel after channel.
		const int count = std::min(channels, g_maxChannels);
		const double output = m_output.value();
		for (int c = 0; c < count; ++c)
		{
			auto& states = m_states[static_cast<size_t>(c)];
			float* samples = audio[c];
			for (uint32_t i = begin; i < end; ++i)
			{
				double x = samples[i];
				for (size_t b = 0; b < static_cast<size_t>(g_bands); ++b)
				{
					const Band& band = m_filters[b];
					for (size_t k = 0; k < static_cast<size_t>(band.count); ++k)
						x = states[b][k].process(band.to[k], x);
				}
				samples[i] = static_cast<float>(flushDenormal(x * output));
				if ((m_frame + (i - begin) + 1) % g_flushInterval == 0)
					flush(c);
			}
		}
		m_frame += end - begin;
	}

	void Equalizer::flush(int channel) noexcept
	{
		for (auto& band : m_states[static_cast<size_t>(channel)])
			for (BiquadState& state : band)
				state.flush();
	}

	void Equalizer::processGliding(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		// Sample after sample, with each gliding band one step further on every sample. Designs and flushes happen at
		// frames counted from the last reset, and the arithmetic per channel is the same as when settled, so how the
		// signal is split into blocks changes nothing.
		const int count = std::min(channels, g_maxChannels);
		for (uint32_t i = begin; i < end; ++i)
		{
			for (int b = 0; b < g_bands; ++b)
			{
				Band& band = m_filters[static_cast<size_t>(b)];
				Sections& coefficients = m_current[static_cast<size_t>(b)];
				if (!band.parametersSettled())
				{
					band.octaves.next();
					band.gain.next();
					band.logQ.next();
					if (band.parametersSettled() || m_frame % g_designInterval == 0)
						design(current(b), band.to, true);
				}
				if (band.morphing())
				{
					++band.position;
					for (size_t k = 0; k < static_cast<size_t>(band.count); ++k)
						coefficients[k] = band.at(k);
				}
				else
				{
					coefficients = band.to;
				}
			}
			const double output = m_output.next();
			for (int c = 0; c < count; ++c)
			{
				auto& states = m_states[static_cast<size_t>(c)];
				double x = audio[c][i];
				for (size_t b = 0; b < static_cast<size_t>(g_bands); ++b)
					for (size_t k = 0; k < static_cast<size_t>(m_filters[b].count); ++k)
						x = states[b][k].process(m_current[b][k], x);
				audio[c][i] = static_cast<float>(flushDenormal(x * output));
			}
			if (++m_frame % g_flushInterval == 0)
				for (int c = 0; c < count; ++c)
					flush(c);
			for (Band& band : m_filters)
				if (band.morphing() && band.position == band.length + 2)
				{
					band.from = band.to;
					band.count = band.targetCount;
					band.position = 0;
					band.length = 0;
				}
		}
	}
}
