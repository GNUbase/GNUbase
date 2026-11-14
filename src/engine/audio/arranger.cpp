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

		// The state's indices found again in plan by the events they play in previous (#885): a step that is still
		// there keeps its repeat, clamped to the ones it has now, and one that is gone leaves the position to decide;
		// a step or event that waits and is gone is dropped, instead of taking over as whatever its old index names.
		ArrangerState remapped(
			const ArrangerPlan& previous, const ArrangerPlan& plan, const ArrangerState& state) noexcept
		{
			ArrangerState result = state;
			if (validStep(previous, state.step))
			{
				const int32_t step = nthStepOf(plan, stepEvent(previous, state.step), rankOf(previous, state.step));
				result.step = step;
				result.section = -1;
				result.repeat = step >= 0 ? std::min(state.repeat, stepOf(plan, step).repeats - 1) : 0;
			}
			else if (state.step < 0 && validSection(previous, state.section))
			{
				result.section = sectionOfEvent(plan, sectionOf(previous, state.section).event);
			}
			else
			{
				result.step = -1;
				result.section = -1;
				result.repeat = 0;
			}
			if (validStep(previous, state.pendingStep))
			{
				const int32_t step =
					nthStepOf(plan, stepEvent(previous, state.pendingStep), rankOf(previous, state.pendingStep));
				result.pendingStep = step;
				result.pendingRepeat = step >= 0 ? std::min(state.pendingRepeat, stepOf(plan, step).repeats - 1) : 0;
				result.pendingSection = -1;
			}
			else if (state.pendingStep < 0 && validSection(previous, state.pendingSection))
			{
				result.pendingSection = sectionOfEvent(plan, sectionOf(previous, state.pendingSection).event);
			}
			else
			{
				result.pendingStep = -1;
				result.pendingRepeat = 0;
				result.pendingSection = -1;
			}
			return result;
		}
	}

	ArrangerState arrangerEnter(const ArrangerPlan& plan, const ArrangerPlan* previous, const ArrangerState& state,
		time::Samples position) noexcept
	{
		// Indices into another plan are found again by their event first; a step or event that waits still does.
		const ArrangerState mapped =
			previous != nullptr && previous != &plan ? remapped(*previous, plan, state) : state;
		ArrangerState result;
		result.pendingStep = mapped.pendingStep;
		result.pendingRepeat = mapped.pendingRepeat;
		result.pendingSection = mapped.pendingSection;
		if (!plan.plays())
			return result;
		if (validStep(plan, mapped.step) && holds(plan, stepOf(plan, mapped.step).section, position))
		{
			result.step = mapped.step;
			result.repeat = mapped.repeat;
			return result;
		}
		if (mapped.step < 0 && holds(plan, mapped.section, position))
		{
			result.section = mapped.section;
			return result;
		}
		for (size_t s = 0; s < plan.steps.size(); ++s)
			if (holds(plan, plan.steps[s].section, position))
			{
				result.step = static_cast<int32_t>(s);
				// A step of the event playing keeps the repeat it had, as far as this step allows (#885).
				result.repeat = mapped.step >= 0 ? std::min(mapped.repeat, plan.steps[s].repeats - 1) : 0;
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
		constexpr int g_fieldBits = 20;
		constexpr int64_t g_fieldMask = (int64_t{1} << g_fieldBits) - 1;

		int64_t field(int32_t value, int shift)
		{
			return std::clamp<int64_t>(value, 0, g_fieldMask) << shift;
		}

		int32_t fieldAt(int64_t packed, int shift)
		{
			return static_cast<int32_t>((packed >> shift) & g_fieldMask);
		}
	}

	std::pair<int64_t, int64_t> pack(const ArrangerState& state, uint64_t generation) noexcept
	{
		// The generation rides in the bits the fields leave free: three at the top of the first word, four at the top
		// of the second. Unsigned assembly: its top bit may land in a word's sign bit.
		const uint64_t gen = generation % g_arrangerGenerations;
		const uint64_t a = static_cast<uint64_t>(field(state.step + 1, 0)) |
			static_cast<uint64_t>(field(state.repeat, g_fieldBits)) |
			static_cast<uint64_t>(field(state.section + 1, 2 * g_fieldBits)) |
			(state.entering ? uint64_t{1} << (3 * g_fieldBits) : uint64_t{0}) | (gen & 7) << 61;
		const uint64_t b = static_cast<uint64_t>(field(state.pendingStep + 1, 0)) |
			static_cast<uint64_t>(field(state.pendingRepeat, g_fieldBits)) |
			static_cast<uint64_t>(field(state.pendingSection + 1, 2 * g_fieldBits)) | ((gen >> 3) & 15) << 60;
		return {static_cast<int64_t>(a), static_cast<int64_t>(b)};
	}

	ArrangerState unpackArranger(int64_t a, int64_t b) noexcept
	{
		ArrangerState state;
		state.step = fieldAt(a, 0) - 1;
		state.repeat = fieldAt(a, g_fieldBits);
		state.section = fieldAt(a, 2 * g_fieldBits) - 1;
		state.entering = ((a >> (3 * g_fieldBits)) & 1) != 0;
		state.pendingStep = fieldAt(b, 0) - 1;
		state.pendingRepeat = fieldAt(b, g_fieldBits);
		state.pendingSection = fieldAt(b, 2 * g_fieldBits) - 1;
		return state;
	}

	uint64_t unpackGeneration(int64_t a, int64_t b) noexcept
	{
		return (static_cast<uint64_t>(a) >> 61) | (static_cast<uint64_t>(b) >> 60 << 3);
	}
}
