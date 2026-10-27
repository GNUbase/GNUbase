// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace gnubase::time
{
	// Musical positions and lengths, in ticks.
	using Ticks = int64_t;
	// Linear positions and lengths, in sample frames.
	using Samples = int64_t;

	// 2^14 * 3 * 5 * 7: binary note values, triplets, quintuplets, septuplets and every common
	// display resolution divide a quarter note into whole ticks.
	inline constexpr Ticks g_ticksPerQuarter = 1720320;

	inline constexpr double g_minBpm = 1.0;
	inline constexpr double g_maxBpm = 999.0;

	struct TempoPoint
	{
		Ticks position = 0;
		double bpm = 120.0;
		// true: the tempo changes linearly per beat from the previous point to this one. false: it jumps here.
		bool ramp = false;
	};

	// Converts between musical time (ticks) and linear time (seconds, samples).
	// Musical positions survive a round trip exactly; sample positions do too while a tick is shorter than a sample.
	class TempoMap
	{
	public:
		explicit TempoMap(double bpm);
		// The first point must be at position 0; throws std::invalid_argument for invalid points.
		explicit TempoMap(const std::vector<TempoPoint>& points);

		double toSeconds(Ticks tick) const;
		Ticks toTicks(double seconds) const;
		Samples toSamples(Ticks tick, double sampleRate) const;
		Ticks toTicks(Samples sample, double sampleRate) const;
		double bpmAt(Ticks tick) const;

		bool operator==(const TempoMap& other) const;
		bool operator!=(const TempoMap& other) const { return !(*this == other); }

	private:
		// The tempo from one point to the next. Before the first point, the first tempo holds.
		struct Segment
		{
			Ticks start = 0;
			double seconds = 0.0; // at start
			double bpm = 120.0;   // at start
			double slope = 0.0;   // BPM per quarter note, 0 for a constant tempo
		};

		std::vector<Segment> m_segments;
	};

	// A track's time base: musical positions follow tempo changes, linear positions stay at their time.
	enum class TimeBase : uint8_t
	{
		Musical,
		Linear,
	};

	// A position in ticks (musical) or samples (linear).
	struct Position
	{
		TimeBase base = TimeBase::Musical;
		int64_t value = 0;
	};

	inline bool operator==(const Position& a, const Position& b)
	{
		return a.base == b.base && a.value == b.value;
	}

	inline bool operator!=(const Position& a, const Position& b)
	{
		return !(a == b);
	}

	inline Samples toSamples(const Position& position, const TempoMap& tempo, double sampleRate)
	{
		return position.base == TimeBase::Linear ? position.value : tempo.toSamples(position.value, sampleRate);
	}

	inline Ticks toTicks(const Position& position, const TempoMap& tempo, double sampleRate)
	{
		return position.base == TimeBase::Musical ? position.value : tempo.toTicks(position.value, sampleRate);
	}

	// The same point in time in another time base, as when a track switches its time base.
	inline Position rebase(const Position& position, TimeBase base, const TempoMap& tempo, double sampleRate)
	{
		return {base,
			base == TimeBase::Musical ? toTicks(position, tempo, sampleRate) : toSamples(position, tempo, sampleRate)};
	}

	namespace detail
	{
		// The last item whose key is at or before value, or the first item if there is none.
		template <typename T, typename Key>
		const T& lastAtOrBefore(const std::vector<T>& items, Key T::* key, Key value)
		{
			const auto next = std::upper_bound(
				items.begin(), items.end(), value, [key](Key v, const T& item) { return v < item.*key; });
			return next == items.begin() ? *next : *std::prev(next);
		}
	}
}
