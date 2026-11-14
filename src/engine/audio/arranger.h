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

		bool operator==(const ArrangerSection& other) const
		{
			return start == other.start && end == other.end && jumps == other.jumps && event == other.event;
		}
	};

	struct ArrangerPlanStep
	{
		int32_t section = 0; // an index into ArrangerPlan::sections
		int32_t repeats = 1; // at least 1
		ArrangerRepeat mode = ArrangerRepeat::Normal;

		bool operator==(const ArrangerPlanStep& other) const
		{
			return section == other.section && repeats == other.repeats && mode == other.mode;
		}
	};

	// What arranger mode plays: the active chain's steps over the arranger events.
	struct ArrangerPlan
	{
		bool active = false; // arranger mode is on
		ArrangerJump jump = ArrangerJump::None;
		std::vector<ArrangerSection> sections; // by event, in the order of the project's arranger events
		std::vector<ArrangerPlanStep> steps;
		// Counts the plan changes of a project, assigned when a snapshot publishes a new plan (rendersnapshot.cpp);
		// the transport publishes it with its state, so a cue walks the plan the state's indices mean (#885). Not
		// part of the content equality: a plan that plays the same is the same plan and keeps its generation.
		uint64_t generation = 0;

		bool plays() const { return active && !steps.empty(); }
		bool operator==(const ArrangerPlan& other) const
		{
			return active == other.active && jump == other.jump && sections == other.sections && steps == other.steps;
		}
	};

	// Where arranger playback is: a chain step and its repeat, or an event that loops since it was triggered, and
	// the step or event that takes over at the next jump point. -1 for none: playback outside the chain is linear.
	// The event and rank fields say what the indices mean: the event a step or section plays and, where a chain plays
	// one event in several steps, which of them, counted from 0. A new plan finds them again by what they play, so a
	// chain edit can't hand a step to another one (#885). They travel with the state but not in its published
	// packing, and the equality that announces changes ignores them: they describe the indices, not the playback.
	struct ArrangerState
	{
		int32_t step = -1;
		int32_t repeat = 0;
		int32_t section = -1; // looping, when step is -1
		int32_t pendingStep = -1;
		int32_t pendingRepeat = 0;
		int32_t pendingSection = -1;
		bool entering = false;  // playback started or was located: arrangerEnter finds where it is first
		uint64_t stepEvent = 0; // the event the step plays, and its rank among that event's steps
		int32_t stepRank = 0;
		uint64_t sectionEvent = 0; // the event the looping section is
		uint64_t pendingEvent = 0; // the same for the step or event that takes over
		int32_t pendingRank = 0;
		uint64_t pendingSectionEvent = 0;

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
	// position, else in the first step whose event holds it, else outside the chain. A step the search finds starts
	// at its first repeat: a repeat belongs to the step that was playing, not to the event's other steps. A state
	// whose indices come from another plan goes through arrangerRemapped first. Real-time safe.
	ArrangerState arrangerEnter(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept;

	// The next turn after position (at at or after it) for playback in state. Real-time safe.
	ArrangerTurn arrangerNext(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept;

	// The state's event and rank fields filled in from plan, so a later plan finds its indices again (#885).
	// Real-time safe.
	ArrangerState arrangerIdentity(const ArrangerPlan& plan, const ArrangerState& state) noexcept;

	// The state's indices found again in plan by the events and ranks it carries (#885): a step that is still there
	// keeps its repeat, clamped to the ones it has now, and one whose event left the chain goes back to -1, where the
	// position decides; a step or event that waits and is gone is dropped, instead of taking over as whatever its old
	// index names. Indices without events pass through as they are. Real-time safe.
	ArrangerState arrangerRemapped(const ArrangerPlan& plan, const ArrangerState& state) noexcept;

	// The plan of a published generation out of the two a cue keeps, or null when neither is it: a cue skips the
	// pass then, rather than walk a plan the state's indices don't mean (#885).
	const ArrangerPlan* planOfGeneration(
		const ArrangerPlan* newest, const ArrangerPlan* previous, uint64_t generation) noexcept;

	// The plan generations that travel in the packed state: the fields leave 27 bits for it in the two sequence-lock
	// words (#885). Plans whose generations differ by a multiple of this share a key; reaching that would take more
	// plan changes inside one audio block than a machine can build, and a cue whose published generation matches
	// neither plan it keeps skips the pass instead of walking a wrong plan.
	inline constexpr uint64_t g_arrangerGenerations = uint64_t{1} << 27;

	// The state and the generation of the plan its step and section indices walk, packed into two integers for a
	// lock-free hand-over, and back. The generation comes back modulo g_arrangerGenerations.
	std::pair<int64_t, int64_t> pack(const ArrangerState& state, uint64_t generation) noexcept;
	ArrangerState unpackArranger(int64_t a, int64_t b) noexcept;
	uint64_t unpackGeneration(int64_t a, int64_t b) noexcept;
}
