// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

#include <array>
#include <atomic>
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
	//
	// A new size, decay or predelay doesn't jump: the comb and predelay lines glide to their new lengths, reading
	// between samples while they move, and the combs' gains glide along, so what the lines hold keeps sounding instead
	// of being cut off or replayed out of order. Damping and mix glide too.
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
		// How long the lines take to glide to a new size or predelay: 1 - 1/e of the way.
		static constexpr double g_glideSeconds = 0.1;

		Reverb() { update(); }

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// How long the reverb sounds after the input went quiet: the predelay plus 1.5 times the decay (90 dB).
		// [any thread] Computed when the parameters change.
		uint32_t tail() const { return m_tail.load(std::memory_order_relaxed); }

	private:
		// A delay line whose length can change while it runs.
		struct Line
		{
			std::vector<float> buffer; // allocated for the longest delay, plus two
			size_t write = 0;          // where the next sample goes

			// The sample pushed `delay` samples ago (1: the last one), between samples interpolated linearly; on a
			// whole number of samples, exactly that sample.
			float read(double delay) const noexcept;
			void push(float sample) noexcept
			{
				buffer[write] = sample;
				write = write + 1 == buffer.size() ? 0 : write + 1;
			}
		};

		// What a comb glides: its length, a whole number of samples once there, and its gains.
		struct CombSettings
		{
			double length = 1.0;
			double feedback = 0.0;
			double inputGain = 0.0; // sqrt(1 - feedback^2): every comb adds the same energy, whatever its decay

			CombSettings between(const CombSettings& to, double t) const
			{
				return {length + ((to.length - length) * t), feedback + ((to.feedback - feedback) * t),
					inputGain + ((to.inputGain - inputGain) * t)};
			}
			bool operator!=(const CombSettings& other) const
			{
				return length != other.length || feedback != other.feedback || inputGain != other.inputGain;
			}
		};

		struct Comb
		{
			Line line;
			CombSettings from;   // when the current glide began
			CombSettings to;     // where it glides to
			float filter = 0.0f; // the damping low-pass's state
		};

		struct Allpass
		{
			std::vector<float> buffer;
			size_t position = 0;
		};

		struct Channel
		{
			std::array<Comb, g_combs> combs;
			std::array<Allpass, g_allpasses> allpasses;
		};

		void update() noexcept;
		// glide: how far the combs are from their `from` to their `to` settings, 1 when there.
		static float processChannel(Channel& channel, float input, double glide, float damp) noexcept;

		double m_sampleRate = 48000.0;
		double m_size = 50.0;
		double m_decay = 2.0;
		double m_damping = 50.0;
		double m_predelayMs = 20.0;
		double m_mix = 0.25;

		Smoothed m_damp{0.25}; // the damping low-pass coefficient
		std::array<Channel, 2> m_channels;
		Line m_predelay;
		Smoothed m_predelayFrames{0.0, 1e-3};
		Smoothed m_glide{1.0}; // the combs' way from their old to their new lengths, from 0 to 1
		Smoothed m_wet{0.25};
		std::atomic<uint32_t> m_tail{0}; // read by tail() on any thread
		bool m_fresh = true;             // nothing processed since the last reset: parameters apply at once
	};
}
