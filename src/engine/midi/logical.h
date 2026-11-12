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

	namespace detail
	{
		// Reads filter lines as tokens: each line's open brackets, the line, its close brackets and its join.
		template <typename Line, typename Test> class LineExpression
		{
		public:
			LineExpression(const Line* lines, size_t count, const Test& test) noexcept
				: m_lines(lines), m_count(count), m_test(test), m_open(count > 0 ? lines[0].open : 0)
			{
			}

			// expression := term (or term)*
			bool expression() noexcept
			{
				bool result = term();
				while (m_line < m_count && m_phase == Phase::Join && m_lines[m_line].join == LogicalBool::Or)
				{
					next();
					result = term() || result; // every line is read, also when the result is known
				}
				return result;
			}

		private:
			enum class Phase : uint8_t
			{
				Open,  // m_open brackets before the line
				Close, // m_close brackets after it
				Join,  // its join to the next line
			};

			// term := factor (and factor)*
			bool term() noexcept
			{
				bool result = factor();
				while (m_line < m_count && m_phase == Phase::Join && m_lines[m_line].join == LogicalBool::And)
				{
					next();
					result = factor() && result;
				}
				return result;
			}

			// factor := ( expression ) | line
			bool factor() noexcept
			{
				if (m_line >= m_count)
					return false;
				if (m_phase == Phase::Open && m_open > 0)
				{
					--m_open;
					const bool result = expression();
					if (m_phase == Phase::Close && m_close > 0)
						--m_close;
					afterClose();
					return result;
				}
				if (m_phase != Phase::Open)
					return false; // not reached for matching brackets
				const bool result = m_test(m_lines[m_line]);
				m_phase = Phase::Close;
				m_close = m_lines[m_line].close;
				afterClose();
				return result;
			}

			void afterClose() noexcept
			{
				if (m_phase == Phase::Close && m_close == 0)
				{
					if (m_line + 1 < m_count)
						m_phase = Phase::Join;
					else
						m_line = m_count;
				}
			}

			void next() noexcept
			{
				++m_line;
				m_phase = Phase::Open;
				m_open = m_lines[m_line].open;
			}

			const Line* m_lines;
			size_t m_count;
			const Test& m_test;
			size_t m_line = 0;
			Phase m_phase = Phase::Open;
			int m_open;
			int m_close = 0;
		};
	}

	// Whether filter lines, with their open and close brackets and their joins (and binding more tightly than or),
	// hold, test saying whether each line does; no lines hold. The rule editor's lines and the project rules' lines
	// (document/projectrules.h) are read alike. Real-time safe if test is.
	template <typename Line, typename Test>
	bool evaluateLines(const Line* lines, size_t count, const Test& test) noexcept
	{
		if (count == 0)
			return true;
		return detail::LineExpression<Line, Test>(lines, count, test).expression();
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

	// A seed for a command that isn't given one: each call returns another, so that each Apply of random operations
	// gives new values.
	uint64_t nextLogicalSeed() noexcept;

	// Applies the actions in order, then keeps the values in their ranges: pitches beyond 0 to 127 delete the note,
	// other values stay from 0 to 127 (the callers keep velocities of notes from 1, and play a velocity of 0 that a
	// controller turned into a note became as a note-off), channels from 1 to 16, and positions the actions changed
	// from 0 and lengths they changed from 1 tick; positions and lengths stay finite. Returns false if the event is
	// deleted. Real-time safe.
	bool transform(const ActionLine* actions, size_t count, LogicalEvent& event, LogicalRandom& random) noexcept;

	// The subtype and main value of an event that isn't a note (type), as the rules see them, from its number and its
	// value at GNUbase's resolution (controllers and pressure 32 bits, pitch bend 32 bits with g_pitchBendCenter in
	// the middle, a program change's bank). Real-time safe.
	void setLogicalValues(LogicalType type, uint8_t number, uint32_t value, LogicalEvent& event) noexcept;
	// Back: the number and value of what the rules made (changed, not a note) of original. Values the rules didn't
	// change keep their full resolution; a program change made of another event has no bank (0xFFFFFFFF). Real-time
	// safe.
	void logicalValues(
		const LogicalEvent& changed, const LogicalEvent& original, uint8_t& number, uint32_t& value) noexcept;

	const char* toString(LogicalType type);
	const char* toString(LogicalFunction function);

	Json toJson(const LogicalRules& rules);
	// Brackets nest at most this deep (reference manual p. 1418), so that matches' recursion stays small.
	inline constexpr int g_maxLogicalBrackets = 3;

	// Throws std::invalid_argument for rules that don't fit logicalRulesSchema or have brackets that don't match or
	// nest deeper than g_maxLogicalBrackets.
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
	// std::invalid_argument for what doesn't fit, or rules input transformers don't have: the select
	// function, or position or length in a filter or an action.
	InputTransformerModules inputTransformerModulesFromJson(const Json& json, InputTransformerModules base);

	Json toJson(const InputTransformer& transformer);
	// base with what json changes: {mode: off, track or project, modules}.
	InputTransformer inputTransformerFromJson(const Json& json, InputTransformer base);
}
