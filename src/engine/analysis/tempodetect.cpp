// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/analysis/tempodetect.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

namespace gnubase::engine::analysis
{
	namespace
	{
		constexpr double g_frameRate = 200.0; // onset envelope frames per second
		constexpr double g_blur = 2.0;        // frames: the envelope's Gaussian, for timing that wavers
		constexpr double g_slowest = 40.0;    // BPM
		constexpr double g_fastest = 240.0;   // BPM
		constexpr double g_preferred = 120.0; // BPM, where the period's weighting peaks
		constexpr double g_octaves = 1.0;     // its width
		constexpr double g_tightness = 100.0; // how much beat tracking keeps to the period
		constexpr double g_snap = 0.125;      // of a beat: how far a beat moves onto a pulse
		constexpr double g_secondsPerMinute = 60.0;

		// The onset envelope: the pulses' weights at their frames, blurred and scaled to a standard deviation of 1.
		std::vector<double> envelope(const std::vector<Pulse>& pulses, size_t frames)
		{
			std::vector<double> impulses(frames, 0.0);
			for (const Pulse& pulse : pulses)
			{
				const auto frame = std::llround(pulse.time * g_frameRate);
				if (frame >= 0 && static_cast<size_t>(frame) < frames)
					impulses[static_cast<size_t>(frame)] += std::max(pulse.weight, 0.0);
			}
			const auto radius = static_cast<int64_t>(3.0 * g_blur);
			std::vector<double> kernel;
			for (int64_t k = -radius; k <= radius; ++k)
				kernel.push_back(std::exp(-0.5 * static_cast<double>(k * k) / (g_blur * g_blur)));
			std::vector<double> blurred(frames, 0.0);
			double sum = 0.0;
			for (size_t n = 0; n < frames; ++n)
			{
				if (impulses[n] == 0.0)
					continue;
				for (int64_t k = -radius; k <= radius; ++k)
				{
					const auto at = static_cast<int64_t>(n) + k;
					if (at >= 0 && static_cast<size_t>(at) < frames)
						blurred[static_cast<size_t>(at)] += impulses[n] * kernel[static_cast<size_t>(k + radius)];
				}
			}
			for (const double value : blurred)
				sum += value * value;
			const double deviation = std::sqrt(sum / static_cast<double>(frames));
			if (deviation > 0.0)
				for (double& value : blurred)
					value /= deviation;
			return blurred;
		}

		// The beat period in frames: the lag where the envelope's autocorrelation, weighted towards the preferred
		// tempo, is highest, refined between frames.
		double period(const std::vector<double>& envelope)
		{
			const auto shortest = static_cast<size_t>(g_frameRate * g_secondsPerMinute / g_fastest);
			const auto longest = std::min(static_cast<size_t>(g_frameRate * g_secondsPerMinute / g_slowest),
				envelope.size() > 1 ? envelope.size() - 1 : 0);
			if (longest <= shortest + 1)
				return 0.0;
			const double preferred = g_frameRate * g_secondsPerMinute / g_preferred;
			std::vector<double> weighted(longest + 2, 0.0);
			for (size_t lag = shortest - 1; lag <= longest + 1 && lag < envelope.size(); ++lag)
			{
				double sum = 0.0;
				for (size_t n = 0; n + lag < envelope.size(); ++n)
					sum += envelope[n] * envelope[n + lag];
				const double octaves = std::log2(static_cast<double>(lag) / preferred) / g_octaves;
				weighted[lag] = sum / static_cast<double>(envelope.size() - lag) * std::exp(-0.5 * octaves * octaves);
			}
			size_t best = shortest;
			for (size_t lag = shortest; lag <= longest; ++lag)
				if (weighted[lag] > weighted[best])
					best = lag;
			// The parabola through the best lag and its neighbours.
			const double left = weighted[best - 1];
			const double right = weighted[best + 1];
			const double curvature = left - (2.0 * weighted[best]) + right;
			const double shift = curvature < 0.0 ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
			return static_cast<double>(best) + shift;
		}

		// The beat frames that fit the envelope and the period best (dynamic programming, as in D. Ellis, "Beat
		// Tracking by Dynamic Programming", 2007): each frame's score is its envelope plus the best score of a beat
		// half a period to two periods before, less the squared log of how far that is from the period.
		std::vector<size_t> track(const std::vector<double>& envelope, double beat)
		{
			const size_t frames = envelope.size();
			std::vector<double> score(frames, 0.0);
			std::vector<int64_t> previous(frames, -1);
			const auto nearest = std::max<int64_t>(1, std::llround(beat / 2.0));
			const auto farthest = std::llround(beat * 2.0);
			for (size_t n = 0; n < frames; ++n)
			{
				double best = 0.0; // or a new start
				for (int64_t gap = nearest; gap <= farthest && gap <= static_cast<int64_t>(n); ++gap)
				{
					const double off = std::log(static_cast<double>(gap) / beat);
					const double candidate = score[n - static_cast<size_t>(gap)] - (g_tightness * off * off);
					if (candidate > best)
					{
						best = candidate;
						previous[n] = static_cast<int64_t>(n) - gap;
					}
				}
				score[n] = envelope[n] + best;
			}
			// From the best frame in the last period back.
			const auto last = frames - std::min(frames, static_cast<size_t>(std::ceil(beat)));
			const auto end = std::max_element(score.begin() + static_cast<std::ptrdiff_t>(last), score.end());
			std::vector<size_t> beats;
			for (auto n = static_cast<int64_t>(end - score.begin()); n >= 0; n = previous[static_cast<size_t>(n)])
				beats.push_back(static_cast<size_t>(n));
			std::reverse(beats.begin(), beats.end());
			return beats;
		}
	}

	std::vector<double> detectBeats(const std::vector<Pulse>& pulses, double length)
	{
		const auto frames = static_cast<size_t>(std::max(0.0, std::floor(length * g_frameRate))) + 1;
		const std::vector<double> onsets = envelope(pulses, frames);
		const double beat = period(onsets);
		if (beat <= 0.0)
			return {};
		std::vector<double> times;
		times.reserve(pulses.size());
		for (const Pulse& pulse : pulses)
			times.push_back(pulse.time);
		std::sort(times.begin(), times.end());
		// Each beat moves onto the nearest pulse, if it is near; those before the first such beat and after the last
		// are left out.
		std::vector<double> beats;
		std::vector<bool> onPulse;
		const double snap = g_snap * beat / g_frameRate;
		for (const size_t frame : track(onsets, beat))
		{
			const double at = static_cast<double>(frame) / g_frameRate;
			const auto next = std::lower_bound(times.begin(), times.end(), at);
			double closest = snap;
			std::optional<double> snapped;
			if (next != times.end() && *next - at <= closest)
			{
				closest = *next - at;
				snapped = *next;
			}
			if (next != times.begin() && at - *std::prev(next) <= closest)
				snapped = *std::prev(next);
			const double placed = snapped.value_or(at);
			if (!beats.empty() && placed <= beats.back())
				continue;
			beats.push_back(placed);
			onPulse.push_back(snapped.has_value());
		}
		const auto first = std::find(onPulse.begin(), onPulse.end(), true) - onPulse.begin();
		const auto last = onPulse.rend() - std::find(onPulse.rbegin(), onPulse.rend(), true);
		if (last - first < 2)
			return {};
		return {beats.begin() + first, beats.begin() + last};
	}

	std::vector<double> rescaleBeats(const std::vector<double>& beats, double factor, bool offbeat)
	{
		if (beats.size() < 2 || factor <= 0.0)
			return beats;
		const auto last = static_cast<double>(beats.size() - 1);
		std::vector<double> rescaled;
		for (int64_t k = 0;; ++k)
		{
			const double x = (static_cast<double>(k) + (offbeat ? 0.5 : 0.0)) / factor;
			if (x > last + 1e-9)
				break;
			const auto i = std::min(static_cast<size_t>(x), beats.size() - 2);
			rescaled.push_back(beats[i] + ((x - static_cast<double>(i)) * (beats[i + 1] - beats[i])));
		}
		return rescaled;
	}

	std::vector<double> smoothBeats(const std::vector<double>& beats)
	{
		if (beats.size() < 2)
			return beats;
		const auto count = static_cast<double>(beats.size());
		double sumX = 0.0;
		double sumY = 0.0;
		double sumXX = 0.0;
		double sumXY = 0.0;
		for (size_t i = 0; i < beats.size(); ++i)
		{
			const auto x = static_cast<double>(i);
			sumX += x;
			sumY += beats[i];
			sumXX += x * x;
			sumXY += x * beats[i];
		}
		const double slope = ((count * sumXY) - (sumX * sumY)) / ((count * sumXX) - (sumX * sumX));
		const double start = (sumY - (slope * sumX)) / count;
		std::vector<double> smoothed;
		smoothed.reserve(beats.size());
		for (size_t i = 0; i < beats.size(); ++i)
			smoothed.push_back(start + (slope * static_cast<double>(i)));
		return smoothed;
	}
}
