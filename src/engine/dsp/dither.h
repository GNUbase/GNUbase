// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

#include <array>
#include <cstdint>

namespace gnubase::engine::dsp
{
	// Reduces the word length to 8 to 24 bits with TPDF dither (triangular noise of plus or minus one step, the sum
	// of two uniform random numbers), so the quantization error is noise independent of the signal instead of
	// distortion. Noise shaping feeds the error back through a high-pass, which moves the noise towards high
	// frequencies, where the ear is less sensitive: first order with the noise transfer function 1 - z^-1, second
	// order with (1 - z^-1)^2. Every output sample is a whole multiple of the step 2^-(bits - 1) within [-1, 1).
	// The noise comes from a fixed seed per channel, so the output is the same on every run (rule 6).
	class Dither
	{
	public:
		enum Parameter : uint8_t
		{
			Bits,         // 8 to 24
			NoiseShaping, // 0 off, 1 first order, 2 second order
		};

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

	private:
		struct Channel
		{
			Random random;
			double error1 = 0.0; // the last two errors, for noise shaping
			double error2 = 0.0;
		};

		int m_bits = 16;
		int m_shaping = 0;
		std::array<Channel, g_maxChannels> m_channels;
	};
}
