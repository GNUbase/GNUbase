// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/fades.h"

#include "engine/audio/automation.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine
{
	namespace
	{
		constexpr double g_pi = 3.14159265358979323846;

		double exponential(double x) noexcept
		{
			return std::expm1(g_fadeCurvature * x) / std::expm1(g_fadeCurvature);
		}
	}

	double fadeGain(FadeShape shape, double x) noexcept
	{
		x = std::clamp(x, 0.0, 1.0);
		switch (shape)
		{
		case FadeShape::Linear:
			return x;
		case FadeShape::Exponential:
			return exponential(x);
		case FadeShape::Logarithmic:
			return 1.0 - exponential(1.0 - x);
		case FadeShape::SCurve:
			return (1.0 - std::cos(g_pi * x)) / 2.0;
		case FadeShape::EqualPower:
			return std::sin(g_pi * x / 2.0);
		}
		return x; // not reached: every shape is handled
	}

	bool mirrorShape(FadeShape shape, FadeShape& mirror) noexcept
	{
		switch (shape)
		{
		case FadeShape::Linear:
		case FadeShape::SCurve:
			mirror = shape;
			return true;
		case FadeShape::Exponential:
			mirror = FadeShape::Logarithmic;
			return true;
		case FadeShape::Logarithmic:
			mirror = FadeShape::Exponential;
			return true;
		case FadeShape::EqualPower:
			break;
		}
		return false;
	}

	namespace
	{
		// The first point after position.
		std::vector<GainPoint>::const_iterator after(const std::vector<GainPoint>& points, double position) noexcept
		{
			return std::upper_bound(points.begin(), points.end(), position,
				[](double at, const GainPoint& point) { return at < static_cast<double>(point.position); });
		}
	}

	double curveGain(const std::vector<GainPoint>& points, double position) noexcept
	{
		if (points.empty())
			return 1.0;
		const auto next = after(points, position);
		if (next == points.begin())
			return next->gain;
		if (next == points.end())
			return points.back().gain;
		const GainPoint& from = *(next - 1);
		const double x =
			(position - static_cast<double>(from.position)) / static_cast<double>(next->position - from.position);
		return from.gain + ((next->gain - from.gain) * automationBend(x, from.curve));
	}

	bool curveChanges(const std::vector<GainPoint>& points, time::Samples position, time::Samples frames) noexcept
	{
		if (points.empty())
			return false;
		// Between points the gain only rises or falls, so it is level where no point is inside and both ends agree.
		const auto next = after(points, static_cast<double>(position));
		return (next != points.end() && next->position < position + frames) ||
			curveGain(points, static_cast<double>(position)) !=
			curveGain(points, static_cast<double>(position + frames));
	}

	std::vector<GainPoint> gainPoints(const std::vector<VolumePoint>& points)
	{
		std::vector<GainPoint> result;
		result.reserve(points.size());
		for (const VolumePoint& point : points)
			result.push_back({point.position, std::pow(10.0, point.volume / 20.0), point.curve});
		return result;
	}

	double volumeDb(double gain)
	{
		return gain > 0.0 ? std::clamp(20.0 * std::log10(gain), g_minEventVolume, g_maxEventVolume) : g_minEventVolume;
	}

	std::vector<VolumePoint> cutVolumeCurve(
		const std::vector<VolumePoint>& points, time::Samples from, time::Samples to)
	{
		std::vector<VolumePoint> result;
		if (points.empty())
			return result;
		const std::vector<GainPoint> gains = gainPoints(points);
		const auto add = [&](time::Samples position)
		{ result.push_back({position, volumeDb(curveGain(gains, static_cast<double>(position))), 0.0}); };
		if (std::none_of(
				points.begin(), points.end(), [from](const VolumePoint& point) { return point.position == from; }))
			add(from);
		for (const VolumePoint& point : points)
			if (point.position >= from && point.position <= to)
				result.push_back(point);
		if (result.back().position != to)
			add(to);
		// Each point bends like the part of the segment it was in, up to the next point: a part from a to b of a
		// segment (as fractions of it) is automationBend's exponential again, with curve * (b - a).
		for (size_t i = 0; i + 1 < result.size(); ++i)
		{
			const time::Samples q = result[i].position;
			const time::Samples r = result[i + 1].position;
			const auto next = std::upper_bound(points.begin(), points.end(), q,
				[](time::Samples at, const VolumePoint& point) { return at < point.position; });
			if (r == q || next == points.begin() || next == points.end())
				continue;
			const VolumePoint& start = *(next - 1);
			result[i].curve =
				start.curve * static_cast<double>(r - q) / static_cast<double>(next->position - start.position);
		}
		return result;
	}
}
