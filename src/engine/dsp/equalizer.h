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
		using Sections = std::array<Biquad, g_maxSections>;

		// A band's sections. Changes don't jump. Frequency, gain and Q glide (in octaves, dB and octaves) with a
		// one-pole over g_smoothingSeconds, and the sections are designed anew from where they are every
		// g_designInterval frames, so every step on the way is a proper bell, shelf or cut. Turning a band on or off,
		// or changing its type or slope, morphs the coefficients in a straight line from where the band was to the
		// new design over the same time; sections beyond those in use are pass-throughs, so they can come and go.
		struct Band
		{
			Smoothed octaves{0.0, 1e-9}; // log2 of the frequency
			Smoothed gain{0.0, 1e-9};    // dB
			Smoothed logQ{0.0, 1e-9};    // log2 of Q
			Sections from{};
			Sections to{};         // designed from the gliding parameters
			int count = 0;         // the sections processed: those in use before or after a morph
			int targetCount = 0;   // the sections in use once the morph is over
			uint32_t position = 0; // samples into the morph
			uint32_t length = 0;   // of the morph, 0 when there is none; it ends two samples after reaching `to`, in
								   // which sections that are no longer in use pass their last state on

			bool morphing() const { return length > 0; }
			bool parametersSettled() const { return octaves.settled() && gain.settled() && logQ.settled(); }
			bool gliding() const { return morphing() || !parametersSettled(); }
			// The coefficients of section k at the current position.
			Biquad at(size_t k) const
			{
				return position >= length ? to[k]
										  : Biquad::between(from[k], to[k], static_cast<double>(position) / length);
			}
		};

		static constexpr uint64_t g_designInterval = 16;

		// The filter states are flushed to zero below 1e-30 every this many frames, counted from the last reset, so
		// they decay to exact silence the same way however the host splits the signal into blocks. (Flushing every
		// sample would let each flush of an early section excite the later ones, which then never quite settle.)
		static constexpr uint64_t g_flushInterval = 256;

		// The band's sections for the settings, pass-throughs after them; returns how many there are. A bell or shelf
		// at 0 dB needs none, unless keepFlat (while it may glide away from 0 dB).
		int design(const BandSettings& settings, Sections& sections, bool keepFlat) const noexcept;
		// The settings with the band's gliding parameters where they are now.
		BandSettings current(int band) const noexcept;
		void update(int band, bool discrete) noexcept;
		void processGliding(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;
		void flush(int channel) noexcept;

		double m_sampleRate = 48000.0;
		Smoothed m_output{1.0}; // linear
		double m_outputDb = 0.0;
		uint32_t m_glideFrames = 0; // from prepare on
		uint64_t m_frame = 0;       // frames processed since the last reset
		bool m_fresh = true;        // nothing processed since the last reset: parameters apply at once
		std::array<BandSettings, g_bands> m_bands{};
		std::array<Band, g_bands> m_filters{};
		std::array<Sections, g_bands> m_current{}; // the gliding bands' coefficients for the current sample
		std::array<std::array<std::array<BiquadState, g_maxSections>, g_bands>, g_maxChannels> m_states{};
	};
}
