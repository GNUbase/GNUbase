// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

// MIDI modifiers (#134, reference manual pp. 1099-1102): settings of a MIDI or instrument track that change how its
// notes play, not the notes themselves. The render snapshot applies them to the notes of the track's parts, the
// track's sequencer to the notes played into it live.
namespace gnubase::engine::midi
{
	enum class RandomTarget : uint8_t
	{
		Off,
		Position, // ticks
		Pitch,    // semitones
		Velocity, // MIDI 1.0 velocity
		Length,   // ticks
	};

	enum class RangeTarget : uint8_t
	{
		Off,
		VelocityLimit,  // velocities below the minimum play at the minimum, those above the maximum at the maximum
		VelocityFilter, // notes with velocities outside the range don't play
		NoteLimit,      // pitches outside the range move into it by octaves
		NoteFilter,     // notes with pitches outside the range don't play
	};

	// Adds a value from minimum to maximum to its target, chosen anew for every note.
	struct RandomModifier
	{
		RandomTarget target = RandomTarget::Off;
		int64_t minimum = 0;
		int64_t maximum = 0;
	};

	// Velocities (MIDI 1.0, 0 to 127) or pitches (0 to 127), the minimum not above the maximum.
	struct RangeModifier
	{
		RangeTarget target = RangeTarget::Off;
		int minimum = 0;
		int maximum = 127;
	};

	struct Modifiers
	{
		bool bypass = false;   // plays the notes as they are
		int transpose = 0;     // semitones, -127 to 127
		int velocityShift = 0; // added to MIDI 1.0 velocities, -127 to 127
		// Multipliers as numerator and denominator, each 1 to 16: of velocities, before the shift, and of lengths.
		std::array<int, 2> velocityCompression = {1, 1};
		std::array<int, 2> lengthCompression = {1, 1};
		std::array<RandomModifier, 2> random = {};
		std::array<RangeModifier, 2> range = {};
	};

	inline bool operator==(const RandomModifier& a, const RandomModifier& b)
	{
		return std::tie(a.target, a.minimum, a.maximum) == std::tie(b.target, b.minimum, b.maximum);
	}

	inline bool operator==(const RangeModifier& a, const RangeModifier& b)
	{
		return std::tie(a.target, a.minimum, a.maximum) == std::tie(b.target, b.minimum, b.maximum);
	}

	inline bool operator==(const Modifiers& a, const Modifiers& b)
	{
		return std::tie(a.bypass, a.transpose, a.velocityShift, a.velocityCompression, a.lengthCompression, a.random,
				   a.range) ==
			std::tie(
				b.bypass, b.transpose, b.velocityShift, b.velocityCompression, b.lengthCompression, b.random, b.range);
	}

	// Random positions and lengths go up to a quarter note either way, in ticks.
	inline constexpr int64_t g_maxRandomTicks = 1720320;

	const char* toString(RandomTarget target);
	const char* toString(RangeTarget target);
	std::optional<RandomTarget> randomTargetFromString(const std::string& name);
	std::optional<RangeTarget> rangeTargetFromString(const std::string& name);

	// What the random generators aimed at target add, for the note that seed stands for: each a value from its
	// minimum to its maximum. The same seed gives the same values. 0 when bypassed.
	int64_t randomOffset(const Modifiers& modifiers, RandomTarget target, uint64_t seed) noexcept;

	// A seed for a note of a part, so that each note gets its own random values, the same every time it plays.
	uint64_t noteSeed(uint64_t part, uint64_t note) noexcept;

	// The pitch and velocity (16 bits) a note plays with: transposed, its velocity compressed, shifted and randomized,
	// then through the ranges in order. False if it doesn't play: filtered out by a range or transposed beyond
	// 0 to 127. The velocity stays as it is while no velocity modifier is on; otherwise they work in MIDI 1.0
	// velocities, and a note-on's velocity stays at least 1.
	bool modifyNote(const Modifiers& modifiers, uint64_t seed, uint8_t& pitch, uint16_t& velocity) noexcept;

	// A note's length in ticks after length compression and random lengths, at least 1 tick.
	int64_t modifyLength(const Modifiers& modifiers, uint64_t seed, int64_t length) noexcept;
}
