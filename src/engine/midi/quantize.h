// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/signaturemap.h"

#include <cstdint>
#include <optional>
#include <vector>

// MIDI quantizing (#132): moves note starts or ends to a grid or a groove, or sets note lengths. Quantizing keeps
// each note's original timing (Note::origin), so it always starts from the original unless it is soft (iterative),
// and resetting puts the original back. These functions run on the command thread; quantizecommands.h has the
// commands.
namespace gnubase::engine::midi
{
	// A position that quantizing moves to, in project ticks, with the groove's velocity and length there.
	struct GridPosition
	{
		time::Ticks position = 0;
		std::optional<uint16_t> velocity;
		std::optional<time::Ticks> length;
	};

	// The positions of a grid or groove. A grid starts again at every bar, so it follows time signature changes and
	// swing counts from the bar's start; a groove repeats every groove length from the settings' origin.
	class QuantizeGrid
	{
	public:
		QuantizeGrid(QuantizeSettings settings, time::SignatureMap signatures);

		// The grid position nearest to tick; at equal distance the later one.
		GridPosition nearest(time::Ticks tick) const;

		// The first grid position after tick.
		time::Ticks next(time::Ticks tick) const;

		// With a groove and pre-quantizing, the position on the pre-quantize grid nearest to tick; otherwise tick.
		time::Ticks preQuantized(time::Ticks tick) const;

		// The grid step in ticks, with tuplets; for a groove its length.
		double step() const;

		const QuantizeSettings& settings() const { return m_settings; }

	private:
		// The grid positions of the bars or groove repetitions around tick, sorted.
		std::vector<GridPosition> around(time::Ticks tick) const;

		QuantizeSettings m_settings;
		time::SignatureMap m_signatures;
	};

	// Where a part lies on the timeline, in project ticks: notes start from start, and notes at or after end don't
	// play.
	struct PartSpan
	{
		time::Ticks start = 0;
		time::Ticks end = 0;
	};

	// Each function quantizes one note of part and returns whether the note changed. Note starts stay within the
	// part: a note doesn't move before its start, or to or past its end.
	//
	// Quantizing starts from the note's original timing while the note still is where quantizing put it: at its
	// quantized position, and for the length functions with its quantized length too. A note moved by other means
	// since starts from where it is. A velocity or length changed by other means stays, and counts as the original.

	// Moves the note's start toward the nearest grid position and keeps its length. With a groove, the velocity and
	// length strengths also move velocity and length toward the groove's.
	bool quantizeStart(Note& note, const PartSpan& part, const QuantizeGrid& grid);

	// Moves the note's end toward the nearest grid position; the start stays. An end that would come at or before
	// the start goes to the first grid position after the start.
	bool quantizeEnd(Note& note, const PartSpan& part, const QuantizeGrid& grid);

	// Sets the note's length to length ticks, or, without length, to the distance from the grid position nearest to
	// its start to the next one, so tuplets and swing count; then notes outside the catch range keep their length.
	bool quantizeLength(Note& note, const PartSpan& part, const QuantizeGrid& grid, std::optional<time::Ticks> length);

	// Puts back the timing the note had before it was quantized. A note moved by other means since stays; a velocity
	// or length changed by other means since stays too.
	bool resetQuantize(Note& note);

	// Makes the note's timing its original one: quantizing starts from here and resetting no longer goes back.
	bool freezeQuantize(Note& note);

	// The rhythm of a part's notes as a groove of length ticks: a point at every note start before length, with the
	// highest velocity and the longest length of the notes that start there. The points are relative to the part's
	// start, so a groove quantizes in phase with the part when its settings' origin is the part's start.
	Groove extractGroove(const MidiPart& part, time::Ticks length);
}
