// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/metronome/metronomesettings.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

// Where the metronome clicks: the clicks of every bar's pattern on a timeline of tempo and signatures. Real-time
// safe: nothing here allocates.
namespace gnubase::engine::metronome
{
	// The project's tempo and signatures.
	struct ProjectTimeline
	{
		const time::TempoMap& tempo;
		const time::SignatureMap& signatures;
		double sampleRate = 48000.0;

		int64_t barAt(time::Ticks tick) const { return signatures.toBarsBeats(tick).bar; }
		time::Ticks barStart(int64_t bar) const { return signatures.barStart(bar); }
		time::TimeSignature signatureAt(time::Ticks tick) const { return signatures.signatureAt(tick); }
		time::Samples toSamples(time::Ticks tick) const { return tempo.toSamples(tick, sampleRate); }
		time::Ticks toTicks(time::Samples sample) const { return tempo.toTicks(sample, sampleRate); }
	};

	// One tempo and signature, bars from position 0: a count-in's. It converts as a tempo map of one tempo does.
	struct ConstantTimeline
	{
		double bpm = 120.0;
		time::TimeSignature signature;
		double sampleRate = 48000.0;

		time::Ticks barLength() const
		{
			return signature.numerator * (time::g_ticksPerQuarter * 4 / signature.denominator);
		}
		int64_t barAt(time::Ticks tick) const
		{
			const time::Ticks length = barLength();
			return tick >= 0 ? tick / length : -((-tick + length - 1) / length);
		}
		time::Ticks barStart(int64_t bar) const { return bar * barLength(); }
		time::TimeSignature signatureAt(time::Ticks /*tick*/) const { return signature; }
		time::Samples toSamples(time::Ticks tick) const
		{
			const double beats = static_cast<double>(tick) / static_cast<double>(time::g_ticksPerQuarter);
			return std::llround(beats * 60.0 / bpm * sampleRate);
		}
		time::Ticks toTicks(time::Samples sample) const
		{
			const double seconds = static_cast<double>(sample) / sampleRate;
			return std::llround(seconds * bpm / 60.0 * static_cast<double>(time::g_ticksPerQuarter));
		}
	};

	// A signature's pattern, compiled: up to g_maxClicks accents, sorted by signature for lookup.
	struct PatternTable
	{
		struct Entry
		{
			time::TimeSignature signature;
			int clicks = 0;
			std::array<uint8_t, g_maxClicks> accents{};
		};
		std::vector<Entry> entries; // sorted by numerator, then denominator

		// The signature's pattern, or nullptr: it clicks its default pattern, one click per beat, the first with
		// accent 1 and the others with accent 2.
		const Entry* find(time::TimeSignature signature) const
		{
			const auto key = [](time::TimeSignature s) { return std::make_tuple(s.numerator, s.denominator); };
			const auto found = std::lower_bound(entries.begin(), entries.end(), signature,
				[&key](const Entry& entry, time::TimeSignature s) { return key(entry.signature) < key(s); });
			return found != entries.end() && found->signature == signature ? &*found : nullptr;
		}
	};

	// Calls click(position, accent) for each click from begin up to end, in order, on a timeline. A bar's clicks are
	// evenly spread over it, at whole ticks, and play at the sample their tick falls on. Clicks with no accent are
	// left out. The clicks are the same however the range is split up.
	template <typename Timeline, typename Click> void clicksIn(
		const Timeline& timeline, const PatternTable& patterns, time::Samples begin, time::Samples end, Click&& click)
	{
		if (end <= begin)
			return;
		// From the bar before: a bar's first click may round to a sample before begin's tick.
		for (int64_t bar = timeline.barAt(timeline.toTicks(begin)) - 1;; ++bar)
		{
			const time::Ticks start = timeline.barStart(bar);
			if (timeline.toSamples(start) >= end)
				return;
			const time::Ticks length = timeline.barStart(bar + 1) - start;
			const time::TimeSignature signature = timeline.signatureAt(start);
			const PatternTable::Entry* pattern = patterns.find(signature);
			const int clicks = pattern != nullptr ? pattern->clicks : signature.numerator;
			for (int k = 0; k < clicks; ++k)
			{
				const time::Samples at = timeline.toSamples(start + (length * k / clicks));
				if (at >= end)
					return;
				int accent = k == 0 ? 1 : 2; // the default pattern
				if (pattern != nullptr)
					accent = pattern->accents[static_cast<size_t>(k)];
				if (at >= begin && accent != 0)
					click(at, accent);
			}
		}
	}
}
