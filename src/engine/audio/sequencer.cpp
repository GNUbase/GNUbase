// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sequencer.h"

#include "engine/graph/graph.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		// Live Input (#933): the pitch class set playing at position, the nearest pitch of it, down on a tie.
		int liveInputPitch(const std::vector<LiveHarmony>& harmonies, int64_t position, int pitch) noexcept
		{
			const auto after = std::upper_bound(harmonies.begin(), harmonies.end(), position,
				[](int64_t at, const LiveHarmony& harmony) { return at < harmony.start; });
			if (after == harmonies.begin() || std::prev(after)->classes == 0)
				return pitch;
			const uint16_t classes = std::prev(after)->classes;
			for (int distance = 0; distance < 12; ++distance)
				for (const int candidate : {pitch - distance, pitch + distance})
					if (candidate >= 0 && candidate <= 127 && ((classes >> (candidate % 12)) & 1U) != 0)
						return candidate;
			return pitch;
		}

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
			case ControllerType::Articulation:
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

		// Sorts controller events (by position) by kind and finds the runs of one kind.
		void index(ChaseIndex& index)
		{
			std::stable_sort(index.controllers.begin(), index.controllers.end(),
				[](const RenderController& a, const RenderController& b) { return chaseKind(a) < chaseKind(b); });
			index.lanes.clear();
			for (size_t i = 0; i < index.controllers.size(); ++i)
			{
				const auto kind = static_cast<uint16_t>(chaseKind(index.controllers[i]));
				if (index.lanes.empty() || index.lanes.back().kind != kind)
					index.lanes.push_back({kind, static_cast<uint32_t>(i), static_cast<uint32_t>(i)});
				index.lanes.back().last = static_cast<uint32_t>(i + 1);
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
			case ControllerType::Articulation: // never rendered: directions play through the slots' output events
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

	ChaseTrack makeChaseTrack(const std::vector<RenderController>& controllers, const std::vector<RenderPart>& parts)
	{
		ChaseTrack track;
		track.parts.resize(parts.size());
		for (size_t i = 0; i < parts.size(); ++i)
			track.parts[i].range = parts[i];
		for (const RenderController& controller : controllers)
		{
			if (chaseKind(controller) < 0) // SysEx among them, so no data is copied
				continue;
			track.all.controllers.push_back(controller);
			if (controller.part >= 0 && static_cast<size_t>(controller.part) < parts.size())
				track.parts[static_cast<size_t>(controller.part)].index.controllers.push_back(controller);
		}
		index(track.all);
		for (ChasePart& part : track.parts)
			index(part.index);
		return track;
	}

	MidiSequencer::MidiSequencer()
		: m_sequence(std::make_unique<const MidiSequence>()), m_input(graph::g_maxEventsPerBlock)
	{
		std::iota(m_inputTo.begin(), m_inputTo.end(), uint16_t{0});
	}

	void MidiSequencer::setSequence(std::unique_ptr<MidiSequence> sequence)
	{
		// Notes of no length don't play, so they don't take the place of an earlier note of their pitch either.
		indexByKind(
			sequence->notes, [](const RenderNote& note)
			{ return note.end > note.start ? ((note.channel % 16) * 128) + (note.pitch % 128) : -1; },
			sequence->noteOrder, sequence->noteLanes);

		// An effect that stops playing loses its state: one that plays again starts afresh, and so does one that
		// moves above or below the divider or to another output, whose notes end where they sounded.
		bool elsewhere = false;
		for (size_t index = 0; index < m_slots.size(); ++index)
		{
			const bool sends = index >= midi::g_effectSlots;
			const midi::EffectRack& rack = sends ? sequence->sends : sequence->inserts;
			const size_t number = index % midi::g_effectSlots;
			const midi::EffectSlot& slot = rack.slots[number];
			SlotEffect& kept = m_slots[index];
			if (rack.bypass || !slot.on || slot.effect.empty() || processors::findMidiEffect(slot.effect) == nullptr)
			{
				kept = SlotEffect();
				continue;
			}
			const uint64_t output = sends ? sequence->sendOutputs[number] : 0;
			const bool input = !sends && number < rack.divider;
			if (!kept.effect || kept.id != slot.effect || kept.output != output || kept.input != input)
			{
				kept.effect = processors::createMidiEffect(slot.effect);
				kept.id = slot.effect;
				kept.serial = ++m_effectSerial;
				kept.output = output;
				kept.input = input;
				if (m_sampleRate > 0.0)
					kept.effect->prepareFor(m_sampleRate, m_maxFrames);
			}
			sequence->effects.push_back({kept.effect, slot.parameters, kept.serial, number, output != 0, sends});
			elsewhere = elsewhere || output != 0;
			sequence->insertCount += sends ? 0 : 1;
			sequence->inputCount += input ? 1 : 0;
		}
		if (!sequence->effects.empty() && m_own.capacity() == 0)
		{
			m_own = graph::EventBuffer(graph::g_maxEventsPerBlock);
			m_chainA = graph::EventBuffer(graph::g_maxEventsPerBlock);
			m_chainB = graph::EventBuffer(graph::g_maxEventsPerBlock);
			m_effectEvents = graph::EventBuffer(graph::g_maxEventsPerBlock);
			m_effectIn = graph::EventBuffer(graph::g_maxEventsPerBlock);
		}
		// The buffers stay once they are there: the audio thread may use them as long as it has a sequence that
		// says so.
		if (elsewhere && m_sendOut[0].capacity() == 0)
			for (graph::EventBuffer& buffer : m_sendOut)
				buffer = graph::EventBuffer(graph::g_maxEventsPerBlock);
		sequence->sendBuffers = m_sendOut[0].capacity() != 0;

		// Sends before modifiers that change notes play what a sequencer without them plays (p. 1106).
		const midi::Modifiers& modifiers = sequence->modifiers;
		const bool preSends = sequence->sends.pre && sequence->insertCount < sequence->effects.size() &&
			!modifiers.bypass && !(modifiers == midi::Modifiers());
		if (!preSends)
		{
			m_pre = nullptr;
		}
		else
		{
			if (!m_pre)
			{
				m_pre = std::make_shared<MidiSequencer>();
				m_pre->setChase(m_chaseSetting);
				if (m_sampleRate > 0.0)
					m_pre->prepareFor(m_sampleRate, m_maxFrames);
			}
			if (m_preOut.capacity() == 0)
				m_preOut = graph::EventBuffer(graph::g_maxEventsPerBlock);
			auto unmodified = std::make_unique<MidiSequence>();
			unmodified->notes = sequence->preNotes;
			unmodified->controllers = sequence->controllers;
			unmodified->chase = sequence->chase;
			m_pre->setSequence(std::move(unmodified));
			sequence->pre = m_pre;
		}
		sequence->serial = ++m_sequenceSerial;
		m_sequence.publish(std::move(sequence));
	}

	void MidiSequencer::prepare(double sampleRate, uint32_t maxFrames)
	{
		m_sampleRate = sampleRate;
		m_maxFrames = maxFrames;
		for (const SlotEffect& slot : m_slots)
			if (slot.effect)
				slot.effect->prepareFor(sampleRate, maxFrames);
		if (m_pre)
			m_pre->prepareFor(sampleRate, maxFrames);
	}

	void MidiSequencer::setChase(const MidiChase& chase) noexcept
	{
		m_chaseSetting = chase;
		if (m_pre)
			m_pre->setChase(chase);
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
		// The parameter events of the effects' automation go to the effects, not on.
		if (!sequence.effects.empty())
			m_effectEvents.clear();
		for (const graph::ProcessEvent& event : context.eventsIn)
		{
			const bool effects = event.type == graph::ProcessEvent::Type::Parameter &&
				event.parameter >= midi::g_effectEvents && event.parameter < midi::g_effectEvents + (8 * 256);
			if (!effects)
				context.eventsOut.add(event);
			else if (!sequence.effects.empty())
				m_effectEvents.add(event);
		}
		context.eventsOut.nextSource();
		if (sequence.sendBuffers)
			for (graph::EventBuffer& buffer : m_sendOut)
				buffer.clear();
		if (sequence.serial != m_serial)
			takeEffects(sequence, context.eventsOut);
		if (sequence.effects.empty())
		{
			playOwn(sequence, context, context.eventsOut);
			return;
		}
		// The sends before the modifiers get what the track plays without them, its input as it came in.
		if (sequence.pre)
		{
			graph::EventBuffer& input = sequence.pre->input();
			input.clear();
			for (const graph::ProcessEvent& event : m_input)
				input.add(event);
			m_preOut.clear();
			sequence.pre->process({nullptr, 0, context.frames, m_none, m_preOut, context.transport});
		}
		m_own.clear();
		playOwn(sequence, context, m_own);
		runEffects(sequence, context, context.eventsOut);
	}

	void MidiSequencer::processInput(uint32_t frames, const graph::TransportBlock& transport) noexcept
	{
		const MidiSequence& sequence = m_sequence.acquire();
		if (sequence.inputCount == 0)
			return;
		takeParameters(sequence);
		const graph::EventBuffer* played = &m_input;
		for (size_t i = 0; i < sequence.inputCount; ++i)
		{
			graph::EventBuffer& to = played == &m_chainA ? m_chainB : m_chainA;
			runEffect(sequence.effects[i], *played, to, frames, transport, true);
			played = &to;
		}
		m_input.clear();
		for (const graph::ProcessEvent& event : *played)
			m_input.add(event);
	}

	void MidiSequencer::runEffect(const RunningEffect& running, const graph::EventBuffer& in, graph::EventBuffer& to,
		uint32_t frames, const graph::TransportBlock& transport, bool late) noexcept
	{
		const uint32_t first =
			midi::g_effectEvents + (static_cast<uint32_t>((running.send ? 4 : 0) + running.slot) * 256);
		const graph::EventBuffer* input = &in;
		for (const graph::ProcessEvent& event : m_effectEvents)
		{
			if (event.parameter < first || event.parameter >= first + 256)
				continue;
			if (input != &m_effectIn)
			{
				m_effectIn.clear();
				for (const graph::ProcessEvent& each : in)
					m_effectIn.add(each);
				m_effectIn.nextSource();
				input = &m_effectIn;
			}
			graph::ProcessEvent parameter = event;
			parameter.parameter -= first;
			// ponytail: the inserts above the divider play before the automation comes, so they get the block
			// before's at its start; a sequencer of their own in the graph would take it on time.
			if (late)
				parameter.offset = 0;
			m_effectIn.add(parameter);
		}
		to.clear();
		running.effect->process({nullptr, 0, frames, *input, to, transport});
	}

	void MidiSequencer::takeParameters(const MidiSequence& sequence) noexcept
	{
		if (sequence.serial == m_parameters)
			return;
		m_parameters = sequence.serial;
		for (const RunningEffect& running : sequence.effects)
			for (size_t id = 0; id < running.parameters.size(); ++id)
				running.effect->setParameter(static_cast<uint32_t>(id), running.parameters[id]);
	}

	void MidiSequencer::takeEffects(const MidiSequence& sequence, graph::EventBuffer& out) noexcept
	{
		m_serial = sequence.serial;
		takeParameters(sequence);
		const bool same = sequence.effects.size() == m_chainCount &&
			std::equal(sequence.effects.begin(), sequence.effects.end(), m_chain.begin(),
				[](const RunningEffect& running, uint64_t serial) { return running.serial == serial; });
		if (same)
			return;
		m_chainCount = std::min(sequence.effects.size(), m_chain.size());
		for (size_t i = 0; i < m_chainCount; ++i)
			m_chain[i] = sequence.effects[i].serial;
		// The notes stay where they are in m_sounding and m_inputHeld: their note-offs follow as usual.
		for (uint16_t index = 0; index < g_notes; ++index)
			if (m_effectsHeld[index] || sounds(index) || m_inputHeld[index])
				out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, index, 0));
		m_effectsHeld.fill(false);
		out.nextSource();
		// Wherever the sends play now: if not where they played, the instrument there ends them as they go.
		for (size_t slot = 0; slot < m_sendOut.size() && sequence.sendBuffers; ++slot)
			for (uint16_t index = 0; index < g_notes; ++index)
				if (m_sendHeld[slot][index])
					m_sendOut[slot].add(noteEvent(graph::ProcessEvent::Type::NoteOff, index, 0));
		for (std::array<bool, g_notes>& held : m_sendHeld)
			held.fill(false);
	}

	void MidiSequencer::runEffects(
		const MidiSequence& sequence, const graph::ProcessContext& context, graph::EventBuffer& out) noexcept
	{
		const auto run = [this, &context](
							 const RunningEffect& running, const graph::EventBuffer& in, graph::EventBuffer& to)
		{ runEffect(running, in, to, context.frames, context.transport, false); };
		const auto play = [this, &out](const graph::EventBuffer& events)
		{
			for (const graph::ProcessEvent& event : events)
				if (event.type == graph::ProcessEvent::Type::NoteOn || event.type == graph::ProcessEvent::Type::NoteOff)
					m_effectsHeld[((event.channel % 16) * 128) + (event.key % 128)] =
						event.type == graph::ProcessEvent::Type::NoteOn;
			out.merge(events);
		};
		graph::EventBuffer* played = &m_own;
		for (size_t i = sequence.inputCount; i < sequence.insertCount; ++i)
		{
			graph::EventBuffer& to = played == &m_chainA ? m_chainB : m_chainA;
			run(sequence.effects[i], *played, to);
			played = &to;
		}
		play(*played);
		if (sequence.insertCount == sequence.effects.size())
			return;
		// What they take is played already, so it can change.
		graph::EventBuffer* const unmodified = sequence.pre ? &m_preOut : &m_own;
		graph::EventBuffer& source = sequence.sends.pre ? *unmodified : *played;
		graph::EventBuffer& sent = played == &m_chainA ? m_chainB : m_chainA;
		// Sends to other instrument tracks get every event: the controllers, pitch bend and sustain too.
		for (size_t i = sequence.insertCount; i < sequence.effects.size(); ++i)
		{
			const RunningEffect& running = sequence.effects[i];
			if (!running.elsewhere)
				continue;
			run(running, source, sent);
			graph::EventBuffer& to = m_sendOut[running.slot];
			to.merge(sent);
			for (const graph::ProcessEvent& event : sent)
				if (event.type == graph::ProcessEvent::Type::NoteOn || event.type == graph::ProcessEvent::Type::NoteOff)
					m_sendHeld[running.slot][((event.channel % 16) * 128) + (event.key % 128)] =
						event.type == graph::ProcessEvent::Type::NoteOn;
		}
		// Those to the track's own output get the notes only, so the track's other events reach it once.
		source.removeIf(
			[](const graph::ProcessEvent& event)
			{
				return event.type != graph::ProcessEvent::Type::NoteOn &&
					event.type != graph::ProcessEvent::Type::NoteOff &&
					event.type != graph::ProcessEvent::Type::PolyPressure;
			});
		for (size_t i = sequence.insertCount; i < sequence.effects.size(); ++i)
		{
			if (sequence.effects[i].elsewhere)
				continue;
			run(sequence.effects[i], source, sent);
			play(sent);
		}
	}

	void MidiSequencer::playOwn(
		const MidiSequence& sequence, const graph::ProcessContext& context, graph::EventBuffer& out) noexcept
	{
		const graph::TransportBlock& transport = context.transport;
		if (!transport.playing && m_next >= 0) // a stop: the instrument ends what the input held
			m_inputHeld.fill(false);
		for (graph::ProcessEvent event : m_input)
		{
			auto ends = static_cast<uint16_t>(g_notes);
			const bool plays = modifyInput(sequence, transport.position + event.offset, event, ends);
			// Still held if the buffer was full: then a stop ends it.
			if (ends != g_notes && out.add(noteEvent(graph::ProcessEvent::Type::NoteOff, ends, event.offset)))
				m_inputHeld[ends] = false;
			if (!plays)
				continue;
			out.add(event);
			if (event.type != graph::ProcessEvent::Type::NoteOn && event.type != graph::ProcessEvent::Type::NoteOff)
				continue;
			const size_t index = ((event.channel % 16) * 128) + (event.key % 128);
			m_inputHeld[index] = event.type == graph::ProcessEvent::Type::NoteOn;
		}
		out.nextSource();

		const uint8_t chase = m_chase.load(std::memory_order_relaxed);
		if (!transport.playing)
		{
			endAll(0, out);
			m_chaseNotes = false;
			if (m_next >= 0)
				m_cursor = m_next; // where playback stopped
			m_next = -1;
			if (m_cursor >= 0 && transport.position != m_cursor) // a locate while stopped
				findChased(sequence, transport.position, chase, true);
			m_cursor = transport.position;
			sendChased(0, out);
			return;
		}
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			if (segment.position != m_next) // a start, a locate or a cycle jump
			{
				endAll(segment.offset, out);
				findChased(sequence, segment.position, chase, false);
				m_chaseNotes = (chase & g_chaseNotes) != 0;
			}
			play(sequence, segment, out);
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

	void MidiSequencer::findChased(const MidiSequence& sequence, int64_t position, uint8_t chase, bool stopped) noexcept
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
				consider(track.all, index, position, chase, stopped);
				continue;
			}
			for (const ChasePart& part : track.parts) // those under the position
			{
				if (part.range.start > position)
					break;
				if (part.range.end > position)
					consider(part.index, index, position, chase, stopped);
			}
		}

		// The kinds found, in time order; at one position by track, then kind, so a bank select precedes its program.
		for (size_t kind = 0; kind < m_chased.size(); ++kind)
		{
			if (m_chased[kind].played || m_chased[kind].position == std::numeric_limits<int64_t>::min())
			{
				m_chased[kind] = Chased();
				continue;
			}
			m_chased[m_chasedCount++] = m_chased[kind];
			if (m_chasedCount - 1 != kind)
				m_chased[kind] = Chased();
		}
		std::sort(m_chased.begin(), m_chased.begin() + static_cast<std::ptrdiff_t>(m_chasedCount),
			[](const Chased& a, const Chased& b)
			{ return std::tie(a.position, a.track, a.kind) < std::tie(b.position, b.track, b.kind); });
	}

	void MidiSequencer::consider(
		const ChaseIndex& index, uint16_t track, int64_t position, uint8_t chase, bool stopped) noexcept
	{
		for (const ChaseLane& lane : index.lanes)
		{
			if (!chases(lane.kind, chase))
				continue;
			const auto first = index.controllers.begin() + lane.first;
			const auto last = index.controllers.begin() + lane.last;
			// Playing, the events at the position play there, after the chase: their kind isn't chased. Stopped,
			// nothing plays them, so they are chased with the others.
			const auto found = stopped ? std::upper_bound(first, last, position,
											 [](int64_t at, const RenderController& c) { return at < c.position; })
									   : std::lower_bound(first, last, position,
											 [](const RenderController& c, int64_t at) { return c.position < at; });
			Chased& chased = m_chased[lane.kind];
			if (!stopped && found != last && found->position == position)
				chased.played = true;
			if (found == first)
				continue;
			const RenderController& controller = *std::prev(found);
			if (controller.position < chased.position) // at one position the later track wins
				continue;
			const bool played = chased.played;
			chased = {controller.position, played, controller.value, track, lane.kind, controller.type,
				controller.number, controller.channel};
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
		// Of each pitch the last note that started before the position, if it still sounds there or is a key switch
		// (#175), in their order: the instrument gets the articulation it would have had, then the notes. A key switch
		// that ended plays for a sample.
		size_t count = 0;
		for (const ChaseLane& lane : sequence.noteLanes)
		{
			const RenderNote* note = lastBefore(
				sequence.notes, sequence.noteOrder, lane, position, [](const RenderNote& n) { return n.start; });
			if (note != nullptr && (note->end > position || note->keySwitch))
				m_chasedNotes[count++] = static_cast<uint32_t>(note - sequence.notes.data());
		}
		std::sort(m_chasedNotes.begin(), m_chasedNotes.begin() + static_cast<std::ptrdiff_t>(count));
		for (size_t i = 0; i < count; ++i)
		{
			RenderNote note = sequence.notes[m_chasedNotes[i]];
			note.end = std::max(note.end, position + 1);
			const auto index = static_cast<uint16_t>(((note.channel % 16) * 128) + (note.pitch % 128));
			if (!sounds(index) && !noteOn(note, offset, out)) // sounds: chased in an earlier block
				return false;
		}
		return true;
	}

	bool MidiSequencer::modifyInput(
		const MidiSequence& sequence, int64_t position, graph::ProcessEvent& event, uint16_t& ends) noexcept
	{
		const midi::Modifiers& modifiers = sequence.modifiers;
		using Type = graph::ProcessEvent::Type;
		if (event.type != Type::NoteOn && event.type != Type::NoteOff && event.type != Type::PolyPressure)
			return true;
		const size_t from = ((event.channel % 16) * 128) + (event.key % 128);
		if (event.type == Type::NoteOn)
		{
			const uint16_t before = m_inputTo[from];
			auto pitch = static_cast<uint8_t>(liveInputPitch(sequence.liveInput, position, event.key % 128));
			m_inputTo[from] = midi::modifyNote(modifiers, midi::noteSeed(0, ++m_random), pitch, event.velocity)
				? static_cast<uint16_t>(((event.channel % 16) * 128) + pitch)
				: static_cast<uint16_t>(g_notes);
			// The key is held and its note goes elsewhere this time: the key's one note-off can end only one of them.
			if (m_inputDown[from] && before != g_notes && before != m_inputTo[from] && m_inputHeld[before])
				ends = before;
			m_inputDown[from] = true;
		}
		const uint16_t to = m_inputTo[from];
		if (event.type == Type::NoteOff)
		{
			m_inputTo[from] = static_cast<uint16_t>(from);
			m_inputDown[from] = false;
		}
		if (to == g_notes)
			return false;
		event.key = static_cast<uint8_t>(to % 128);
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
		std::shared_ptr<MidiSequencer> sequencer, std::shared_ptr<graph::Processor> instrument, uint64_t serial)
		: m_sequencer(std::move(sequencer)), m_instrument(std::move(instrument)), m_serial(serial),
		  m_events(graph::g_maxEventsPerBlock)
	{
	}

	void InstrumentChannel::prepare(double sampleRate, uint32_t maxFrames)
	{
		m_sequencer->prepareFor(sampleRate, maxFrames);
		if (m_instrument)
			m_instrument->prepareFor(sampleRate, maxFrames);
	}

	template <typename Departed> void InstrumentChannel::release(
		const Departed& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept
	{
		for (uint16_t index = 0; index < MidiSequencer::g_notes; ++index)
		{
			if (!m_held[index] || m_ending[index] || !departed.holds(index) || m_sequencer->holds(index) ||
				std::any_of(staying.begin(), staying.end(), [this, index](const std::shared_ptr<MidiSequencer>& source)
					{ return source->playsOn() == m_serial && source->holds(index); }))
				continue;
			m_ending[index] = true;
			++m_endingCount;
		}
	}

	void InstrumentChannel::releaseNotesOf(
		MidiSequencer& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept
	{
		release(departed, staying);
		departed.forget();
	}

	void InstrumentChannel::releaseNotesOf(
		MidiSendTap& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept
	{
		release(departed, staying);
		departed.forget();
	}

	MidiSendTap::MidiSendTap(std::shared_ptr<MidiSequencer> source, size_t slot)
		: m_source(std::move(source)), m_slot(slot)
	{
	}

	void MidiSendTap::process(const graph::ProcessContext& context) noexcept
	{
		for (const graph::ProcessEvent& event : m_source->sendOutput(m_slot))
		{
			if (event.type != graph::ProcessEvent::Type::NoteOff ? !context.eventsOut.add(event)
																 : !context.eventsOut.addNoteOff(event))
				continue;
			if (event.type == graph::ProcessEvent::Type::NoteOn || event.type == graph::ProcessEvent::Type::NoteOff)
				m_held[((event.channel % 16) * 128) + (event.key % 128)] =
					event.type == graph::ProcessEvent::Type::NoteOn;
		}
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
