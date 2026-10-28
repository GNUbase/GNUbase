// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

// Fade curves. Real-time safe: the audio thread evaluates them for every sample of a fade.
namespace gnubase::engine
{
	// How strongly the exponential and logarithmic shapes bend: exponential is (e^(kx) - 1) / (e^k - 1).
	inline constexpr double g_fadeCurvature = 3.0;

	// The gain of a fade in at x, from 0 (its start: silence) to 1 (its end: full volume); x is clamped to that.
	// - linear: x
	// - exponential: (e^(kx) - 1) / (e^k - 1) with k = g_fadeCurvature
	// - logarithmic: 1 - exponential(1 - x)
	// - sCurve: (1 - cos(pi x)) / 2
	// - equalPower: sin(pi x / 2)
	// A fade out uses the same curve backwards: its gain at x is fadeGain(shape, 1 - x).
	double fadeGain(FadeShape shape, double x) noexcept;

	// The fade out that mirrors a fade in, so that in an equal-gain crossfade over the same range their gains add up
	// to 1: linear and sCurve mirror themselves, exponential and logarithmic each other. equalPower has none.
	bool mirrorShape(FadeShape shape, FadeShape& mirror) noexcept;
}
