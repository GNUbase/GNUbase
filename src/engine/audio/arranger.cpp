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
	}

	ArrangerState arrangerEnter(const ArrangerPlan& plan, const ArrangerState& state, time::Samples position) noexcept
	{
		ArrangerState result; // a step or event that waits still does
		result.pendingStep = state.pendingStep;
		result.pendingRepeat = state.pendingRepeat;
		result.pendingSection = state.pendingSection;
		if (!plan.plays())
			return result;
		if (validStep(plan, state.step) && holds(plan, stepOf(plan, state.step).section, position))
		{
			result.step = state.step;
			result.repeat = state.repeat;
			return result;
		}
		if (state.step < 0 && holds(plan, state.section, position))
		{
			result.section = state.section;
			return result;
		}
		for (size_t s = 0; s < plan.steps.size(); ++s)
			if (holds(plan, plan.steps[s].section, position))
			{
				result.step = static_cast<int32_t>(s);
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

	std::pair<int64_t, int64_t> pack(const ArrangerState& state) noexcept
	{
		return {field(state.step + 1, 0) | field(state.repeat, g_fieldBits) |
				field(state.section + 1, 2 * g_fieldBits) | (state.entering ? int64_t{1} << (3 * g_fieldBits) : 0),
			field(state.pendingStep + 1, 0) | field(state.pendingRepeat, g_fieldBits) |
				field(state.pendingSection + 1, 2 * g_fieldBits)};
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
}
