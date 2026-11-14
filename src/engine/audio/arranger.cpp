// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/arranger.h"

#include <algorithm>

namespace gnubase::engine
{
	namespace
	{
		bool validSection(const ArrangerPlan& plan, int32_t section)
		{
			return section >= 0 && static_cast<size_t>(section) < plan.sections.size();
		}

		bool validStep(const ArrangerPlan& plan, int32_t step)
		{
			return step >= 0 && static_cast<size_t>(step) < plan.steps.size() &&
				validSection(plan, plan.steps[static_cast<size_t>(step)].section);
		}

		const ArrangerSection& sectionOf(const ArrangerPlan& plan, int32_t section)
		{
			return plan.sections[static_cast<size_t>(section)];
		}

		const ArrangerPlanStep& stepOf(const ArrangerPlan& plan, int32_t step)
		{
			return plan.steps[static_cast<size_t>(step)];
		}

		bool holds(const ArrangerPlan& plan, int32_t section, time::Samples position)
		{
			return validSection(plan, section) && sectionOf(plan, section).start <= position &&
				position < sectionOf(plan, section).end;
		}

		uint64_t stepEvent(const ArrangerPlan& plan, int32_t step)
		{
			return validStep(plan, step) ? sectionOf(plan, stepOf(plan, step).section).event : uint64_t{0};
		}

		// How many of the steps before step play the same event: its rank among that event's steps.
		int32_t rankOf(const ArrangerPlan& plan, int32_t step)
		{
			const uint64_t event = stepEvent(plan, step);
			int32_t rank = 0;
			for (int32_t s = 0; s < step; ++s)
				if (stepEvent(plan, s) == event)
					++rank;
			return rank;
		}

		// The step of plan that is the rank-th one playing event, or -1 if it has fewer of them.
		int32_t nthStepOf(const ArrangerPlan& plan, uint64_t event, int32_t rank)
		{
			for (size_t s = 0; s < plan.steps.size(); ++s)
			{
				if (stepEvent(plan, static_cast<int32_t>(s)) != event)
					continue;
				if (rank == 0)
					return static_cast<int32_t>(s);
				--rank;
			}
			return -1;
		}

		int32_t sectionOfEvent(const ArrangerPlan& plan, uint64_t event)
		{
			for (size_t s = 0; s < plan.sections.size(); ++s)
				if (plan.sections[s].event == event)
					return static_cast<int32_t>(s);
			return -1;
		}
	}

	ArrangerState arrangerIdentity(const ArrangerPlan& plan, const ArrangerState& state) noexcept
	{
		ArrangerState result = state;
		result.stepEvent = validStep(plan, state.step) ? stepEvent(plan, state.step) : 0;
		result.stepRank = validStep(plan, state.step) ? rankOf(plan, state.step) : 0;
		result.sectionEvent =
			state.step < 0 && validSection(plan, state.section) ? sectionOf(plan, state.section).event : 0;
		result.pendingEvent = validStep(plan, state.pendingStep) ? stepEvent(plan, state.pendingStep) : 0;
		result.pendingRank = validStep(plan, state.pendingStep) ? rankOf(plan, state.pendingStep) : 0;
		result.pendingSectionEvent = state.pendingStep < 0 && validSection(plan, state.pendingSection)
			? sectionOf(plan, state.pendingSection).event
			: 0;
		return result;
	}

	ArrangerState arrangerRemapped(const ArrangerPlan& plan, const ArrangerState& state) noexcept
	{
		// The repeat a step found again keeps: its own, as far as the step found allows. A Forever step's counter
		// runs past its repeats and is kept as it is.
		const auto keptRepeat = [&plan, &state](int32_t step, int32_t repeat)
		{
			if (step < 0)
				return int32_t{0};
			const ArrangerPlanStep& found = stepOf(plan, step);
			return found.mode == ArrangerRepeat::Forever ? repeat : std::min(repeat, found.repeats - 1);
		};
		ArrangerState result = state;
		if (state.step >= 0 && state.stepEvent != 0)
		{
			const int32_t step = nthStepOf(plan, state.stepEvent, state.stepRank);
			result.step = step;
			result.section = -1;
			result.sectionEvent = 0;
			result.repeat = keptRepeat(step, state.repeat);
		}
		else if (state.step < 0 && state.sectionEvent != 0)
			result.section = sectionOfEvent(plan, state.sectionEvent);
		if (state.pendingStep >= 0 && state.pendingEvent != 0)
		{
			const int32_t step = nthStepOf(plan, state.pendingEvent, state.pendingRank);
			result.pendingStep = step;
			result.pendingRepeat = keptRepeat(step, state.pendingRepeat);
			result.pendingSection = -1;
			result.pendingSectionEvent = 0;
		}
		else if (state.pendingStep < 0 && state.pendingSectionEvent != 0)
			result.pendingSection = sectionOfEvent(plan, state.pendingSectionEvent);
		return result;
	}

	const ArrangerPlan* planOfGeneration(
		const ArrangerPlan* newest, const ArrangerPlan* previous, uint64_t generation) noexcept
	{
		if (newest != nullptr && newest->generation % g_arrangerGenerations == generation)
			return newest;
		if (previous != nullptr && previous->generation % g_arrangerGenerations == generation)
			return previous;
		return nullptr;
	}

	ArrangerState arrangerEnter(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept
	{
		ArrangerState result; // a step or event that waits still does, with the events its indices mean
		result.pendingStep = state.pendingStep;
		result.pendingRepeat = state.pendingRepeat;
		result.pendingSection = state.pendingSection;
		result.pendingEvent = state.pendingEvent;
		result.pendingRank = state.pendingRank;
		result.pendingSectionEvent = state.pendingSectionEvent;
		if (!plan.plays())
			return result;
		if (validStep(plan, state.step) && holds(plan, stepOf(plan, state.step).section, position))
		{
			result.step = state.step;
			result.repeat = state.repeat;
			result.stepEvent = stepEvent(plan, state.step);
			result.stepRank = rankOf(plan, state.step);
			return result;
		}
		if (state.step < 0 && holds(plan, state.section, position))
		{
			result.section = state.section;
			result.sectionEvent = sectionOf(plan, state.section).event;
			return result;
		}
		for (size_t s = 0; s < plan.steps.size(); ++s)
			if (holds(plan, plan.steps[s].section, position))
			{
				result.step = static_cast<int32_t>(s);
				result.stepEvent = stepEvent(plan, result.step);
				result.stepRank = rankOf(plan, result.step);
				return result;
			}
		return result;
	}

	ArrangerTurn arrangerNext(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept
	{
		ArrangerTurn turn;
		if (!plan.plays())
			return turn;
		int32_t current = -1; // the section playing
		if (validStep(plan, state.step))
			current = stepOf(plan, state.step).section;
		else if (state.step < 0 && validSection(plan, state.section))
			current = state.section;
		// A step or event that waits takes over at the next jump point of the event playing, or at once outside one.
		const bool pendingStep = validStep(plan, state.pendingStep);
		if (pendingStep || validSection(plan, state.pendingSection))
		{
			turn.next.step = pendingStep ? state.pendingStep : -1;
			turn.next.repeat = pendingStep ? state.pendingRepeat : 0;
			turn.next.section = pendingStep ? -1 : state.pendingSection;
			turn.next.stepEvent = pendingStep ? state.pendingEvent : 0;
			turn.next.stepRank = pendingStep ? state.pendingRank : 0;
			turn.next.sectionEvent = pendingStep ? 0 : state.pendingSectionEvent;
			turn.to =
				sectionOf(plan, pendingStep ? stepOf(plan, state.pendingStep).section : state.pendingSection).start;
			turn.at = position;
			if (current >= 0 && plan.jump != ArrangerJump::None)
			{
				const ArrangerSection& section = sectionOf(plan, current);
				const auto point = std::lower_bound(section.jumps.begin(), section.jumps.end(), position);
				turn.at = std::max(position, point != section.jumps.end() ? *point : section.end);
			}
			return turn;
		}
		if (current < 0)
			return turn;
		const ArrangerSection& section = sectionOf(plan, current);
		turn.at = std::max(section.end, position);
		turn.next = state;
		turn.to = section.start;
		if (state.step < 0)
			return turn; // a triggered event loops
		const ArrangerPlanStep& step = stepOf(plan, state.step);
		if (step.mode == ArrangerRepeat::Forever || state.repeat + 1 < step.repeats)
		{
			turn.next.repeat = state.repeat + 1;
			return turn;
		}
		turn.stops = step.mode == ArrangerRepeat::Pause;
		turn.next = ArrangerState();
		if (validStep(plan, state.step + 1))
		{
			turn.next.step = state.step + 1;
			turn.next.stepEvent = stepEvent(plan, state.step + 1);
			turn.next.stepRank = rankOf(plan, state.step + 1);
			turn.to = sectionOf(plan, stepOf(plan, state.step + 1).section).start;
		}
		else
		{
			turn.to = turn.at; // after the chain playback goes on
		}
		return turn;
	}

	namespace
	{
		// The packed words: step and section 20 bits each, the repeats 10 (they are 1 to 999; a Forever loop's
		// counter clamps at the top of the field), the entering flag 1, and the rest of both words is the plan's
		// generation (#885).
		constexpr int g_indexBits = 20;
		constexpr int g_repeatBits = 10;
		constexpr int g_enteringBit = g_indexBits + g_repeatBits + g_indexBits; // 50
		constexpr int g_generationLowBits = 64 - (g_enteringBit + 1);           // 13, after the entering bit
		constexpr int g_generationHighBits = 64 - g_enteringBit;                // 14, the rest of the second word
		constexpr uint64_t g_generationLowMask = (uint64_t{1} << g_generationLowBits) - 1;
		constexpr uint64_t g_generationHighMask = (uint64_t{1} << g_generationHighBits) - 1;

		uint64_t bits(int32_t value, int width, int shift)
		{
			const int64_t mask = (int64_t{1} << width) - 1;
			return static_cast<uint64_t>(std::clamp<int64_t>(value, 0, mask)) << shift;
		}

		int32_t bitsAt(int64_t packed, int width, int shift)
		{
			const uint64_t mask = (uint64_t{1} << width) - 1;
			return static_cast<int32_t>((static_cast<uint64_t>(packed) >> shift) & mask);
		}
	}

	std::pair<int64_t, int64_t> pack(const ArrangerState& state, uint64_t generation) noexcept
	{
		// Unsigned assembly: the generation's top bits may land in a word's sign bit.
		const uint64_t gen = generation % g_arrangerGenerations;
		const uint64_t a = bits(state.step + 1, g_indexBits, 0) | bits(state.repeat, g_repeatBits, g_indexBits) |
			bits(state.section + 1, g_indexBits, g_indexBits + g_repeatBits) |
			(state.entering ? uint64_t{1} << g_enteringBit : uint64_t{0}) |
			(gen & g_generationLowMask) << (g_enteringBit + 1);
		const uint64_t b = bits(state.pendingStep + 1, g_indexBits, 0) |
			bits(state.pendingRepeat, g_repeatBits, g_indexBits) |
			bits(state.pendingSection + 1, g_indexBits, g_indexBits + g_repeatBits) |
			((gen >> g_generationLowBits) & g_generationHighMask) << g_enteringBit;
		return {static_cast<int64_t>(a), static_cast<int64_t>(b)};
	}

	ArrangerState unpackArranger(int64_t a, int64_t b) noexcept
	{
		ArrangerState state;
		state.step = bitsAt(a, g_indexBits, 0) - 1;
		state.repeat = bitsAt(a, g_repeatBits, g_indexBits);
		state.section = bitsAt(a, g_indexBits, g_indexBits + g_repeatBits) - 1;
		state.entering = ((static_cast<uint64_t>(a) >> g_enteringBit) & 1U) != 0;
		state.pendingStep = bitsAt(b, g_indexBits, 0) - 1;
		state.pendingRepeat = bitsAt(b, g_repeatBits, g_indexBits);
		state.pendingSection = bitsAt(b, g_indexBits, g_indexBits + g_repeatBits) - 1;
		return state;
	}

	uint64_t unpackGeneration(int64_t a, int64_t b) noexcept
	{
		return (static_cast<uint64_t>(a) >> (g_enteringBit + 1)) |
			((static_cast<uint64_t>(b) >> g_enteringBit) << g_generationLowBits);
	}
}
