// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/time/tempomap.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

// Arranger playback (#167, reference manual pp. 404-413): the path the transport plays in arranger mode, through the
// steps of the active arranger chain. The command thread compiles the plan; the transport (audio thread) and the cue
// thread walk it with the same functions, so voices are cued where playback jumps.
namespace gnubase::engine
{
	// How a chain step repeats (reference manual p. 407).
	enum class ArrangerRepeat : uint8_t
	{
		Normal,  // plays its repeats, then the next step
		Forever, // loops until another step or event is chosen
		Pause,   // plays its repeats, then playback stops at the next step's start
	};

	inline constexpr std::array<const char*, 3> g_arrangerRepeatModes = {
		"normal", "repeatForever", "pauseAfterRepeats"};

	// When a chosen step or event takes over from the one playing (Jump Mode, reference manual pp. 411-412).
	enum class ArrangerJump : uint8_t
	{
		None,     // at once
		FourBars, // every 4 bars from the event's start, or at its end
		TwoBars,
		OneBar,  // at the next bar line
		OneBeat, // at the next beat
		End,     // at the event's end
	};

	inline constexpr std::array<const char*, 6> g_arrangerJumpModes = {
		"none", "4bars", "2bars", "1bar", "1beat", "end"};

	// An arranger event in project samples, with the points where a jump that waits may happen: ascending, after the
	// start, the last one the end (none with ArrangerJump::None).
	struct ArrangerSection
	{
		time::Samples start = 0;
		time::Samples end = 0;
		std::vector<time::Samples> jumps;
		uint64_t event = 0; // the arranger event's id
	};

	struct ArrangerPlanStep
	{
		int32_t section = 0; // an index into ArrangerPlan::sections
		int32_t repeats = 1; // at least 1
		ArrangerRepeat mode = ArrangerRepeat::Normal;
	};

	// What arranger mode plays: the active chain's steps over the arranger events.
	struct ArrangerPlan
	{
		bool active = false; // arranger mode is on
		ArrangerJump jump = ArrangerJump::None;
		std::vector<ArrangerSection> sections; // by event, in the order of the project's arranger events
		std::vector<ArrangerPlanStep> steps;

		bool plays() const { return active && !steps.empty(); }
	};

	// Where arranger playback is: a chain step and its repeat, or an event that loops since it was triggered, and
	// the step or event that takes over at the next jump point. -1 for none: playback outside the chain is linear.
	struct ArrangerState
	{
		int32_t step = -1;
		int32_t repeat = 0;
		int32_t section = -1; // looping, when step is -1
		int32_t pendingStep = -1;
		int32_t pendingRepeat = 0;
		int32_t pendingSection = -1;
		bool entering = false; // playback started or was located: arrangerEnter finds where it is first

		bool operator==(const ArrangerState& other) const
		{
			return step == other.step && repeat == other.repeat && section == other.section &&
				pendingStep == other.pendingStep && pendingRepeat == other.pendingRepeat &&
				pendingSection == other.pendingSection && entering == other.entering;
		}
		bool operator!=(const ArrangerState& other) const { return !(*this == other); }
	};

	// Whether two states play the same, whether or not they are entering.
	inline bool samePlace(ArrangerState a, ArrangerState b)
	{
		a.entering = false;
		b.entering = false;
		return a == b;
	}

	// The next change on the path from a position: playback that reaches at goes on at to, in state next, or stops
	// there with stops (the cursor goes to to). at is -1 if playback goes on linearly.
	struct ArrangerTurn
	{
		time::Samples at = -1;
		time::Samples to = 0;
		ArrangerState next;
		bool stops = false;
	};

	// Where playback that starts or is located at a position is in the plan: in the step it is in if that holds the
	// position, else in the first step whose event holds it, else outside the chain. Real-time safe.
	ArrangerState arrangerEnter(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept;

	// The next turn after position (at at or after it) for playback in state. Real-time safe.
	ArrangerTurn arrangerNext(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept;

	// The state packed into two integers for a lock-free hand-over, and back.
	std::pair<int64_t, int64_t> pack(const ArrangerState& state) noexcept;
	ArrangerState unpackArranger(int64_t a, int64_t b) noexcept;
}
