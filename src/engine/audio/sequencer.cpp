// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sequencer.h"

#include "engine/graph/graph.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr int64_t g_never = std::numeric_limits<int64_t>::max();
		constexpr int64_t g_now = std::numeric_limits<int64_t>::min(); // the end of a note to end at once

		// MidiChase as the flags the audio thread reads.
		constexpr uint8_t g_chaseNotes = 1;
		constexpr uint8_t g_chaseControllers = 2;
		constexpr uint8_t g_chasePrograms = 4;
		constexpr uint8_t g_chasePitchBend = 8;
		constexpr uint8_t g_chaseBeyondParts = 16;
		constexpr uint8_t g_chaseControllerEvents = g_chaseControllers | g_chasePrograms | g_chasePitchBend;

		// A controller event's kind for chasing (ChaseLane::kind), or -1 if it isn't chased.
		int chaseKind(const RenderController& controller)
		{
			const int channel = controller.channel % 16;
			switch (controller.type)
			{
			case ControllerType::Control:
				return (channel * 128) + (controller.number % 128);
			case ControllerType::Program:
				return static_cast<int>(MidiSequencer::g_notes) + channel;
			case ControllerType::PitchBend:
				return static_cast<int>(MidiSequencer::g_notes) + 16 + channel;
			case ControllerType::ChannelPressure:
			case ControllerType::PolyPressure:
			case ControllerType::SysEx:
				return -1;
			}
			return -1;
		}

		bool chases(int kind, uint8_t chase)
		{
			if (kind < static_cast<int>(MidiSequencer::g_notes))
				return (chase & g_chaseControllers) != 0;
			if (kind < static_cast<int>(MidiSequencer::g_notes) + 16)
				return (chase & g_chasePrograms) != 0;
			return (chase & g_chasePitchBend) != 0;
		}

		// Indexes events by kind: order gets the indices of those with a kind, by kind and then in their order, lanes
		// the runs of one kind.
		template <typename Event, typename KindOf> void indexByKind(const std::vector<Event>& events, KindOf kindOf,
			std::vector<uint32_t>& order, std::vector<ChaseLane>& lanes)
		{
			order.clear();
			lanes.clear();
			for (size_t i = 0; i < events.size(); ++i)
				if (kindOf(events[i]) >= 0)
					order.push_back(static_cast<uint32_t>(i));
			std::stable_sort(order.begin(), order.end(),
				[&](uint32_t a, uint32_t b) { return kindOf(events[a]) < kindOf(events[b]); });
			for (size_t i = 0; i < order.size(); ++i)
			{
				const auto kind = static_cast<uint16_t>(kindOf(events[order[i]]));
				if (lanes.empty() || lanes.back().kind != kind)
					lanes.push_back({kind, static_cast<uint32_t>(i), static_cast<uint32_t>(i)});
				lanes.back().last = static_cast<uint32_t>(i + 1);
			}
		}

		// The last of a lane's events before a position, or nullptr.
		template <typename Event, typename PositionOf> const Event* lastBefore(const std::vector<Event>& events,
			const std::vector<uint32_t>& order, const ChaseLane& lane, int64_t position, PositionOf positionOf)
		{
			const auto first = order.begin() + lane.first;
			const auto found = std::lower_bound(first, order.begin() + lane.last, position,
				[&](uint32_t i, int64_t at) { return positionOf(events[i]) < at; });
			return found == first ? nullptr : &events[*std::prev(found)];
		}

		graph::ProcessEvent noteEvent(graph::ProcessEvent::Type type, uint16_t index, uint32_t offset)
		{
			graph::ProcessEvent event;
			event.offset = offset;
			event.type = type;
			event.channel = static_cast<uint8_t>(index / 128);
			event.key = static_cast<uint8_t>(index % 128);
			return event;
		}

		graph::ProcessEvent controllerEvent(
			ControllerType type, uint8_t channel, uint8_t number, uint32_t value, uint32_t offset)
		{
			using Type = graph::ProcessEvent::Type;
			graph::ProcessEvent event;
			event.offset = offset;
			event.channel = channel;
			event.key = number;
			event.value = value;
			switch (type)
			{
			case ControllerType::Control:
				event.type = Type::Controller;
				break;
			case ControllerType::PitchBend:
				event.type = Type::PitchBend;
				event.key = 0;
				break;
			case ControllerType::ChannelPressure:
				event.type = Type::ChannelPressure;
				event.key = 0;
				break;
			case ControllerType::PolyPressure:
				event.type = Type::PolyPressure;
				break;
			case ControllerType::Program:
				event.type = Type::Program;
				break;
			case ControllerType::SysEx:
				event.type = Type::SysEx;
				event.channel = 0;
				event.key = 0;
				event.value = 0;
				break;
			}
			return event;
		}

		graph::ProcessEvent controllerEvent(const RenderController& controller, uint32_t offset)
		{
			graph::ProcessEvent event =
				controllerEvent(controller.type, controller.channel, controller.number, controller.value, offset);
			if (controller.type == ControllerType::SysEx)
			{
				event.data = controller.data.data();
				event.size = static_cast<uint32_t>(controller.data.size());
			}
			return event;
		}
	}

	ChaseTrack makeChaseTrack(const std::vector<RenderController>& controllers, std::vector<RenderPart> parts)
	{
		ChaseTrack track;
		for (const RenderController& controller : controllers)
		{
			if (chaseKind(controller) < 0)
				continue;
			RenderController copy = controller;
			copy.data.clear();
			track.controllers.push_back(std::move(copy));
		}
		track.parts = std::move(parts);
		indexByKind(track.controllers, chaseKind, track.order, track.lanes);
		return track;
	}

	MidiSequencer::MidiSequencer() : m_sequence(std::make_unique<const MidiSequence>())
	{
	}

	void MidiSequencer::setSequence(std::unique_ptr<MidiSequence> sequence)
	{
		// Notes of no length don't play, so they don't take the place of an earlier note of their pitch either.
		indexByKind(
			sequence->notes, [](const RenderNote& note)
			{ return note.end > note.start ? ((note.channel % 16) * 128) + (note.pitch % 128) : -1; },
			sequence->noteOrder, sequence->noteLanes);
		m_sequence.publish(std::move(sequence));
	}

	void MidiSequencer::setChase(const MidiChase& chase) noexcept
	{
		const auto flag = [](bool on, uint8_t value) { return on ? value : uint8_t{0}; };
		m_chase.store(static_cast<uint8_t>(flag(chase.notes, g_chaseNotes) |
						  flag(chase.controllers, g_chaseControllers) | flag(chase.programs, g_chasePrograms) |
						  flag(chase.pitchBend, g_chasePitchBend) | flag(chase.beyondParts, g_chaseBeyondParts)),
			std::memory_order_relaxed);
	}

	void MidiSequencer::process(const graph::ProcessContext& context) noexcept
	{
		// The sequence stays in use until the next block, so SysEx data lives as long as the events pointing at it.
		const MidiSequence& sequence = m_sequence.acquire();
		for (const graph::ProcessEvent& event : context.eventsIn)
			context.eventsOut.add(event);
		context.eventsOut.nextSource();

		const uint8_t chase = m_chase.load(std::memory_order_relaxed);
		const graph::TransportBlock& transport = context.transport;
		if (!transport.playing)
		{
			endAll(0, context.eventsOut);
			m_chaseNotes = false;
			if (m_next >= 0)
				m_cursor = m_next; // where playback stopped
			m_next = -1;
			if (m_cursor >= 0 && transport.position != m_cursor) // a locate while stopped
				findChased(sequence, transport.position, chase);
			m_cursor = transport.position;
			sendChased(0, context.eventsOut);
			return;
		}
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			if (segment.position != m_next) // a start, a locate or a cycle jump
			{
				endAll(segment.offset, context.eventsOut);
				findChased(sequence, segment.position, chase);
				m_chaseNotes = (chase & g_chaseNotes) != 0;
			}
			play(sequence, segment, context.eventsOut);
			m_next = segment.position + segment.frames;
		}
	}

	void MidiSequencer::play(
		const MidiSequence& sequence, const graph::TransportSegment& segment, graph::EventBuffer& out) noexcept
	{
		const int64_t begin = segment.position;
		const int64_t end = begin + segment.frames;
		auto note = std::lower_bound(sequence.notes.begin(), sequence.notes.end(), begin,
			[](const RenderNote& n, int64_t at) { return n.start < at; });
		auto controller = std::lower_bound(sequence.controllers.begin(), sequence.controllers.end(), begin,
			[](const RenderController& c, int64_t at) { return c.position < at; });

		// Event by event in time order; at one sample note-offs, then controller events, then note-ons. A chase
		// takes place at the segment's start: its controller events before those at that position, its notes before
		// the notes that start there. When the buffer is full, the rest of the segment is skipped.
		for (;;)
		{
			int64_t ending = g_never;
			for (size_t i = 0; i < m_soundingCount; ++i)
				ending = std::min(ending, std::max(m_ends[m_sounding[i]], begin));
			const int64_t starting = note != sequence.notes.end() ? note->start : g_never;
			const int64_t changing = controller != sequence.controllers.end() ? controller->position : g_never;
			const int64_t chasing = m_chasedSent < m_chasedCount || m_chaseNotes ? begin : g_never;
			const int64_t at = std::min({ending, starting, changing, chasing});
			if (at >= end)
				return;
			const auto offset = segment.offset + static_cast<uint32_t>(at - begin);
			if (ending == at)
			{
				for (size_t i = 0; i < m_soundingCount;)
				{
					const uint16_t index = m_sounding[i];
					if (m_ends[index] > at)
						++i;
					else if (!noteOff(index, offset, out)) // removes it from m_sounding
						return;
				}
			}
			else if (m_chasedSent < m_chasedCount)
			{
				if (!sendChased(offset, out))
					return;
			}
			else if (changing == at)
			{
				if (!out.add(controllerEvent(*controller, offset)))
					return;
				++controller;
			}
			else if (m_chaseNotes)
			{
				if (!chaseNotes(sequence, begin, offset, out))
					return;
				m_chaseNotes = false;
			}
			else
			{
				if (note->end > note->start && !noteOn(*note, offset, out))
					return;
				++note;
			}
		}
	}

	void MidiSequencer::findChased(const MidiSequence& sequence, int64_t position, uint8_t chase) noexcept
	{
		for (size_t i = m_chasedSent; i < m_chasedCount; ++i) // the rest of a chase that didn't fit
			m_chased[i] = Chased();
		m_chasedCount = 0;
		m_chasedSent = 0;
		if ((chase & g_chaseControllerEvents) == 0)
			return;
		for (size_t t = 0; t < sequence.chase.size(); ++t)
		{
			const ChaseTrack& track = sequence.chase[t];
			const auto index = static_cast<uint16_t>(t);
			if ((chase & g_chaseBeyondParts) != 0)
			{
				consider(track, index, std::numeric_limits<int64_t>::min(), position, chase);
				continue;
			}
			for (const RenderPart& part : track.parts) // those under the position, each from its start
			{
				if (part.start > position)
					break;
				if (part.end > position)
					consider(track, index, part.start, position, chase);
			}
		}

		// The kinds found, in time order; at one position by track, then kind, so a bank select precedes its program.
		for (size_t kind = 0; kind < m_chased.size(); ++kind)
			if (m_chased[kind].position != std::numeric_limits<int64_t>::min())
			{
				m_chased[m_chasedCount++] = m_chased[kind];
				if (m_chasedCount - 1 != kind)
					m_chased[kind] = Chased();
			}
		std::sort(m_chased.begin(), m_chased.begin() + static_cast<std::ptrdiff_t>(m_chasedCount),
			[](const Chased& a, const Chased& b)
			{ return std::tie(a.position, a.track, a.kind) < std::tie(b.position, b.track, b.kind); });
	}

	void MidiSequencer::consider(
		const ChaseTrack& track, uint16_t trackIndex, int64_t from, int64_t position, uint8_t chase) noexcept
	{
		for (const ChaseLane& lane : track.lanes)
		{
			if (!chases(lane.kind, chase))
				continue;
			const RenderController* controller = lastBefore(
				track.controllers, track.order, lane, position, [](const RenderController& c) { return c.position; });
			Chased& chased = m_chased[lane.kind];
			if (controller == nullptr || controller->position < from || controller->position < chased.position)
				continue;
			chased = {controller->position, controller->value, trackIndex, lane.kind, controller->type,
				controller->number, controller->channel};
		}
	}

	bool MidiSequencer::sendChased(uint32_t offset, graph::EventBuffer& out) noexcept
	{
		for (; m_chasedSent < m_chasedCount; ++m_chasedSent)
		{
			const Chased& chased = m_chased[m_chasedSent];
			if (!out.add(controllerEvent(chased.type, chased.channel, chased.number, chased.value, offset)))
				return false;
			m_chased[m_chasedSent] = Chased();
		}
		m_chasedCount = 0;
		m_chasedSent = 0;
		return true;
	}

	bool MidiSequencer::chaseNotes(
		const MidiSequence& sequence, int64_t position, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		// Of each pitch the last note that started before the position, if it still sounds there, in their order.
		size_t count = 0;
		for (const ChaseLane& lane : sequence.noteLanes)
		{
			const RenderNote* note = lastBefore(
				sequence.notes, sequence.noteOrder, lane, position, [](const RenderNote& n) { return n.start; });
			if (note != nullptr && note->end > position)
				m_chasedNotes[count++] = static_cast<uint32_t>(note - sequence.notes.data());
		}
		std::sort(m_chasedNotes.begin(), m_chasedNotes.begin() + static_cast<std::ptrdiff_t>(count));
		for (size_t i = 0; i < count; ++i)
		{
			const RenderNote& note = sequence.notes[m_chasedNotes[i]];
			const auto index = static_cast<uint16_t>(((note.channel % 16) * 128) + (note.pitch % 128));
			if (!sounds(index) && !noteOn(note, offset, out)) // sounds: chased in an earlier block
				return false;
		}
		return true;
	}

	bool MidiSequencer::sounds(uint16_t index) const noexcept
	{
		return std::binary_search(soundingBegin(), soundingEnd(), index);
	}

	bool MidiSequencer::noteOn(const RenderNote& note, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		const auto index = static_cast<uint16_t>(((note.channel % 16) * 128) + (note.pitch % 128));
		if (!noteOff(index, offset, out)) // if it sounds, it ends first
			return false;
		graph::ProcessEvent event = noteEvent(graph::ProcessEvent::Type::NoteOn, index, offset);
		event.velocity = note.velocity;
		if (!out.add(event))
			return false;
		uint16_t* const first = m_sounding.data();
		uint16_t* const last = first + m_soundingCount;
		uint16_t* const position = std::lower_bound(first, last, index);
		std::move_backward(position, last, last + 1);
		*position = index;
		++m_soundingCount;
		m_ends[index] = note.end;
		return true;
	}

	bool MidiSequencer::noteOff(uint16_t index, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		uint16_t* const first = m_sounding.data();
		uint16_t* const last = first + m_soundingCount;
		uint16_t* const position = std::lower_bound(first, last, index);
		if (position == last || *position != index)
			return true;
		if (!out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, index, offset)))
			return false;
		std::move(position + 1, last, position);
		--m_soundingCount;
		return true;
	}

	void MidiSequencer::endAll(uint32_t offset, graph::EventBuffer& out) noexcept
	{
		size_t kept = 0;
		for (size_t i = 0; i < m_soundingCount; ++i)
		{
			const uint16_t index = m_sounding[i];
			if (out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, index, offset)))
				continue;
			m_ends[index] = g_now; // wherever playback goes on
			m_sounding[kept++] = index;
		}
		m_soundingCount = kept;
	}

	InstrumentChannel::InstrumentChannel(
		std::shared_ptr<MidiSequencer> sequencer, std::shared_ptr<graph::Processor> instrument)
		: m_sequencer(std::move(sequencer)), m_instrument(std::move(instrument)), m_events(graph::g_maxEventsPerBlock)
	{
	}

	void InstrumentChannel::prepare(double sampleRate, uint32_t maxFrames)
	{
		m_sequencer->prepareFor(sampleRate, maxFrames);
		if (m_instrument)
			m_instrument->prepareFor(sampleRate, maxFrames);
	}

	void InstrumentChannel::releaseNotesOf(
		MidiSequencer& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept
	{
		for (const uint16_t* note = departed.soundingBegin(); note != departed.soundingEnd(); ++note)
		{
			const uint16_t index = *note;
			if (!m_held[index] || m_ending[index] || m_sequencer->sounds(index) ||
				std::any_of(staying.begin(), staying.end(),
					[index](const std::shared_ptr<MidiSequencer>& source) { return source->sounds(index); }))
				continue;
			m_ending[index] = true;
			++m_endingCount;
		}
		departed.forget();
	}

	void InstrumentChannel::process(const graph::ProcessContext& context) noexcept
	{
		m_events.clear();
		m_sequencer->process(
			{context.audio, context.channels, context.frames, context.eventsIn, m_events, context.transport});

		// At a stop every note ends, also those whose source can't end them any more.
		if (m_playing && !context.transport.playing)
			for (size_t index = 0; index < m_held.size(); ++index)
				if (m_held[index] && !m_ending[index])
				{
					m_ending[index] = true;
					++m_endingCount;
				}
		m_playing = context.transport.playing;

		for (const graph::ProcessEvent& event : m_events)
		{
			if (event.type != graph::ProcessEvent::Type::NoteOn && event.type != graph::ProcessEvent::Type::NoteOff)
				continue;
			const size_t index = ((event.channel % 16) * 128) + (event.key % 128);
			m_held[index] = event.type == graph::ProcessEvent::Type::NoteOn;
			if (m_ending[index]) // ended here, or started again
			{
				m_ending[index] = false;
				--m_endingCount;
			}
		}
		m_events.nextSource();
		for (size_t index = 0; m_endingCount > 0 && index < m_ending.size(); ++index)
		{
			if (!m_ending[index])
				continue;
			if (!m_events.add(noteEvent(graph::ProcessEvent::Type::NoteOff, static_cast<uint16_t>(index), 0)))
				break; // the next block
			m_held[index] = false;
			m_ending[index] = false;
			--m_endingCount;
		}

		if (m_instrument)
			m_instrument->process(
				{context.audio, context.channels, context.frames, m_events, context.eventsOut, context.transport});
	}
}
