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
	}

	float Reverb::Line::read(double delay) const noexcept
	{
		// 1 <= delay <= buffer.size() - 2, so both taps are samples still in the line.
		const size_t size = buffer.size();
		const double whole = std::floor(delay);
		const auto fraction = static_cast<float>(delay - whole);
		const auto back = static_cast<size_t>(whole);
		const size_t at = write >= back ? write - back : write + size - back;
		const float newer = buffer[at];
		if (fraction == 0.0f)
			return newer;
		const float older = buffer[at == 0 ? size - 1 : at - 1];
		return newer + (fraction * (older - newer));
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
				channel.combs[k].line.buffer.assign(
					frames(std::ceil((g_combTunings[k] + spread) * rate * g_maxScale)) + 2, 0.0f);
			for (size_t k = 0; k < channel.allpasses.size(); ++k)
				channel.allpasses[k].buffer.assign(frames((g_allpassTunings[k] + spread) * rate), 0.0f);
		}
		m_predelay.buffer.assign(frames(std::ceil(g_maxPredelaySeconds * sampleRate)) + 3, 0.0f);
		m_predelayFrames.setTime(g_glideSeconds, sampleRate);
		m_glide.setTime(g_glideSeconds, sampleRate);
		m_wet.setTime(g_smoothingSeconds, sampleRate);
		m_damp.setTime(g_smoothingSeconds, sampleRate);
		reset();
		update();
	}

	void Reverb::reset() noexcept
	{
		for (Channel& channel : m_channels)
		{
			for (Comb& comb : channel.combs)
			{
				std::fill(comb.line.buffer.begin(), comb.line.buffer.end(), 0.0f);
				comb.line.write = 0;
				comb.from = comb.to;
				comb.filter = 0.0f;
			}
			for (Allpass& allpass : channel.allpasses)
			{
				std::fill(allpass.buffer.begin(), allpass.buffer.end(), 0.0f);
				allpass.position = 0;
			}
		}
		std::fill(m_predelay.buffer.begin(), m_predelay.buffer.end(), 0.0f);
		m_predelay.write = 0;
		m_predelayFrames.finish();
		m_glide.finish();
		m_wet.finish();
		m_damp.finish();
		m_fresh = true;
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
		const bool glide = !m_fresh;
		m_damp.set(m_damping / 100.0 * g_maxDamp, glide);
		m_wet.set(m_mix, glide);

		// The combs' new settings. If any changed while running, every comb glides from where it is now.
		const double rate = m_sampleRate / g_tuningRate * scale(m_size);
		std::array<std::array<CombSettings, g_combs>, 2> settings{};
		bool moved = false;
		for (size_t ch = 0; ch < m_channels.size(); ++ch)
		{
			const int spread = static_cast<int>(ch) * g_stereoSpread;
			for (size_t k = 0; k < m_channels[ch].combs.size(); ++k)
			{
				const Comb& comb = m_channels[ch].combs[k];
				size_t length = frames((g_combTunings[k] + spread) * rate);
				if (!comb.line.buffer.empty())
					length = std::min(length, comb.line.buffer.size() - 2);
				CombSettings& next = settings[ch][k];
				next.length = static_cast<double>(length);
				// Falls by 60 dB in m_decay seconds: g^(decay * fs / length) = 10^-3.
				next.feedback = std::pow(10.0, -3.0 * next.length / (m_decay * m_sampleRate));
				next.inputGain = std::sqrt(1.0 - (next.feedback * next.feedback));
				moved = moved || next != comb.to;
			}
		}
		const double progress = m_glide.value();
		if (moved)
			for (size_t ch = 0; ch < m_channels.size(); ++ch)
				for (size_t k = 0; k < m_channels[ch].combs.size(); ++k)
				{
					Comb& comb = m_channels[ch].combs[k];
					comb.from = glide && progress < 1.0 ? comb.from.between(comb.to, progress) : comb.to;
					comb.to = settings[ch][k];
					if (!glide)
						comb.from = comb.to;
				}
		if (moved && glide)
		{
			m_glide.jumpTo(0.0);
			m_glide.setTarget(1.0);
		}
		else if (moved)
		{
			m_glide.jumpTo(1.0);
		}

		double predelay = std::round(m_predelayMs / 1000.0 * m_sampleRate);
		if (!m_predelay.buffer.empty())
			predelay = std::min(predelay, static_cast<double>(m_predelay.buffer.size() - 3));
		m_predelayFrames.set(predelay, glide);

		const double samples = ((m_predelayMs / 1000.0) + (1.5 * m_decay)) * m_sampleRate;
		m_tail.store(static_cast<uint32_t>(
						 std::min(std::ceil(samples), static_cast<double>(std::numeric_limits<uint32_t>::max() - 1))),
			std::memory_order_relaxed);
	}

	float Reverb::processChannel(Channel& channel, float input, double glide, float damp) noexcept
	{
		float sum = 0.0f;
		for (Comb& comb : channel.combs)
		{
			const CombSettings now = glide >= 1.0 ? comb.to : comb.from.between(comb.to, glide);
			const float out = comb.line.read(now.length);
			comb.filter = flushDenormal((out * (1.0f - damp)) + (comb.filter * damp));
			comb.line.push(flushDenormal(
				(input * static_cast<float>(now.inputGain)) + (comb.filter * static_cast<float>(now.feedback))));
			sum += out;
		}
		float out = sum * g_combsGain;
		for (Allpass& allpass : channel.allpasses)
		{
			// Schroeder's allpass: v[n] = x[n] + g v[n - D], y[n] = v[n - D] - g v[n]. Unlike Freeverb's
			// approximation its gain is 1 at every frequency.
			float& slot = allpass.buffer[allpass.position];
			const float delayed = slot;
			const float v = flushDenormal(out + (delayed * g_allpassFeedback));
			slot = v;
			allpass.position = allpass.position + 1 == allpass.buffer.size() ? 0 : allpass.position + 1;
			out = delayed - (g_allpassFeedback * v);
		}
		return out;
	}

	void Reverb::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		if (m_predelay.buffer.empty()) // not prepared
			return;
		const int count = std::min(channels, 2);
		m_fresh = m_fresh && begin == end;
		for (uint32_t i = begin; i < end; ++i)
		{
			const float left = audio[0][i];
			m_predelay.push(count == 2 ? 0.5f * (left + audio[1][i]) : left);
			const float input = m_predelay.read(m_predelayFrames.next() + 1.0);
			const double glide = m_glide.next();
			const auto damp = static_cast<float>(m_damp.next());
			const double mix = m_wet.next();
			const auto wet = static_cast<float>(mix);
			const auto dry = static_cast<float>(1.0 - mix);
			audio[0][i] = (dry * left) + (wet * processChannel(m_channels[0], input, glide, damp));
			if (count == 2)
				audio[1][i] = (dry * audio[1][i]) + (wet * processChannel(m_channels[1], input, glide, damp));
		}
	}
}
