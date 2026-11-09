// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Rule-based MIDI transforming (#178, reference manual pp. 1392-1423): filter lines that find events, a function that
// says what happens to them and action lines that change them. The rule editor applies rules to MIDI parts
// (logicalcommands.h), the input transformer to what a track's MIDI input plays (processors/midieffects.h), and the
// transformer MIDI effect is a rule of this kind with a fixed shape. Values are those of MIDI 1.0 (0 to 127), channels
// count from 1 to 16 and positions and lengths are in ticks of 480 to a quarter note, as the reference shows them, so
// presets mean the same everywhere (docs/features/midi-rules.md).
namespace gnubase::engine::midi
{
	enum class LogicalType : uint8_t
	{
		Note,
		PolyPressure,
		Controller,
		Program,
		Aftertouch, // channel pressure
		PitchBend,
	};

	inline constexpr int g_logicalTypes = 6;
	inline constexpr double g_logicalTicksPerQuarter = 480.0;

	// An event as the rules see it. Subtype and main value: a note's pitch and velocity, poly pressure's pitch and
	// pressure, a controller's number and value, a program change's program (main value unused), aftertouch's pressure
	// (main value unused) and pitch bend's least and most significant 7 bits. Secondary value: a note's note-off
	// velocity.
	struct LogicalEvent
	{
		LogicalType type = LogicalType::Note;
		int channel = 1; // 1 to 16
		int subtype = 0;
		int mainValue = 0;
		int secondaryValue = 0;
		double position = 0.0; // project position in ticks of 480 to a quarter note; 0 for live input
		double length = 0.0;   // notes, in those ticks
	};

	enum class LogicalTarget : uint8_t
	{
		Position,
		Length,
		Subtype,
		MainValue,
		Channel,
		Type,
		SecondaryValue,
	};

	enum class LogicalCondition : uint8_t
	{
		Equal,
		Unequal,
		Bigger,
		BiggerOrEqual,
		Less,
		LessOrEqual,
		InsideRange,   // from parameter 1 to parameter 2, both included
		OutsideRange,  // below parameter 1 or above parameter 2
		NoteIsEqualTo, // the same pitch class as parameter 1, in any octave
	};

	enum class LogicalBool : uint8_t
	{
		And,
		Or,
	};

	// A filter line: open brackets, the test, close brackets, and how it joins the next line. And binds more tightly
	// than or; brackets are evaluated first, from the innermost out.
	struct FilterLine
	{
		int open = 0; // 0 to 3
		LogicalTarget target = LogicalTarget::Type;
		LogicalCondition condition = LogicalCondition::Equal;
		double parameter1 = 0.0; // for type, a LogicalType
		double parameter2 = 0.0;
		int close = 0; // 0 to 3
		LogicalBool join = LogicalBool::And;
	};

	enum class LogicalFunction : uint8_t
	{
		Delete,          // the input transformer's Filter: found events go
		Transform,       // found events change by the actions
		Insert,          // found events stay, and changed copies are added
		InsertExclusive, // found events change, the others go
		Select,          // the rule editor only: returns the found notes and changes nothing
	};

	enum class LogicalOperation : uint8_t
	{
		Add,
		Subtract,
		MultiplyBy,
		DivideBy,
		RoundBy,           // to the nearest multiple of parameter 1
		SetRandom,         // a random whole number from parameter 1 to parameter 2
		SetFixed,          // parameter 1
		SetRelativeRandom, // adds a random whole number from parameter 1 to parameter 2
		UseSubtype,        // main value only: the subtype
		UseMainValue,      // subtype only: the main value
		Mirror,            // around parameter 1
		TransposeToScale,  // subtype only: the closest pitch of the scale with root parameter 1 (0 is C) and type
						   // parameter 2 (a midi::ScaleType)
		AddLength,         // position only: moves notes to where they end
	};

	struct ActionLine
	{
		LogicalTarget target = LogicalTarget::Subtype;
		LogicalOperation operation = LogicalOperation::Add;
		double parameter1 = 0.0;
		double parameter2 = 0.0;
	};

	struct LogicalRules
	{
		std::vector<FilterLine> filters; // none find every event
		LogicalFunction function = LogicalFunction::Transform;
		std::vector<ActionLine> actions;
	};

	bool operator==(const FilterLine& a, const FilterLine& b);
	bool operator==(const ActionLine& a, const ActionLine& b);
	bool operator==(const LogicalRules& a, const LogicalRules& b);
	inline bool operator!=(const LogicalRules& a, const LogicalRules& b)
	{
		return !(a == b);
	}

	// Whether the filter lines find the event. Real-time safe.
	bool matches(const FilterLine* lines, size_t count, const LogicalEvent& event) noexcept;

	// A random number generator for the random operations: the same seed gives the same numbers everywhere.
	struct LogicalRandom
	{
		uint64_t state = 0;
		// A whole number from low to high, both included.
		int between(int low, int high) noexcept;
	};

	// Applies the actions in order, then keeps the values in their ranges: pitches beyond 0 to 127 delete the note,
	// other values stay from 0 to 127 (the callers keep velocities of notes from 1, and play a velocity of 0 that a
	// controller turned into a note became as a note-off), channels from 1 to 16, positions from 0 and lengths from 1
	// tick. Returns false if the event is deleted. Real-time safe.
	bool transform(const ActionLine* actions, size_t count, LogicalEvent& event, LogicalRandom& random) noexcept;

	const char* toString(LogicalType type);
	const char* toString(LogicalFunction function);

	Json toJson(const LogicalRules& rules);
	// Throws std::invalid_argument for rules that don't fit logicalRulesSchema or have brackets that don't match.
	LogicalRules logicalRulesFromJson(const Json& json);
	Json logicalRulesSchema();

	struct LogicalPreset
	{
		std::string name; // "Category/Name" for a factory preset
		LogicalRules rules;
	};

	// The presets that come with GNUbase, for the rule editor and the input transformers.
	const std::vector<LogicalPreset>& factoryLogicalPresets();

	// Input transformers (reference manual pp. 1097-1099, 1392-1404): up to four modules that change what MIDI input
	// plays, each a set of rules, in order.
	struct InputTransformerModule
	{
		bool on = false;
		LogicalRules rules;
	};

	inline constexpr size_t g_inputTransformerModules = 4;
	using InputTransformerModules = std::array<InputTransformerModule, g_inputTransformerModules>;

	bool operator==(const InputTransformerModule& a, const InputTransformerModule& b);

	enum class InputTransformerMode : uint8_t
	{
		Off,
		Track,   // the track's own modules
		Project, // the project's modules, which every track in this mode shares
	};

	// A MIDI or instrument track's input transformer.
	struct InputTransformer
	{
		InputTransformerMode mode = InputTransformerMode::Off;
		InputTransformerModules modules;
	};

	bool operator==(const InputTransformer& a, const InputTransformer& b);
	inline bool operator!=(const InputTransformer& a, const InputTransformer& b)
	{
		return !(a == b);
	}

	Json toJson(const InputTransformerModules& modules);
	// base with what json changes: an array of at most four modules, each {on, rules} with either left out. Throws
	// std::invalid_argument for what doesn't fit, or rules whose function input transformers don't have (select).
	InputTransformerModules inputTransformerModulesFromJson(const Json& json, InputTransformerModules base);

	Json toJson(const InputTransformer& transformer);
	// base with what json changes: {mode: off, track or project, modules}.
	InputTransformer inputTransformerFromJson(const Json& json, InputTransformer base);
}
