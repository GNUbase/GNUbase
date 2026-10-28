// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/reverb.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gnubase::engine::dsp
{
	namespace
	{
		// Freeverb's delay lengths at 44.1 kHz: mutually prime, so the echoes don't pile up on each other.
		constexpr std::array<int, Reverb::g_combs> g_combTunings = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
		constexpr std::array<int, Reverb::g_allpasses> g_allpassTunings = {556, 441, 341, 225};
		constexpr int g_stereoSpread = 23; // added to every delay of the right channel
		constexpr double g_tuningRate = 44100.0;
		constexpr double g_maxScale = 2.0;
		constexpr float g_allpassFeedback = 0.5f;
		constexpr float g_maxDamp = 0.5f;
		// The comb outputs are uncorrelated, so their sum has eight times the power of one.
		constexpr float g_combsGain = 0.35355339f; // 1 / sqrt(8)
		static_assert(Reverb::g_combs == 8);

		double scale(double size)
		{
			return 0.3 + ((g_maxScale - 0.3) * size / 100.0);
		}

		size_t frames(double samples)
		{
			return static_cast<size_t>(std::max(1.0, std::round(samples)));
		}

		void resize(std::vector<float>& buffer, size_t& length, size_t& position, size_t newLength)
		{
			length = std::min(newLength, buffer.size());
			position = length == 0 ? 0 : position % length;
		}
	}

	void Reverb::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		const double rate = sampleRate / g_tuningRate;
		for (size_t ch = 0; ch < m_channels.size(); ++ch)
		{
			const int spread = static_cast<int>(ch) * g_stereoSpread;
			Channel& channel = m_channels[ch];
			for (size_t k = 0; k < channel.combs.size(); ++k)
			{
				Line& line = channel.combs[k].line;
				line.buffer.assign(frames(std::ceil((g_combTunings[k] + spread) * rate * g_maxScale)) + 1, 0.0f);
				line.length = 1;
				line.position = 0;
			}
			for (size_t k = 0; k < channel.allpasses.size(); ++k)
			{
				Line& line = channel.allpasses[k];
				line.length = frames((g_allpassTunings[k] + spread) * rate);
				line.buffer.assign(line.length, 0.0f);
				line.position = 0;
			}
		}
		m_predelay.buffer.assign(frames(std::ceil(g_maxPredelaySeconds * sampleRate)) + 1, 0.0f);
		m_predelay.length = 1;
		m_predelay.position = 0;
		update();
		reset();
	}

	void Reverb::reset() noexcept
	{
		for (Channel& channel : m_channels)
		{
			for (Comb& comb : channel.combs)
			{
				std::fill(comb.line.buffer.begin(), comb.line.buffer.end(), 0.0f);
				comb.filter = 0.0f;
			}
			for (Line& line : channel.allpasses)
				std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
		}
		std::fill(m_predelay.buffer.begin(), m_predelay.buffer.end(), 0.0f);
	}

	void Reverb::setParameter(uint32_t id, double value) noexcept
	{
		switch (id)
		{
		case Size:
			m_size = clampParameter(value, 0.0, 100.0);
			break;
		case Decay:
			m_decay = clampParameter(value, 0.1, 20.0);
			break;
		case Damping:
			m_damping = clampParameter(value, 0.0, 100.0);
			break;
		case Predelay:
			m_predelayMs = clampParameter(value, 0.0, g_maxPredelaySeconds * 1000.0);
			break;
		case Mix:
			m_mix = clampParameter(value, 0.0, 100.0) / 100.0;
			break;
		default:
			return;
		}
		update();
	}

	void Reverb::update() noexcept
	{
		m_damp = static_cast<float>(m_damping / 100.0) * g_maxDamp;
		const double rate = m_sampleRate / g_tuningRate * scale(m_size);
		for (size_t ch = 0; ch < m_channels.size(); ++ch)
		{
			const int spread = static_cast<int>(ch) * g_stereoSpread;
			for (size_t k = 0; k < m_channels[ch].combs.size(); ++k)
			{
				Comb& comb = m_channels[ch].combs[k];
				if (comb.line.buffer.empty()) // not prepared
					continue;
				resize(
					comb.line.buffer, comb.line.length, comb.line.position, frames((g_combTunings[k] + spread) * rate));
				// Falls by 60 dB in m_decay seconds: g^(decay * fs / length) = 10^-3.
				const double feedback =
					std::pow(10.0, -3.0 * static_cast<double>(comb.line.length) / (m_decay * m_sampleRate));
				comb.feedback = static_cast<float>(feedback);
				comb.inputGain = static_cast<float>(std::sqrt(1.0 - (feedback * feedback)));
			}
		}
		if (!m_predelay.buffer.empty())
			resize(m_predelay.buffer, m_predelay.length, m_predelay.position,
				static_cast<size_t>(std::round(m_predelayMs / 1000.0 * m_sampleRate)));
	}

	uint32_t Reverb::tail() const
	{
		const double samples = ((m_predelayMs / 1000.0) + (1.5 * m_decay)) * m_sampleRate;
		return static_cast<uint32_t>(
			std::min(std::ceil(samples), static_cast<double>(std::numeric_limits<uint32_t>::max() - 1)));
	}

	float Reverb::processChannel(Channel& channel, float input) const noexcept
	{
		float sum = 0.0f;
		for (Comb& comb : channel.combs)
		{
			Line& line = comb.line;
			const float out = line.buffer[line.position];
			comb.filter = flushDenormal((out * (1.0f - m_damp)) + (comb.filter * m_damp));
			line.buffer[line.position] = flushDenormal((input * comb.inputGain) + (comb.filter * comb.feedback));
			line.position = line.position + 1 == line.length ? 0 : line.position + 1;
			sum += out;
		}
		float out = sum * g_combsGain;
		for (Line& line : channel.allpasses)
		{
			// Schroeder's allpass: v[n] = x[n] + g v[n - D], y[n] = v[n - D] - g v[n]. Unlike Freeverb's
			// approximation its gain is 1 at every frequency.
			const float delayed = line.buffer[line.position];
			const float v = flushDenormal(out + (delayed * g_allpassFeedback));
			line.buffer[line.position] = v;
			line.position = line.position + 1 == line.length ? 0 : line.position + 1;
			out = delayed - (g_allpassFeedback * v);
		}
		return out;
	}

	void Reverb::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		if (m_predelay.buffer.empty()) // not prepared
			return;
		const int count = std::min(channels, 2);
		const auto wet = static_cast<float>(m_mix);
		const auto dry = static_cast<float>(1.0 - m_mix);
		const bool predelay = m_predelay.length > 0;
		for (uint32_t i = begin; i < end; ++i)
		{
			const float left = audio[0][i];
			float input = count == 2 ? 0.5f * (left + audio[1][i]) : left;
			if (predelay)
			{
				float& slot = m_predelay.buffer[m_predelay.position];
				const float delayed = slot;
				slot = input;
				input = delayed;
				m_predelay.position = m_predelay.position + 1 == m_predelay.length ? 0 : m_predelay.position + 1;
			}
			audio[0][i] = (dry * left) + (wet * processChannel(m_channels[0], input));
			if (count == 2)
				audio[1][i] = (dry * audio[1][i]) + (wet * processChannel(m_channels[1], input));
		}
	}
}
