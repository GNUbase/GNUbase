// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

namespace gnubase::engine::dsp
{
	// The coefficients of a second-order section, normalized so that a0 is 1:
	//   H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
	// First-order sections have b2 = a2 = 0.
	struct Biquad
	{
		double b0 = 1.0;
		double b1 = 0.0;
		double b2 = 0.0;
		double a1 = 0.0;
		double a2 = 0.0;

		// The designs of the Audio EQ Cookbook (R. Bristow-Johnson): the bilinear transform of the analog prototype,
		// with the frequency prewarped, so the digital response at f is the analog one at tan(pi f / fs) /
		// tan(pi f0 / fs). frequency is clamped below the Nyquist frequency.
		static Biquad peaking(double sampleRate, double frequency, double q, double gainDb);
		static Biquad lowShelf(double sampleRate, double frequency, double q, double gainDb);
		static Biquad highShelf(double sampleRate, double frequency, double q, double gainDb);
		static Biquad highPass(double sampleRate, double frequency, double q);
		static Biquad lowPass(double sampleRate, double frequency, double q);
		// First-order Butterworth sections: s / (s + 1) and 1 / (s + 1), bilinear with prewarping.
		static Biquad highPass1(double sampleRate, double frequency);
		static Biquad lowPass1(double sampleRate, double frequency);
	};

	// The state of one section on one channel: transposed direct form II, in double precision.
	struct BiquadState
	{
		double z1 = 0.0;
		double z2 = 0.0;

		double process(const Biquad& c, double x) noexcept
		{
			const double y = (c.b0 * x) + z1;
			z1 = (c.b1 * x) - (c.a1 * y) + z2;
			z2 = (c.b2 * x) - (c.a2 * y);
			return y;
		}

		void flush() noexcept
		{
			z1 = flushDenormal(z1);
			z2 = flushDenormal(z2);
		}
	};
}
