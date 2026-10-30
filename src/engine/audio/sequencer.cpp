// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sequencer.h"

#include "engine/graph/graph.h"

#include <algorithm>
#include <limits>
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

		// Where a controller event is remembered while chasing, or -1 if chase doesn't chase it.
		int chasedIndex(const RenderController& controller, uint8_t chase)
		{
			switch (controller.type)
			{
			case ControllerType::Control:
				return (chase & g_chaseControllers) != 0 ? ((controller.channel % 16) * 128) + (controller.number % 128)
														 : -1;
			case ControllerType::Program:
				return (chase & g_chasePrograms) != 0
					? static_cast<int>(MidiSequencer::g_notes) + (controller.channel % 16)
					: -1;
			case ControllerType::PitchBend:
				return (chase & g_chasePitchBend) != 0
					? static_cast<int>(MidiSequencer::g_notes) + 16 + (controller.channel % 16)
					: -1;
			case ControllerType::ChannelPressure:
			case ControllerType::PolyPressure:
			case ControllerType::SysEx:
				return -1;
			}
			return -1;
		}

		// How many controller events chase can find at most.
		size_t chasedCount(uint8_t chase)
		{
			return ((chase & g_chaseControllers) != 0 ? MidiSequencer::g_notes : 0) +
				((chase & g_chasePrograms) != 0 ? 16 : 0) + ((chase & g_chasePitchBend) != 0 ? 16 : 0);
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

		graph::ProcessEvent controllerEvent(const RenderController& controller, uint32_t offset)
		{
			using Type = graph::ProcessEvent::Type;
			graph::ProcessEvent event;
			event.offset = offset;
			event.channel = controller.channel;
			event.key = controller.number;
			event.value = controller.value;
			switch (controller.type)
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
				event.data = controller.data.data();
				event.size = static_cast<uint32_t>(controller.data.size());
				break;
			}
			return event;
		}
	}

	MidiSequencer::MidiSequencer() : m_sequence(std::make_unique<const MidiSequence>())
	{
	}

	void MidiSequencer::setSequence(std::unique_ptr<MidiSequence> sequence)
	{
		sequence->reach.resize(sequence->notes.size());
		int64_t reach = std::numeric_limits<int64_t>::min();
		for (size_t i = 0; i < sequence->notes.size(); ++i)
			sequence->reach[i] = reach = std::max(reach, sequence->notes[i].end);
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
			m_pending = 0;
			if (m_next >= 0)
				m_cursor = m_next; // where playback stopped
			m_next = -1;
			// A locate while stopped chases the controller events, again in the next block if they didn't fit.
			if (m_cursor < 0 || transport.position == m_cursor ||
				chaseControllers(sequence, transport.position, 0, chase, context.eventsOut))
				m_cursor = transport.position;
			return;
		}
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			if (segment.position != m_next) // a start, a locate or a cycle jump
			{
				endAll(segment.offset, context.eventsOut);
				m_pending = chase;
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
			const int64_t chasing = (m_pending & (g_chaseNotes | g_chaseControllerEvents)) != 0 ? begin : g_never;
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
			else if ((m_pending & g_chaseControllerEvents) != 0)
			{
				if (!chaseControllers(sequence, begin, offset, m_pending, out))
					return;
				m_pending &= static_cast<uint8_t>(~g_chaseControllerEvents);
			}
			else if (changing == at)
			{
				if (!out.add(controllerEvent(*controller, offset)))
					return;
				++controller;
			}
			else if ((m_pending & g_chaseNotes) != 0)
			{
				if (!chaseNotes(sequence, begin, offset, out))
					return;
				m_pending = 0;
			}
			else
			{
				if (note->end > note->start && !noteOn(*note, offset, out))
					return;
				++note;
			}
		}
	}

	bool MidiSequencer::chaseControllers(const MidiSequence& sequence, int64_t position, uint32_t offset, uint8_t chase,
		graph::EventBuffer& out) noexcept
	{
		const size_t wanted = chasedCount(chase);
		if (wanted == 0)
			return true;
		const std::vector<RenderController>& controllers = sequence.controllers;
		const auto before = [](const RenderController& c, int64_t at) { return c.position < at; };
		auto first = controllers.begin();
		if ((chase & g_chaseBeyondParts) == 0)
		{
			// The earliest start of the parts under the position; none: nothing to chase.
			int64_t start = g_never;
			for (const RenderPart& part : sequence.parts)
			{
				if (part.start > position)
					break;
				if (part.end > position)
					start = std::min(start, part.start);
			}
			if (start == g_never)
				return true;
			first = std::lower_bound(controllers.begin(), controllers.end(), start, before);
		}

		// Back from the position, the last controller event of each kind.
		size_t found = 0;
		for (auto c = std::lower_bound(first, controllers.end(), position, before); c != first && found < wanted;)
		{
			--c;
			const int index = chasedIndex(*c, chase);
			if (index < 0 || m_chased[static_cast<size_t>(index)])
				continue;
			m_chased[static_cast<size_t>(index)] = true;
			m_found[found++] = static_cast<size_t>(c - controllers.begin());
		}

		// Sent in the order they were sent before.
		bool room = true;
		for (size_t i = found; i-- > 0;)
		{
			const RenderController& controller = controllers[m_found[i]];
			m_chased[static_cast<size_t>(chasedIndex(controller, chase))] = false;
			room = room && out.add(controllerEvent(controller, offset));
		}
		return room;
	}

	bool MidiSequencer::chaseNotes(
		const MidiSequence& sequence, int64_t position, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		// From the first note that may still sound up to those that start at the position.
		const std::vector<RenderNote>& notes = sequence.notes;
		const auto first = static_cast<size_t>(
			std::upper_bound(sequence.reach.begin(), sequence.reach.end(), position) - sequence.reach.begin());
		const auto last = static_cast<size_t>(std::lower_bound(notes.begin(), notes.end(), position,
												  [](const RenderNote& n, int64_t at) { return n.start < at; }) -
			notes.begin());
		for (size_t i = first; i < last; ++i)
		{
			const RenderNote& note = notes[i];
			if (note.end <= position)
				continue;
			const auto index = static_cast<uint16_t>(((note.channel % 16) * 128) + (note.pitch % 128));
			if (sounds(index))
				m_ends[index] = note.end; // started again before the position: sounds on until the later one ends
			else if (!noteOn(note, offset, out))
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
