// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/biquad.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::dsp
{
	namespace
	{
		// The angular frequency of f, kept clear of 0 and the Nyquist frequency, where the designs degenerate.
		double omega(double sampleRate, double frequency)
		{
			const double f = std::clamp(frequency, 1.0, 0.49 * sampleRate);
			return 2.0 * g_pi * f / sampleRate;
		}

		Biquad normalize(double b0, double b1, double b2, double a0, double a1, double a2)
		{
			return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
		}

		double positiveQ(double q)
		{
			return std::max(q, 0.01);
		}
	}

	Biquad Biquad::peaking(double sampleRate, double frequency, double q, double gainDb)
	{
		const double a = std::pow(10.0, gainDb / 40.0);
		const double w = omega(sampleRate, frequency);
		const double alpha = std::sin(w) / (2.0 * positiveQ(q));
		const double cosw = std::cos(w);
		return normalize(
			1.0 + (alpha * a), -2.0 * cosw, 1.0 - (alpha * a), 1.0 + (alpha / a), -2.0 * cosw, 1.0 - (alpha / a));
	}

	Biquad Biquad::lowShelf(double sampleRate, double frequency, double q, double gainDb)
	{
		const double a = std::pow(10.0, gainDb / 40.0);
		const double w = omega(sampleRate, frequency);
		const double cosw = std::cos(w);
		const double beta = 2.0 * std::sqrt(a) * std::sin(w) / (2.0 * positiveQ(q)); // 2 sqrt(A) alpha
		return normalize(a * ((a + 1.0) - ((a - 1.0) * cosw) + beta), 2.0 * a * ((a - 1.0) - ((a + 1.0) * cosw)),
			a * ((a + 1.0) - ((a - 1.0) * cosw) - beta), (a + 1.0) + ((a - 1.0) * cosw) + beta,
			-2.0 * ((a - 1.0) + ((a + 1.0) * cosw)), (a + 1.0) + ((a - 1.0) * cosw) - beta);
	}

	Biquad Biquad::highShelf(double sampleRate, double frequency, double q, double gainDb)
	{
		const double a = std::pow(10.0, gainDb / 40.0);
		const double w = omega(sampleRate, frequency);
		const double cosw = std::cos(w);
		const double beta = 2.0 * std::sqrt(a) * std::sin(w) / (2.0 * positiveQ(q));
		return normalize(a * ((a + 1.0) + ((a - 1.0) * cosw) + beta), -2.0 * a * ((a - 1.0) + ((a + 1.0) * cosw)),
			a * ((a + 1.0) + ((a - 1.0) * cosw) - beta), (a + 1.0) - ((a - 1.0) * cosw) + beta,
			2.0 * ((a - 1.0) - ((a + 1.0) * cosw)), (a + 1.0) - ((a - 1.0) * cosw) - beta);
	}

	Biquad Biquad::highPass(double sampleRate, double frequency, double q)
	{
		const double w = omega(sampleRate, frequency);
		const double alpha = std::sin(w) / (2.0 * positiveQ(q));
		const double cosw = std::cos(w);
		return normalize((1.0 + cosw) / 2.0, -(1.0 + cosw), (1.0 + cosw) / 2.0, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
	}

	Biquad Biquad::lowPass(double sampleRate, double frequency, double q)
	{
		const double w = omega(sampleRate, frequency);
		const double alpha = std::sin(w) / (2.0 * positiveQ(q));
		const double cosw = std::cos(w);
		return normalize((1.0 - cosw) / 2.0, 1.0 - cosw, (1.0 - cosw) / 2.0, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
	}

	Biquad Biquad::highPass1(double sampleRate, double frequency)
	{
		// s / (s + 1) with s = (1 - z^-1) / (k (1 + z^-1)), k = tan(w / 2).
		const double k = std::tan(omega(sampleRate, frequency) / 2.0);
		return normalize(1.0, -1.0, 0.0, 1.0 + k, k - 1.0, 0.0);
	}

	Biquad Biquad::lowPass1(double sampleRate, double frequency)
	{
		const double k = std::tan(omega(sampleRate, frequency) / 2.0);
		return normalize(k, k, 0.0, 1.0 + k, k - 1.0, 0.0);
	}
}
