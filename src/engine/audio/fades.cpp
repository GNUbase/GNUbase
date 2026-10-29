// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/fades.h"

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
}
