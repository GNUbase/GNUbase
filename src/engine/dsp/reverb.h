// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

#include <array>
#include <cstdint>
#include <vector>

namespace gnubase::engine::dsp
{
	// An algorithmic reverb after Schroeder and Freeverb: per output channel eight parallel comb filters with damping
	// in their feedback, then four allpass diffusers (true allpasses, unlike Freeverb's); the right channel's delays
	// are a little longer, which decorrelates the sides. Unlike Freeverb, the decay is a time: each comb's feedback is
	// set so that it decays by 60 dB in that time (Schroeder's formula), so size and decay are independent. Every loop
	// gain stays below 1, so the reverb is stable at every setting. The input is the sum of the first two channels
	// after the predelay; mono nodes get the left output, channels after the second pass unchanged.
	class Reverb
	{
	public:
		enum Parameter : uint8_t
		{
			Size,     // %, 0 to 100: scales the room, i.e. the delay lengths
			Decay,    // s, 0.1 to 20: the time the reverb takes to fall by 60 dB (RT60), without damping
			Damping,  // %, 0 to 100: high frequencies decay faster
			Predelay, // ms, 0 to 200
			Mix,      // %, 0 to 100: 0 dry only, 100 reverb only
		};

		static constexpr int g_combs = 8;
		static constexpr int g_allpasses = 4;
		static constexpr double g_maxPredelaySeconds = 0.2;

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// How long the reverb sounds after the input went quiet: the predelay plus 1.5 times the decay (90 dB).
		uint32_t tail() const;

	private:
		struct Line
		{
			std::vector<float> buffer; // allocated for the largest size
			size_t length = 1;         // in use
			size_t position = 0;
		};

		struct Comb
		{
			Line line;
			float feedback = 0.0f;
			float inputGain = 0.0f; // sqrt(1 - feedback^2): every comb adds the same energy, whatever its decay
			float filter = 0.0f;    // the damping low-pass's state
		};

		struct Channel
		{
			std::array<Comb, g_combs> combs;
			std::array<Line, g_allpasses> allpasses;
		};

		void update() noexcept;
		float processChannel(Channel& channel, float input) const noexcept;

		double m_sampleRate = 48000.0;
		double m_size = 50.0;
		double m_decay = 2.0;
		double m_damping = 50.0;
		double m_predelayMs = 20.0;
		double m_mix = 0.25;

		float m_damp = 0.0f; // the damping low-pass coefficient
		std::array<Channel, 2> m_channels;
		Line m_predelay;
	};
}
