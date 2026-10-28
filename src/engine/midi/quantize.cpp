// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/quantize.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

namespace gnubase::engine::midi
{
	namespace
	{
		// The largest power of two below n: an n-tuplet takes the time of that many steps.
		int tupletSpan(int n)
		{
			int span = 1;
			while (span * 2 < n)
				span *= 2;
			return span;
		}

		int64_t floorDiv(int64_t value, int64_t divisor)
		{
			const int64_t quotient = value / divisor;
			return quotient * divisor > value ? quotient - 1 : quotient;
		}

		// SplitMix64: the same numbers on every platform, unlike the standard library's distributions.
		uint64_t mix(uint64_t value)
		{
			value += 0x9E3779B97F4A7C15ULL;
			value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
			value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
			return value ^ (value >> 31U);
		}

		enum class Edge : uint8_t
		{
			Start,
			End,
		};

		// A random offset from -randomize to randomize that depends only on the seed, the note and the edge, so
		// quantizing the same notes with the same seed always gives the same result.
		time::Ticks randomOffset(const QuantizeSettings& settings, ObjectId note, Edge edge)
		{
			if (settings.randomize <= 0)
				return 0;
			const uint64_t random = mix(settings.seed ^ mix((note * 2) + static_cast<uint64_t>(edge)));
			const auto span = (static_cast<uint64_t>(settings.randomize) * 2) + 1;
			return static_cast<time::Ticks>(random % span) - settings.randomize;
		}

		// Grid positions of one bar: a straight grid of step ticks from the bar's start, every second position
		// swing ticks later.
		void addBar(const time::SignatureMap& signatures, int64_t bar, double step, double swing,
			std::vector<GridPosition>& positions)
		{
			const time::Ticks start = signatures.barStart(bar);
			const auto length = static_cast<double>(signatures.barStart(bar + 1) - start);
			for (int64_t k = 0; static_cast<double>(k) * step < length; ++k)
			{
				const double offset = (static_cast<double>(k) * step) + (k % 2 == 1 ? swing : 0.0);
				positions.push_back({start + std::llround(offset), std::nullopt, std::nullopt});
			}
		}

		std::vector<GridPosition> gridAround(
			const time::SignatureMap& signatures, time::Ticks tick, double step, double swing)
		{
			const int64_t bar = signatures.toBarsBeats(std::max<time::Ticks>(tick, 0)).bar;
			std::vector<GridPosition> positions;
			for (int64_t b = std::max<int64_t>(bar - 1, 0); b <= bar + 1; ++b)
				addBar(signatures, b, step, swing, positions);
			return positions;
		}

		GridPosition nearestOf(const std::vector<GridPosition>& positions, time::Ticks tick)
		{
			const GridPosition* best = &positions.front();
			for (const GridPosition& position : positions)
				if (std::llabs(position.position - tick) <= std::llabs(best->position - tick))
					best = &position;
			return *best;
		}

		NoteTiming timingOf(const Note& note)
		{
			return {note.position, note.length, note.velocity};
		}

		// The timing quantizing starts from: the original while the note still has its quantized timing, otherwise
		// the current one, since the note was changed by other means.
		NoteTiming originalOf(const Note& note)
		{
			if (note.origin && note.origin->quantized == timingOf(note))
				return note.origin->original;
			return timingOf(note);
		}

		// Gives the note its new timing and remembers the original, unless they are the same.
		bool assign(Note& note, const NoteTiming& original, const NoteTiming& quantized)
		{
			const Note before = note;
			note.position = quantized.position;
			note.length = quantized.length;
			note.velocity = quantized.velocity;
			if (quantized == original)
				note.origin.reset();
			else
				note.origin = QuantizeOrigin{original, quantized};
			return !(note == before);
		}

		// Where quantizing moves a position, in project ticks, or nothing if it stays: outside the catch range or
		// inside the safe range.
		std::optional<GridPosition> target(const QuantizeGrid& grid, time::Ticks tick, ObjectId note, Edge edge)
		{
			const QuantizeSettings& settings = grid.settings();
			const time::Ticks from = grid.preQuantized(tick);
			GridPosition position = grid.nearest(from);
			time::Ticks distance = position.position - from;
			if (settings.catchRange > 0 && std::llabs(distance) > settings.catchRange)
				return std::nullopt;
			if (settings.safeRange > 0 && std::llabs(distance) <= settings.safeRange)
				return std::nullopt;
			if (settings.groove && settings.maxMove > 0)
				distance = std::clamp(distance, -settings.maxMove, settings.maxMove);
			position.position = from + std::llround(settings.strength * static_cast<double>(distance)) +
				randomOffset(settings, note, edge);
			return position;
		}

		template <typename T> T toward(T from, T to, double strength)
		{
			return static_cast<T>(std::llround(
				static_cast<double>(from) + (strength * (static_cast<double>(to) - static_cast<double>(from)))));
		}
	}

	QuantizeGrid::QuantizeGrid(QuantizeSettings settings, time::SignatureMap signatures)
		: m_settings(std::move(settings)), m_signatures(std::move(signatures))
	{
	}

	double QuantizeGrid::step() const
	{
		if (m_settings.groove)
			return static_cast<double>(m_settings.groove->length);
		auto step = static_cast<double>(m_settings.grid.ticks());
		if (m_settings.tuplet != 0)
			step = step * tupletSpan(m_settings.tuplet) / m_settings.tuplet;
		return step;
	}

	std::vector<GridPosition> QuantizeGrid::around(time::Ticks tick) const
	{
		if (!m_settings.groove)
		{
			const bool swings = m_settings.grid.kind == NoteValueKind::Straight && m_settings.tuplet == 0;
			const double swing = swings ? m_settings.swing * step() / 3.0 : 0.0;
			return gridAround(m_signatures, tick, step(), swing);
		}
		// Repetitions of the groove from the one before tick's to two after, so the next position after any of
		// them is there too.
		const Groove& groove = *m_settings.groove;
		const int64_t repetition = floorDiv(tick - m_settings.origin, groove.length);
		std::vector<GridPosition> positions;
		for (int64_t r = repetition - 1; r <= repetition + 2; ++r)
			for (const GroovePoint& point : groove.points)
				positions.push_back(
					{m_settings.origin + (r * groove.length) + point.position, point.velocity, point.length});
		return positions;
	}

	GridPosition QuantizeGrid::nearest(time::Ticks tick) const
	{
		const std::vector<GridPosition> positions = around(tick);
		if (positions.empty())
			return {tick, std::nullopt, std::nullopt};
		return nearestOf(positions, tick);
	}

	time::Ticks QuantizeGrid::next(time::Ticks tick) const
	{
		for (const GridPosition& position : around(tick))
			if (position.position > tick)
				return position.position;
		return tick + std::max<time::Ticks>(std::llround(step()), 1); // not reached: a later bar is always there
	}

	time::Ticks QuantizeGrid::preQuantized(time::Ticks tick) const
	{
		if (!m_settings.groove || !m_settings.preQuantize)
			return tick;
		return nearestOf(
			gridAround(m_signatures, tick, static_cast<double>(m_settings.preQuantize->ticks()), 0.0), tick)
			.position;
	}

	bool quantizeStart(Note& note, time::Ticks partStart, const QuantizeGrid& grid)
	{
		const QuantizeSettings& settings = grid.settings();
		const NoteTiming original = originalOf(note);
		const NoteTiming from = settings.soft ? timingOf(note) : original;
		NoteTiming result = timingOf(note);
		result.position = from.position;
		const std::optional<GridPosition> moved = target(grid, partStart + from.position, note.id, Edge::Start);
		if (moved && moved->position >= partStart)
		{
			result.position = moved->position - partStart;
			if (moved->velocity && settings.velocityStrength > 0.0)
				result.velocity = toward(from.velocity, *moved->velocity, settings.velocityStrength);
			if (moved->length && settings.lengthStrength > 0.0)
				result.length = std::max<time::Ticks>(toward(from.length, *moved->length, settings.lengthStrength), 1);
		}
		return assign(note, original, result);
	}

	bool quantizeEnd(Note& note, time::Ticks partStart, const QuantizeGrid& grid)
	{
		const NoteTiming original = originalOf(note);
		const NoteTiming current = timingOf(note);
		const NoteTiming& from = grid.settings().soft ? current : original;
		time::Ticks end = from.position + from.length;
		if (const std::optional<GridPosition> moved = target(grid, partStart + end, note.id, Edge::End))
			end = moved->position - partStart;
		if (end <= current.position)
			end = grid.next(partStart + current.position) - partStart;
		NoteTiming result = current;
		result.length = end - current.position;
		return assign(note, original, result);
	}

	bool quantizeLength(Note& note, time::Ticks partStart, const QuantizeGrid& grid, std::optional<time::Ticks> length)
	{
		const NoteTiming original = originalOf(note);
		NoteTiming result = timingOf(note);
		if (length)
		{
			result.length = *length;
		}
		else
		{
			const time::Ticks start = partStart + result.position;
			const GridPosition position = grid.nearest(start);
			const time::Ticks catchRange = grid.settings().catchRange;
			if (catchRange > 0 && std::llabs(position.position - start) > catchRange)
				return false;
			result.length = grid.next(position.position) - position.position;
		}
		return assign(note, original, result);
	}

	bool resetQuantize(Note& note)
	{
		if (!note.origin || !(note.origin->quantized == timingOf(note)))
			return false;
		const NoteTiming original = note.origin->original;
		note.position = original.position;
		note.length = original.length;
		note.velocity = original.velocity;
		note.origin.reset();
		return true;
	}

	bool freezeQuantize(Note& note)
	{
		if (!note.origin)
			return false;
		note.origin.reset();
		return true;
	}

	Groove extractGroove(const MidiPart& part, time::Ticks length)
	{
		std::map<time::Ticks, GroovePoint> points;
		for (const auto& [id, note] : part.notes)
		{
			if (note.position < 0 || note.position >= length)
				continue;
			GroovePoint& point = points[note.position];
			point.position = note.position;
			point.velocity = std::max(point.velocity.value_or(0), note.velocity);
			point.length = std::max(point.length.value_or(0), note.length);
		}
		Groove groove;
		groove.length = length;
		for (const auto& [position, point] : points)
			groove.points.push_back(point);
		return groove;
	}
}
