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

	void MidiSequencer::setSequence(std::unique_ptr<const MidiSequence> sequence)
	{
		m_sequence.publish(std::move(sequence));
	}

	void MidiSequencer::process(const graph::ProcessContext& context) noexcept
	{
		// The sequence stays in use until the next block, so SysEx data lives as long as the events pointing at it.
		const MidiSequence& sequence = m_sequence.acquire();
		for (const graph::ProcessEvent& event : context.eventsIn)
			context.eventsOut.add(event);

		const graph::TransportBlock& transport = context.transport;
		if (!transport.playing)
		{
			endAll(0, context.eventsOut);
			m_next = -1;
			return;
		}
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			if (segment.position != m_next)
				endAll(segment.offset, context.eventsOut); // a start, a locate or a cycle jump
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

		// Event by event in time order; at one sample note-offs, then controller events, then note-ons.
		for (;;)
		{
			int64_t ending = g_never;
			for (size_t i = 0; i < m_soundingCount; ++i)
				ending = std::min(ending, std::max(m_ends[m_sounding[i]], begin));
			const int64_t starting = note != sequence.notes.end() ? note->start : g_never;
			const int64_t changing = controller != sequence.controllers.end() ? controller->position : g_never;
			const int64_t at = std::min({ending, starting, changing});
			if (at >= end)
				break;
			const auto offset = segment.offset + static_cast<uint32_t>(at - begin);
			if (ending == at)
			{
				for (size_t i = 0; i < m_soundingCount;)
				{
					const uint16_t index = m_sounding[i];
					if (m_ends[index] <= at)
						noteOff(index, offset, out); // removes it from m_sounding
					else
						++i;
				}
			}
			else if (changing == at)
			{
				out.add(controllerEvent(*controller, offset));
				++controller;
			}
			else
			{
				if (note->end > note->start)
					noteOn(*note, offset, out);
				++note;
			}
		}
	}

	void MidiSequencer::noteOn(const RenderNote& note, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		const auto index = static_cast<uint16_t>(((note.channel % 16) * 128) + (note.pitch % 128));
		const auto first = m_sounding.begin();
		const auto last = first + static_cast<std::ptrdiff_t>(m_soundingCount);
		const auto position = std::lower_bound(first, last, index);
		if (position != last && *position == index)
		{
			out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, index, offset)); // it sounds: end it first
		}
		else
		{
			std::move_backward(position, last, last + 1);
			*position = index;
			++m_soundingCount;
		}
		m_ends[index] = note.end;
		graph::ProcessEvent event = noteEvent(graph::ProcessEvent::Type::NoteOn, index, offset);
		event.velocity = note.velocity;
		out.add(event);
	}

	void MidiSequencer::noteOff(uint16_t index, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		const auto first = m_sounding.begin();
		const auto last = first + static_cast<std::ptrdiff_t>(m_soundingCount);
		const auto position = std::lower_bound(first, last, index);
		if (position == last || *position != index)
			return;
		std::move(position + 1, last, position);
		--m_soundingCount;
		out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, index, offset));
	}

	void MidiSequencer::endAll(uint32_t offset, graph::EventBuffer& out) noexcept
	{
		for (size_t i = 0; i < m_soundingCount; ++i)
			out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, m_sounding[i], offset));
		m_soundingCount = 0;
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

	void InstrumentChannel::process(const graph::ProcessContext& context) noexcept
	{
		m_events.clear();
		if (m_release.exchange(false, std::memory_order_acq_rel))
			for (size_t index = 0; index < m_held.size(); ++index)
				if (m_held[index])
					m_events.add(noteEvent(graph::ProcessEvent::Type::NoteOff, static_cast<uint16_t>(index), 0));
		m_sequencer->process(
			{context.audio, context.channels, context.frames, context.eventsIn, m_events, context.transport});

		for (const graph::ProcessEvent& event : m_events)
		{
			const size_t index = ((event.channel % 16) * 128) + (event.key % 128);
			if (event.type == graph::ProcessEvent::Type::NoteOn)
				m_held[index] = true;
			else if (event.type == graph::ProcessEvent::Type::NoteOff)
				m_held[index] = false;
		}
		if (m_instrument)
			m_instrument->process(
				{context.audio, context.channels, context.frames, m_events, context.eventsOut, context.transport});
	}
}
