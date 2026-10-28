// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/time/tempomap.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gnubase::time
{
	// The MIDI display resolution: 120 display ticks per sixteenth note, 480 per quarter.
	inline constexpr int g_defaultDisplayTicksPerSixteenth = 120;

	struct TimeSignature
	{
		int numerator = 4;
		int denominator = 4; // the beat: 1, 2, 4, 8 or 16
	};

	// Signatures change at bar starts.
	struct SignatureChange
	{
		int64_t bar = 0; // zero-based
		TimeSignature signature;
	};

	inline bool operator==(const TimeSignature& a, const TimeSignature& b)
	{
		return a.numerator == b.numerator && a.denominator == b.denominator;
	}

	inline bool operator==(const SignatureChange& a, const SignatureChange& b)
	{
		return a.bar == b.bar && a.signature == b.signature;
	}

	// A zero-based musical position; formatBarsBeats shows it the way rulers do, starting at 1.1.1.0.
	struct BarsBeats
	{
		int64_t bar = 0;
		int beat = 0;      // in units of the signature's denominator
		int sixteenth = 0; // within the beat
		int tick = 0;      // display ticks within the sixteenth
	};

	std::string formatBarsBeats(const BarsBeats& position);

	// Converts between ticks and bars and beats. displayTicksPerSixteenth must divide g_ticksPerQuarter / 4.
	class SignatureMap
	{
	public:
		explicit SignatureMap(TimeSignature signature = {});
		// The first change must be at bar 0; throws std::invalid_argument for invalid changes.
		explicit SignatureMap(const std::vector<SignatureChange>& changes);

		Ticks barStart(int64_t bar) const;
		TimeSignature signatureAt(Ticks tick) const;

		// The changes the map was made from.
		std::vector<SignatureChange> changes() const;

		bool operator==(const SignatureMap& other) const { return changes() == other.changes(); }
		bool operator!=(const SignatureMap& other) const { return !(*this == other); }
		// Positions between display ticks round down to the display tick.
		BarsBeats toBarsBeats(Ticks tick, int displayTicksPerSixteenth = g_defaultDisplayTicksPerSixteenth) const;
		Ticks toTicks(
			const BarsBeats& position, int displayTicksPerSixteenth = g_defaultDisplayTicksPerSixteenth) const;

	private:
		// Bars from one change to the next. Before bar 0, the first signature holds.
		struct Section
		{
			int64_t bar = 0;
			Ticks start = 0;
			Ticks barLength = 0;
			TimeSignature signature;
		};

		std::vector<Section> m_sections;
	};
}
