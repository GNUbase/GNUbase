// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Signal processing shared by GNUbase's built-in effects (#114): the cores behind the built-in processors and the
// built-in plugins, and later the channel strip. Cores know nothing of graphs, events or plugins. They are prepared
// off the audio thread and then process blocks in place without allocating, locking or throwing, the same way on
// every call for the same input and parameters (rule 6).
namespace gnubase::engine::dsp
{
	// The most channels a core processes; the most a bus or channel has. Channels beyond it pass unchanged.
	inline constexpr int g_maxChannels = 16;

	inline constexpr double g_pi = 3.14159265358979323846;

	inline double dbToGain(double db)
	{
		return std::pow(10.0, db / 20.0);
	}

	inline double gainToDb(double gain)
	{
		return 20.0 * std::log10(std::max(gain, 1e-30));
	}

	// Values this small are treated as silence, so feedback paths and filter states decay to exact zero instead of
	// through subnormal numbers, which are slow on most CPUs. 1e-30 is about -600 dB, far below any audible level.
	inline double flushDenormal(double value)
	{
		return std::abs(value) < 1e-30 ? 0.0 : value;
	}

	inline float flushDenormal(float value)
	{
		return std::abs(value) < 1e-30f ? 0.0f : value;
	}

	// A one-pole smoothing coefficient: the state covers 1 - 1/e of a step in the given time.
	inline double smoothingCoefficient(double seconds, double sampleRate)
	{
		return seconds <= 0.0 ? 0.0 : std::exp(-1.0 / (seconds * sampleRate));
	}

	// How long gains and mixes take to follow a change: a one-pole glide, so automation doesn't click.
	inline constexpr double g_smoothingSeconds = 0.02;

	// A value that glides to its target with a one-pole low-pass, one step per sample, so changes don't click. Close
	// to the target it snaps onto it, so a settled value is exactly the target and costs nothing.
	class Smoothed
	{
	public:
		// tolerance: how close counts as there, in the value's unit.
		explicit Smoothed(double value = 0.0, double tolerance = 1e-6)
			: m_value(value), m_target(value), m_tolerance(tolerance)
		{
		}

		void setTime(double seconds, double sampleRate) noexcept
		{
			m_coefficient = smoothingCoefficient(seconds, sampleRate);
		}

		// Glides there from the current value.
		void setTarget(double target) noexcept { m_target = target; }

		// Jumps there.
		void jumpTo(double target) noexcept
		{
			m_target = target;
			m_value = target;
		}

		void set(double target, bool glide) noexcept
		{
			if (glide)
				setTarget(target);
			else
				jumpTo(target);
		}

		void finish() noexcept { m_value = m_target; }

		bool settled() const { return m_value == m_target; }
		double value() const { return m_value; }
		double target() const { return m_target; }

		// The value for the next sample.
		double next() noexcept
		{
			if (m_value != m_target)
			{
				m_value = m_target + ((m_value - m_target) * m_coefficient);
				if (std::abs(m_value - m_target) <= m_tolerance)
					m_value = m_target;
			}
			return m_value;
		}

	private:
		double m_value;
		double m_target;
		double m_tolerance;
		double m_coefficient = 0.0; // 0 until setTime: jumps
	};

	// A parameter value clamped to its range, rounded for stepped parameters.
	inline double clampParameter(double value, double minimum, double maximum, bool stepped = false)
	{
		if (std::isnan(value))
			value = minimum;
		value = std::clamp(value, minimum, maximum);
		return stepped ? std::round(value) : value;
	}

	// A deterministic pseudo-random generator (xorshift32): the same seed gives the same numbers on every platform.
	class Random
	{
	public:
		explicit Random(uint32_t seed = 0x9e3779b9u) : m_state(seed == 0 ? 0x9e3779b9u : seed) {}

		uint32_t next() noexcept
		{
			m_state ^= m_state << 13;
			m_state ^= m_state >> 17;
			m_state ^= m_state << 5;
			return m_state;
		}

		// Uniform in [0, 1).
		double uniform() noexcept { return static_cast<double>(next()) * (1.0 / 4294967296.0); }

	private:
		uint32_t m_state;
	};
}
