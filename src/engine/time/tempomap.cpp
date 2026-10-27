// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/time/tempomap.h"

#include <algorithm>

#include <cmath>
#include <stdexcept>

namespace gnubase::time
{
	namespace
	{
		double toBeats(Ticks ticks)
		{
			return static_cast<double>(ticks) / static_cast<double>(g_ticksPerQuarter);
		}
	}

	TempoMap::TempoMap(double bpm) : TempoMap(std::vector<TempoPoint>{{0, bpm, false}})
	{
	}

	TempoMap::TempoMap(const std::vector<TempoPoint>& points)
	{
		if (points.empty() || points.front().position != 0)
			throw std::invalid_argument("a tempo map starts with a point at position 0");
		m_segments.reserve(points.size());
		for (const TempoPoint& point : points)
		{
			const bool inRange = point.bpm >= g_minBpm && point.bpm <= g_maxBpm; // false for NaN
			if (!inRange)
				throw std::invalid_argument("tempo out of range");
			if (!m_segments.empty())
			{
				Segment& previous = m_segments.back();
				if (point.position <= previous.start)
					throw std::invalid_argument("tempo points must be in increasing order");
				if (point.ramp)
					previous.slope = (point.bpm - previous.bpm) / toBeats(point.position - previous.start);
			}
			// The segments up to this point are complete, so toSeconds can already measure it.
			const double seconds = m_segments.empty() ? 0.0 : toSeconds(point.position);
			m_segments.push_back({point.position, seconds, point.bpm, 0.0});
		}
	}

	double TempoMap::toSeconds(Ticks tick) const
	{
		const Segment& segment = detail::lastAtOrBefore(m_segments, &Segment::start, tick);
		const double beats = toBeats(tick - segment.start);
		if (segment.slope == 0.0 || beats < 0.0)
			return segment.seconds + (beats * 60.0 / segment.bpm);
		// The tempo grows linearly per beat, so the time is the integral of 60 / tempo.
		return segment.seconds + (60.0 / segment.slope * std::log1p(segment.slope * beats / segment.bpm));
	}

	Ticks TempoMap::toTicks(double seconds) const
	{
		const Segment& segment = detail::lastAtOrBefore(m_segments, &Segment::seconds, seconds);
		const double offset = seconds - segment.seconds;
		const double beats = segment.slope == 0.0 || offset < 0.0
			? offset * segment.bpm / 60.0
			: segment.bpm / segment.slope * std::expm1(segment.slope * offset / 60.0);
		return segment.start + std::llround(beats * static_cast<double>(g_ticksPerQuarter));
	}

	Samples TempoMap::toSamples(Ticks tick, double sampleRate) const
	{
		return std::llround(toSeconds(tick) * sampleRate);
	}

	Ticks TempoMap::toTicks(Samples sample, double sampleRate) const
	{
		return toTicks(static_cast<double>(sample) / sampleRate);
	}

	bool TempoMap::operator==(const TempoMap& other) const
	{
		return std::equal(m_segments.begin(), m_segments.end(), other.m_segments.begin(), other.m_segments.end(),
			[](const Segment& a, const Segment& b)
			{ return a.start == b.start && a.seconds == b.seconds && a.bpm == b.bpm && a.slope == b.slope; });
	}

	double TempoMap::bpmAt(Ticks tick) const
	{
		const Segment& segment = detail::lastAtOrBefore(m_segments, &Segment::start, tick);
		const double beats = toBeats(tick - segment.start);
		return beats < 0.0 ? segment.bpm : segment.bpm + (segment.slope * beats);
	}
}
