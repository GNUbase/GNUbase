// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/biquad.h"
#include "engine/dsp/dsp.h"

#include <array>
#include <cstdint>

namespace gnubase::engine::dsp
{
	enum class FilterType : uint8_t
	{
		Bell,      // peaking: gain at the frequency, width from Q
		LowShelf,  // gain below the frequency
		HighShelf, // gain above the frequency
		LowCut,    // high-pass, Butterworth, with the band's slope
		HighCut,   // low-pass, Butterworth, with the band's slope
	};

	// Slopes of the cut filters, in dB per octave: 6, 12, 24, 36, 48.
	enum class Slope : uint8_t
	{
		Db6,
		Db12,
		Db24,
		Db36,
		Db48,
	};

	// A parametric equalizer with six bands, each of which can be any filter type. Bells and shelves are the cookbook
	// biquads; cuts are Butterworth filters of order 1, 2, 4, 6 or 8 made of cascaded sections. By default the first
	// band is a low cut and the last a high cut, both off, with a low shelf, two bells and a high shelf in between.
	class Equalizer
	{
	public:
		static constexpr int g_bands = 6;

		enum BandParameter : uint8_t
		{
			On,        // 0 off, 1 on
			Type,      // FilterType
			Frequency, // Hz, 20 to 20000
			Gain,      // dB, -24 to 24; bells and shelves
			Q,         // 0.1 to 12; bells and shelves
			Slope,     // dsp::Slope; cuts
			BandParameterCount,
		};

		// Parameter ids: Output, then each band's parameters in the order above.
		enum Parameter : uint8_t
		{
			Output = 0, // dB, -24 to 24
		};
		static constexpr uint32_t parameter(int band, BandParameter p)
		{
			return 1 + (static_cast<uint32_t>(band) * BandParameterCount) + p;
		}
		static constexpr uint32_t g_parameterCount = 1 + (g_bands * BandParameterCount);

		struct BandSettings
		{
			bool on = false;
			FilterType type = FilterType::Bell;
			double frequency = 1000.0;
			double gain = 0.0;
			double q = 1.0;
			dsp::Slope slope = dsp::Slope::Db12;
		};

		// The settings each band starts with.
		static BandSettings defaultBand(int band);

		Equalizer();

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		const BandSettings& band(int index) const { return m_bands[static_cast<size_t>(index)]; }

	private:
		static constexpr int g_maxSections = 4; // order 8

		void update(int band) noexcept;

		double m_sampleRate = 48000.0;
		double m_output = 1.0; // linear
		double m_outputDb = 0.0;
		std::array<BandSettings, g_bands> m_bands{};
		std::array<std::array<Biquad, g_maxSections>, g_bands> m_sections{};
		std::array<int, g_bands> m_sectionCount{}; // 0 for a band that is off
		std::array<std::array<std::array<BiquadState, g_maxSections>, g_bands>, g_maxChannels> m_states{};
	};
}
