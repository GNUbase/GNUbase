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

	MidiSequencer::MidiSequencer()
		: m_sequence(std::make_unique<const MidiSequence>()), m_input(graph::g_maxEventsPerBlock)
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
		context.eventsOut.nextSource();
		for (const graph::ProcessEvent& event : m_input)
			context.eventsOut.add(event);
		context.eventsOut.nextSource();

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

		// Event by event in time order; at one sample note-offs, then controller events, then note-ons. When the
		// buffer is full, the rest of the segment is skipped.
		for (;;)
		{
			int64_t ending = g_never;
			for (size_t i = 0; i < m_soundingCount; ++i)
				ending = std::min(ending, std::max(m_ends[m_sounding[i]], begin));
			const int64_t starting = note != sequence.notes.end() ? note->start : g_never;
			const int64_t changing = controller != sequence.controllers.end() ? controller->position : g_never;
			const int64_t at = std::min({ending, starting, changing});
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
			else if (changing == at)
			{
				if (!out.add(controllerEvent(*controller, offset)))
					return;
				++controller;
			}
			else
			{
				if (note->end > note->start && !noteOn(*note, offset, out))
					return;
				++note;
			}
		}
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
