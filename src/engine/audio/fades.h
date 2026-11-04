// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <vector>

// Fade curves and event volume curves. Real-time safe: the audio thread evaluates them for every sample of a fade or
// a volume curve.
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

	// A point of a volume curve as it plays: at position, gain (linear), bent by curve on the way to the next point.
	struct GainPoint
	{
		time::Samples position = 0;
		double gain = 1.0;
		double curve = 0.0;
	};

	// The gain of a volume curve (points by position) at a position: from point to point it goes from one's gain to
	// the next's along automationBend(x, curve) of the first (engine/audio/automation.h); level with the first point
	// before it and with the last one after it; 1 without points. Points at the same position make a jump.
	double curveGain(const std::vector<GainPoint>& points, double position) noexcept;
	// Whether the curve changes the gain anywhere from position to position + frames.
	bool curveChanges(const std::vector<GainPoint>& points, time::Samples position, time::Samples frames) noexcept;

	// An audio event's volume curve as it plays, in its clip's frames.
	std::vector<GainPoint> gainPoints(const std::vector<VolumePoint>& points);
	// The part of a volume curve (not empty) from one clip frame to another, with points at both ends that keep it as
	// it was there; bends cut exactly.
	std::vector<VolumePoint> cutVolumeCurve(
		const std::vector<VolumePoint>& points, time::Samples from, time::Samples to);
	double volumeDb(double gain); // from g_minEventVolume to g_maxEventVolume
}
