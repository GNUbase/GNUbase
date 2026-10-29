// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "engine/time/tempomap.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Quantize settings (#132): the grid or groove that quantizing moves notes to, and how it moves them. A project keeps
// its current settings; presets are named settings. quantize.h has the functions that quantize.
namespace gnubase::engine::midi
{
	enum class NoteValueKind : uint8_t
	{
		Straight,
		Triplet, // two thirds of the straight value
		Dotted,  // one and a half times the straight value
	};

	// A note value such as 1/16, a 1/8 triplet or a dotted 1/4; as text "1/16", "1/8T" and "1/4.".
	struct NoteValue
	{
		int denominator = 16; // 1, 2, 4 and so on up to 128
		NoteValueKind kind = NoteValueKind::Straight;

		// Exact for every note value: g_ticksPerQuarter divides into all of them.
		time::Ticks ticks() const;
	};

	inline bool operator==(const NoteValue& a, const NoteValue& b)
	{
		return a.denominator == b.denominator && a.kind == b.kind;
	}

	std::string toString(const NoteValue& value);
	std::optional<NoteValue> noteValueFromString(const std::string& text);

	// A position of a groove, with the velocity and length notes there had.
	struct GroovePoint
	{
		time::Ticks position = 0; // from the groove's start
		std::optional<uint16_t> velocity;
		std::optional<time::Ticks> length;
	};

	// A timing pattern that repeats every length ticks, such as one bar of a played drum part.
	struct Groove
	{
		time::Ticks length = 0;
		std::vector<GroovePoint> points; // sorted by position, each from 0 to below length
	};

	// Distances are in ticks. Settings that don't apply to the kind of grid are kept but have no effect: swing only
	// works on a straight grid without tuplets, and preQuantize, maxMove and the velocity and length strengths only
	// with a groove.
	struct QuantizeSettings
	{
		NoteValue grid;
		// 0 is off; n divides the time of p grid steps into n, p being the largest power of two below n: 3 makes
		// triplets, 5 quintuplets.
		int tuplet = 0;
		// 0 to 1: moves every second grid position later, at 1 by a third of a grid step, which is a triplet feel.
		double swing = 0.0;
		time::Ticks catchRange = 0; // only notes this close to a grid position move; 0 catches all
		time::Ticks safeRange = 0;  // notes this close to a grid position stay where they are
		time::Ticks randomize = 0;  // moves quantized notes by up to this much, earlier or later
		uint64_t seed = 0;          // of the randomization: the same seed moves the same notes the same way
		// Iterative quantizing starts from where the notes are now; otherwise from where they were before they were
		// first quantized, so trying other settings never builds on earlier ones.
		bool soft = false;
		double strength = 1.0; // 0 to 1: the part of the way to the grid that notes move

		std::optional<Groove> groove;         // quantizes to this groove instead of the grid
		std::optional<NoteValue> preQuantize; // grooves: quantizes to this straight grid first
		time::Ticks maxMove = 0;              // grooves: notes move at most this far; 0 is no limit
		double velocityStrength = 0.0;        // grooves: how far velocities move toward the groove's
		double lengthStrength = 0.0;          // grooves: how far lengths move toward the groove's
		time::Ticks origin = 0;               // grooves: where the groove starts repeating, in project ticks
	};

	Json toJson(const QuantizeSettings& settings);

	// base with the settings json has changed. Throws std::invalid_argument for a value out of range.
	QuantizeSettings quantizeSettingsFromJson(const Json& json, QuantizeSettings base = {});

	// The JSON schema of quantizeSettingsFromJson's json; every property is optional.
	Json quantizeSettingsSchema();

	Json toJson(const Groove& groove);

	struct QuantizePreset
	{
		std::string name;
		QuantizeSettings settings;
	};

	// The presets that come with GNUbase: straight grids from 1/1 to 1/128, "1/16" for example, and triplet and
	// dotted grids from 1/2 to 1/64, such as "1/8 Triplet" and "1/4 Dotted".
	const std::vector<QuantizePreset>& factoryQuantizePresets();
}
