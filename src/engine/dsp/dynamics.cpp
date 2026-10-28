// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/dynamics.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::dsp
{
	namespace
	{
		// The highest square of the first channels at frame i.
		double peakSquare(float* const* audio, int channels, uint32_t i) noexcept
		{
			double peak = 0.0;
			for (int c = 0; c < channels; ++c)
			{
				const double x = audio[c][i];
				peak = std::max(peak, x * x);
			}
			return peak;
		}

		// Moves state toward target with the coefficient.
		double follow(double state, double target, double coefficient) noexcept
		{
			return target + ((state - target) * coefficient);
		}
	}

	// ---- Gate ----

	void Gate::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		update();
		reset();
	}

	void Gate::reset() noexcept
	{
		m_gain = m_range;
		m_holding = 0;
	}

	void Gate::setParameter(uint32_t id, double value) noexcept
	{
		switch (id)
		{
		case Threshold:
			m_thresholdDb = clampParameter(value, -80.0, 0.0);
			break;
		case Range:
			m_rangeDb = clampParameter(value, -80.0, 0.0);
			break;
		case Attack:
			m_attackMs = clampParameter(value, 0.01, 100.0);
			break;
		case Hold:
			m_holdMs = clampParameter(value, 0.0, 2000.0);
			break;
		case Release:
			m_releaseMs = clampParameter(value, 1.0, 4000.0);
			break;
		default:
			return;
		}
		update();
	}

	void Gate::update() noexcept
	{
		m_threshold = dbToGain(m_thresholdDb);
		m_range = dbToGain(m_rangeDb);
		m_attack = smoothingCoefficient(m_attackMs / 1000.0, m_sampleRate);
		m_release = smoothingCoefficient(m_releaseMs / 1000.0, m_sampleRate);
		m_holdFrames = static_cast<uint64_t>(std::llround(m_holdMs / 1000.0 * m_sampleRate));
	}

	void Gate::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		const int count = std::min(channels, g_maxChannels);
		const double threshold = m_threshold * m_threshold;
		for (uint32_t i = begin; i < end; ++i)
		{
			bool open = false;
			if (peakSquare(audio, count, i) >= threshold)
			{
				open = true;
				m_holding = m_holdFrames;
			}
			else if (m_holding > 0)
			{
				open = true;
				--m_holding;
			}
			const double target = open ? 1.0 : m_range;
			m_gain = follow(m_gain, target, target > m_gain ? m_attack : m_release);
			const auto gain = static_cast<float>(m_gain);
			for (int c = 0; c < count; ++c)
				audio[c][i] = flushDenormal(audio[c][i] * gain);
		}
	}

	// ---- Compressor ----

	double Compressor::curve(double inputDb, double thresholdDb, double ratio, double kneeDb)
	{
		const double over = inputDb - thresholdDb;
		if (2.0 * over < -kneeDb)
			return inputDb;
		if (kneeDb > 0.0 && 2.0 * std::abs(over) <= kneeDb)
		{
			const double into = over + (kneeDb / 2.0);
			return inputDb + (((1.0 / ratio) - 1.0) * into * into / (2.0 * kneeDb));
		}
		return thresholdDb + (over / ratio);
	}

	void Compressor::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		update();
		reset();
	}

	void Compressor::reset() noexcept
	{
		m_meanSquare = 0.0;
		m_reduction = 0.0;
	}

	void Compressor::setParameter(uint32_t id, double value) noexcept
	{
		switch (id)
		{
		case Threshold:
			m_thresholdDb = clampParameter(value, -60.0, 0.0);
			break;
		case Ratio:
			m_ratio = clampParameter(value, 1.0, 20.0);
			break;
		case Knee:
			m_kneeDb = clampParameter(value, 0.0, 24.0);
			break;
		case Attack:
			m_attackMs = clampParameter(value, 0.1, 100.0);
			break;
		case Release:
			m_releaseMs = clampParameter(value, 10.0, 2000.0);
			break;
		case Makeup:
			m_makeupDb = clampParameter(value, 0.0, 24.0);
			break;
		case Detection:
			m_detection = clampParameter(value, 0.0, 1.0, true) != 0.0 ? DetectionMode::Rms : DetectionMode::Peak;
			break;
		default:
			return;
		}
		update();
	}

	void Compressor::update() noexcept
	{
		m_attack = smoothingCoefficient(m_attackMs / 1000.0, m_sampleRate);
		m_release = smoothingCoefficient(m_releaseMs / 1000.0, m_sampleRate);
		m_rmsCoefficient = smoothingCoefficient(0.01, m_sampleRate);
	}

	void Compressor::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		const int count = std::min(channels, g_maxChannels);
		for (uint32_t i = begin; i < end; ++i)
		{
			double square = peakSquare(audio, count, i);
			if (m_detection == DetectionMode::Rms)
			{
				m_meanSquare = flushDenormal(follow(m_meanSquare, square, m_rmsCoefficient));
				square = m_meanSquare;
			}
			const double levelDb = 10.0 * std::log10(std::max(square, 1e-30));
			const double target = curve(levelDb, m_thresholdDb, m_ratio, m_kneeDb) - levelDb;
			m_reduction = flushDenormal(follow(m_reduction, target, target < m_reduction ? m_attack : m_release));
			const auto gain = static_cast<float>(dbToGain(m_reduction + m_makeupDb));
			for (int c = 0; c < count; ++c)
				audio[c][i] = flushDenormal(audio[c][i] * gain);
		}
	}

	// ---- Limiter ----

	void Limiter::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		m_lookahead = static_cast<uint32_t>(std::max(1L, std::lround(g_lookaheadSeconds * sampleRate)));
		m_window = m_lookahead + 1;
		m_delay.assign(static_cast<size_t>(g_maxChannels) * m_lookahead, 0.0f);
		m_average.assign(m_window, 1.0);
		m_minValues.assign(m_window, 1.0);
		m_minExpiry.assign(m_window, 0);
		update();
		reset();
	}

	void Limiter::reset() noexcept
	{
		std::fill(m_delay.begin(), m_delay.end(), 0.0f);
		std::fill(m_average.begin(), m_average.end(), 1.0);
		m_sum = static_cast<double>(m_window);
		m_position = 0;
		m_minFront = 0;
		m_minSize = 0;
		m_frame = 0;
		m_released = 1.0;
	}

	void Limiter::setParameter(uint32_t id, double value) noexcept
	{
		switch (id)
		{
		case Input:
			m_inputDb = clampParameter(value, 0.0, 24.0);
			break;
		case Ceiling:
			m_ceilingDb = clampParameter(value, -30.0, 0.0);
			break;
		case Release:
			m_releaseMs = clampParameter(value, 1.0, 1000.0);
			break;
		default:
			return;
		}
		update();
	}

	void Limiter::update() noexcept
	{
		m_input = dbToGain(m_inputDb);
		m_ceiling = dbToGain(m_ceilingDb);
		m_release = smoothingCoefficient(m_releaseMs / 1000.0, m_sampleRate);
	}

	void Limiter::process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept
	{
		if (m_lookahead == 0) // not prepared
			return;
		const int count = std::min(channels, g_maxChannels);
		const auto input = static_cast<float>(m_input);
		const auto ceiling = static_cast<float>(m_ceiling);
		const uint32_t window = m_window;
		for (uint32_t i = begin; i < end; ++i)
		{
			// The gain this frame needs once it leaves the delay.
			double peak = 0.0;
			for (int c = 0; c < count; ++c)
				peak = std::max(peak, static_cast<double>(std::abs(audio[c][i] * input)));
			const double required = peak > m_ceiling ? m_ceiling / peak : 1.0;

			// The minimum over the last L + 1 frames.
			while (m_minSize > 0 && m_minValues[(m_minFront + m_minSize - 1) % window] >= required)
				--m_minSize;
			const uint32_t back = (m_minFront + m_minSize) % window;
			m_minValues[back] = required;
			m_minExpiry[back] = m_frame + window;
			++m_minSize;
			while (m_minExpiry[m_minFront] <= m_frame)
			{
				m_minFront = (m_minFront + 1) % window;
				--m_minSize;
			}
			const double minimum = m_minValues[m_minFront];

			// Down at once, up with the release; then averaged over the window.
			m_released = minimum < m_released ? minimum : follow(m_released, minimum, m_release);
			const auto slot = static_cast<uint32_t>(m_frame % window);
			m_sum += m_released - m_average[slot];
			m_average[slot] = m_released;
			if (slot == window - 1) // recompute now and then, so rounding errors don't add up
			{
				m_sum = 0.0;
				for (const double value : m_average)
					m_sum += value;
			}
			const auto gain = static_cast<float>(m_sum / window);

			for (int c = 0; c < count; ++c)
			{
				float& delayed = m_delay[(static_cast<size_t>(c) * m_lookahead) + m_position];
				const float out = delayed * gain;
				delayed = audio[c][i] * input;
				audio[c][i] = std::clamp(flushDenormal(out), -ceiling, ceiling);
			}
			m_position = m_position + 1 == m_lookahead ? 0 : m_position + 1;
			++m_frame;
		}
	}
}
