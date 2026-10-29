// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/dither.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::dsp
{
	void Dither::prepare(double /*sampleRate*/, uint32_t /*maxFrames*/)
	{
		reset();
	}

	void Dither::reset() noexcept
	{
		for (size_t c = 0; c < m_channels.size(); ++c)
			m_channels[c] = {Random(0x2545f491u + (static_cast<uint32_t>(c) * 0x9e3779b9u)), 0.0, 0.0};
	}

	void Dither::setParameter(uint32_t id, double value) noexcept
	{
		if (id == Bits)
			m_bits = static_cast<int>(clampParameter(value, 8.0, 24.0, true));
		else if (id == NoiseShaping)
			m_shaping = static_cast<int>(clampParameter(value, 0.0, 2.0, true));
	}

	void Dither::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		const int count = std::min(channels, g_maxChannels);
		const double step = std::ldexp(1.0, 1 - m_bits);
		const double steps = 1.0 / step;
		const double highest = 1.0 - step;
		// The error fed back: NTF(z) = 1 - c1 z^-1 - c2 z^-2.
		double c1 = 0.0;
		double c2 = 0.0;
		if (m_shaping == 1)
		{
			c1 = 1.0;
		}
		else if (m_shaping == 2)
		{
			c1 = 2.0;
			c2 = -1.0;
		}
		const double maxError = 4.0 * step; // after clipping, so the feedback can't run away
		for (int c = 0; c < count; ++c)
		{
			Channel& channel = m_channels[static_cast<size_t>(c)];
			float* samples = audio[c];
			for (uint32_t i = begin; i < end; ++i)
			{
				const double wanted = samples[i] - (c1 * channel.error1) - (c2 * channel.error2);
				const double noise = (channel.random.uniform() - channel.random.uniform()) * step;
				const double quantized = std::clamp(std::floor(((wanted + noise) * steps) + 0.5) * step, -1.0, highest);
				channel.error2 = channel.error1;
				channel.error1 = std::clamp(quantized - wanted, -maxError, maxError);
				samples[i] = static_cast<float>(quantized);
			}
		}
	}
}
