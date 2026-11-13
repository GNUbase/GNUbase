// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sequencer.h"

#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"

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

		// How the track is heard again in this block (ChaseTrack::heardAgain).
		HeardAgain heard(const ChaseTrack& track) noexcept
		{
			return track.heardAgain ? track.heardAgain->load(std::memory_order_relaxed) : HeardAgain::No;
		}

		bool raised(const ChaseTrack& track) noexcept
		{
			return heard(track) != HeardAgain::No;
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

	std::vector<uint16_t> channelChaseKinds(const RenderChannel& channel)
	{
		std::vector<RenderController> sent;
		const auto add = [&sent](ControllerType type, uint8_t number, uint8_t midiChannel)
		{
			RenderController controller;
			controller.type = type;
			controller.number = number;
			controller.channel = midiChannel;
			sent.push_back(controller);
		};
		// The channel's own values go out on MIDI channel 1 (channelAutomation).
		if (channel.program)
			add(ControllerType::Program, 0, 0);
		if (channel.program && channel.bank)
		{
			add(ControllerType::Control, 0, 0);
			add(ControllerType::Control, 32, 0);
		}
		if (channel.midiVolume)
			add(ControllerType::Control, 7, 0);
		if (channel.midiPan)
			add(ControllerType::Control, 10, 0);
		for (const RenderAutomationLane& lane : channel.automation)
			if (lane.target.type == AutomationTargetType::Controller)
				add(ControllerType::Control, lane.target.number, lane.target.channel);
			else if (lane.target.type == AutomationTargetType::PitchBend)
				add(ControllerType::PitchBend, 0, lane.target.channel);
		std::vector<uint16_t> kinds;
		kinds.reserve(sent.size());
		for (const RenderController& controller : sent)
			kinds.push_back(static_cast<uint16_t>(chaseKind(controller)));
		return kinds;
	}

	graph::ProcessEvent chaseKindEvent(uint16_t kind) noexcept
	{
		graph::ProcessEvent event;
		if (kind < MidiSequencer::g_notes)
		{
			event.type = graph::ProcessEvent::Type::Controller;
			event.channel = static_cast<uint8_t>(kind / 128);
			event.key = static_cast<uint8_t>(kind % 128);
			return event;
		}
		const size_t after = kind - MidiSequencer::g_notes;
		event.type = after < 16 ? graph::ProcessEvent::Type::Program : graph::ProcessEvent::Type::PitchBend;
		event.channel = static_cast<uint8_t>(after % 16);
		return event;
	}

	MidiSequencer::MidiSequencer()
		: m_sequence(std::make_unique<const MidiSequence>()), m_input(graph::g_maxEventsPerBlock),
		  m_stripChase(std::make_shared<graph::EventBuffer>(graph::g_maxEventsPerBlock))
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
		// moves above or below the divider or to another output, or a send whose events come from elsewhere (the
		// inserts', the track's own or the sequencer's without the modifiers), whose notes end where they sounded.
		const midi::Modifiers& modifiers = sequence->modifiers;
		const bool modified = !modifiers.bypass && !(modifiers == midi::Modifiers());
		// 0: the inserts', 1: the track's own, 2: the sequencer's without the modifiers.
		const auto sendSource = static_cast<uint8_t>(int{sequence->sends.pre} + int{sequence->sends.pre && modified});
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
			const uint8_t source = sends ? sendSource : 0;
			if (!kept.effect || kept.id != slot.effect || kept.output != output || kept.input != input ||
				kept.source != source)
			{
				kept.effect = processors::createMidiEffect(slot.effect);
				kept.id = slot.effect;
				kept.serial = ++m_effectSerial;
				kept.output = output;
				kept.input = input;
				kept.source = source;
				if (m_sampleRate > 0.0)
					kept.effect->prepareFor(m_sampleRate, m_maxFrames);
				// Not on the audio thread yet. Later the lanes of its automation carry the slot's values, which they
				// send again only when they change (#992).
				for (size_t id = 0; id < slot.parameters.size(); ++id)
					kept.effect->setParameter(static_cast<uint32_t>(id), slot.parameters[id]);
			}
			sequence->effects.push_back({kept.effect, kept.serial, number, output != 0, sends});
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
		// says so. A MidiSendTap reads them for every send with an output, also one that doesn't play.
		const bool elsewhere = std::any_of(
			sequence->sendOutputs.begin(), sequence->sendOutputs.end(), [](uint64_t output) { return output != 0; });
		if (elsewhere && m_sendOut[0].capacity() == 0)
			for (graph::EventBuffer& buffer : m_sendOut)
				buffer = graph::EventBuffer(graph::g_maxEventsPerBlock);
		sequence->sendBuffers = m_sendOut[0].capacity() != 0;

		// Sends before modifiers that change notes play what a sequencer without them plays (p. 1106).
		const bool preSends = sequence->sends.pre && sequence->insertCount < sequence->effects.size() && modified;
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
		if (sequence->articulations.id != m_mapId || sequence->articulations.slots != m_mapSlots)
		{
			m_mapId = sequence->articulations.id;
			m_mapSlots = sequence->articulations.slots;
			++m_slotsSerial;
		}
		sequence->slotsSerial = m_slotsSerial;
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

	bool MidiSequencer::chasesKindOf(const graph::ProcessEvent& event) const noexcept
	{
		const uint8_t chase = m_chase.load(std::memory_order_relaxed);
		switch (event.type)
		{
		case graph::ProcessEvent::Type::Controller:
			return (chase & g_chaseControllers) != 0;
		case graph::ProcessEvent::Type::Program:
			return (chase & g_chasePrograms) != 0;
		case graph::ProcessEvent::Type::PitchBend:
			return (chase & g_chasePitchBend) != 0;
		case graph::ProcessEvent::Type::NoteOn:
		case graph::ProcessEvent::Type::NoteOff:
		case graph::ProcessEvent::Type::PolyPressure:
		case graph::ProcessEvent::Type::ChannelPressure:
		case graph::ProcessEvent::Type::SysEx:
		case graph::ProcessEvent::Type::Parameter:
			return false;
		}
		return false;
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

	void MidiSequencer::takeEffects(const MidiSequence& sequence, graph::EventBuffer& out) noexcept
	{
		m_serial = sequence.serial;
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
		// Another map, or other slots: the slot selected before isn't the one of that index now, whose on events
		// didn't go out.
		if (sequence.slotsSerial != m_inputSlotsSerial)
		{
			m_inputSlotsSerial = sequence.slotsSerial;
			m_inputSlot = -1;
			m_inputSlotHeld = -1;
		}
		for (graph::ProcessEvent event : m_input)
		{
			if (triggerSlot(sequence, event, out))
				continue;
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
		// A routed MIDI track's chase goes to its strip alone (MidiSequence::chaseIntoStrip), at the jump's offset,
		// or the rest of it that didn't fit at the start of the next block.
		m_stripChase->clear();
		const bool intoStrip = sequence.chaseIntoStrip;
		if (!transport.playing)
		{
			endAll(0, out);
			m_chaseNotes = false;
			if (m_next >= 0)
				m_cursor = m_next; // where playback stopped
			m_next = -1;
			if (m_cursor >= 0 && transport.position != m_cursor) // a locate while stopped
				findChased(sequence, transport.position, chase, true);
			else
				chaseHeardAgain(sequence, transport.position, chase, true);
			m_cursor = transport.position;
			sendChased(0, intoStrip ? *m_stripChase : out);
			return;
		}
		if (intoStrip)
			sendChased(0, *m_stripChase);
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			if (segment.position != m_next) // a start, a locate or a cycle jump
			{
				endAll(segment.offset, out);
				findChased(sequence, segment.position, chase, false);
				m_chaseNotes = (chase & g_chaseNotes) != 0;
				if (intoStrip)
					sendChased(segment.offset, *m_stripChase);
			}
			else if (s == 0)
			{
				chaseHeardAgain(sequence, segment.position, chase, false);
				if (intoStrip)
					sendChased(segment.offset, *m_stripChase);
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
			const bool chased = m_chasedSent < m_chasedCount && !sequence.chaseIntoStrip; // still to send here
			const int64_t chasing = chased || m_chaseNotes ? begin : g_never;
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
			else if (chased)
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

	void MidiSequencer::chaseHeardAgain(
		const MidiSequence& sequence, int64_t position, uint8_t chase, bool stopped) noexcept
	{
		if (std::any_of(sequence.chase.begin(), sequence.chase.end(), raised))
			findChased(sequence, position, chase, stopped, m_chasedSent == m_chasedCount);
	}

	void MidiSequencer::findChased(
		const MidiSequence& sequence, int64_t position, uint8_t chase, bool stopped, bool heardAgain) noexcept
	{
		for (size_t i = m_chasedSent; i < m_chasedCount; ++i) // the rest of a chase that didn't fit
			m_chased[i] = Chased();
		m_chasedCount = 0;
		m_chasedSent = 0;
		if ((chase & g_chaseControllerEvents) == 0)
			return;
		if (heardAgain)
		{
			m_resent.fill(false);
			for (const ChaseTrack& track : sequence.chase)
				if (raised(track))
					for (const uint16_t kind : track.channelKinds)
						m_resent[kind] = true;
		}
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
		// Per MIDI channel, the earlier bank select (CC 0 or CC 32) chased for tracks heard again, known by the time
		// its program comes: the controllers' kinds come before the programs'.
		std::array<const Chased*, 16> bank{};
		for (size_t kind = 0; kind < m_chased.size(); ++kind)
		{
			const Chased& found = m_chased[kind];
			// Heard again: the parts of a track that was soloed out or disabled, and the kinds the channels sent.
			const bool none = found.played || found.position == std::numeric_limits<int64_t>::min();
			bool skipped =
				!none && heardAgain && heard(sequence.chase[found.track]) != HeardAgain::Quiet && !m_resent[kind];
			const bool program = kind >= g_notes && kind < g_notes + 16;
			// Another track's program change on that MIDI channel after the bank select, or after the earlier of CC 0
			// and CC 32, follows it, as at a jump; the instrument would keep that program in the bank it had otherwise.
			// An earlier one a jump sends before the bank select, which changes nothing until the next program change:
			// it isn't sent again.
			if (const Chased* select = program ? bank[kind - g_notes] : nullptr; skipped && select != nullptr &&
				std::tie(found.position, found.track) > std::tie(select->position, select->track))
				skipped = false;
			if (none || skipped)
			{
				m_chased[kind] = Chased();
				continue;
			}
			const auto number = static_cast<uint8_t>(kind % 128);
			if (heardAgain && kind < g_notes && (number == 0 || number == 32))
			{
				const Chased*& select = bank[kind / 128];
				if (select == nullptr ||
					std::tie(found.position, found.track) < std::tie(select->position, select->track))
					select = &m_chased[m_chasedCount];
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
		// The key no longer holds the note it played (a second note-on, or its note-off).
		const auto letGo = [this, from]
		{
			const uint16_t to = m_inputTo[from];
			if (m_inputDown[from] && to != g_notes && m_inputKeys[to] > 0)
				--m_inputKeys[to];
		};
		if (event.type == Type::NoteOn)
		{
			const uint16_t before = m_inputTo[from];
			letGo();
			// A drum map plays the sound's O-note on its channel (MIDI thru, p. 1239), else Live Input moves it.
			int channel = event.channel % 16;
			bool plays = true;
			uint8_t pitch = 0;
			if (sequence.drumInput.size() == static_cast<size_t>(g_drumSounds))
			{
				const LiveDrumNote& drum = sequence.drumInput[event.key % 128];
				pitch = drum.key;
				channel = drum.channel >= 0 ? drum.channel : channel;
				plays = !drum.mute;
			}
			else
			{
				pitch = static_cast<uint8_t>(liveInputPitch(sequence.liveInput, position, event.key % 128));
			}
			// The sound slot its remote triggers selected, or the default one, plays it with its MIDI modifiers but
			// the length, before the track's (#175).
			const std::vector<SoundSlot>& slots = sequence.articulations.slots;
			if (!slots.empty())
			{
				const SoundSlot& slot =
					slots[m_inputSlot >= 0 && static_cast<size_t>(m_inputSlot) < slots.size() ? m_inputSlot : 0];
				pitch = static_cast<uint8_t>(std::clamp(pitch + slot.transpose, 0, 127));
				event.velocity = static_cast<uint16_t>(std::clamp<int64_t>(
					(int64_t{event.velocity} * slot.velocity) / 100, 1, std::numeric_limits<uint16_t>::max()));
				if (slot.channel >= 0)
					channel = slot.channel;
			}
			m_inputTo[from] = plays && midi::modifyNote(modifiers, midi::noteSeed(0, ++m_random), pitch, event.velocity)
				? static_cast<uint16_t>((channel * 128) + pitch)
				: static_cast<uint16_t>(g_notes);
			// The key is held and its note goes elsewhere this time: the key's one note-off can end only one of them,
			// so the first ends now, unless another held key plays it too.
			if (m_inputDown[from] && before != g_notes && before != m_inputTo[from] && m_inputHeld[before] &&
				m_inputKeys[before] == 0)
				ends = before;
			if (m_inputTo[from] != g_notes)
				++m_inputKeys[m_inputTo[from]];
			m_inputDown[from] = true;
		}
		const uint16_t to = m_inputTo[from];
		if (event.type == Type::NoteOff)
		{
			letGo();
			m_inputTo[from] = static_cast<uint16_t>(from);
			m_inputDown[from] = false;
			// Two held keys play the same note (Live Input moved one onto the other's): it ends with the last.
			if (to != g_notes && m_inputKeys[to] > 0)
				return false;
		}
		if (to == g_notes)
			return false;
		event.key = static_cast<uint8_t>(to % 128);
		event.channel = static_cast<uint8_t>(to / 128);
		return true;
	}

	bool MidiSequencer::triggerSlot(
		const MidiSequence& sequence, const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept
	{
		using Type = graph::ProcessEvent::Type;
		const ArticulationMap& map = sequence.articulations;
		const bool trigger = map.programTriggers ? event.type == Type::Program
												 : event.type == Type::NoteOn || event.type == Type::NoteOff;
		if (!trigger)
			return false;
		const auto found = std::find_if(
			map.slots.begin(), map.slots.end(), [&event](const SoundSlot& slot) { return slot.trigger == event.key; });
		if (found == map.slots.end())
			return false;
		const auto slot = static_cast<int>(found - map.slots.begin());
		// A key whose note-on played, before the map came or the key became a trigger: its note-off ends that note.
		if (event.type == Type::NoteOff && m_inputDown[((event.channel % 16) * 128) + (event.key % 128)])
			return false;
		if (event.type == Type::NoteOff)
		{
			// Without latch the default slot comes back where the key is released, unless another came since.
			if (slot == m_inputSlotHeld)
			{
				m_inputSlotHeld = -1;
				selectSlot(map, 0, event.channel, event.offset, out);
			}
			return true;
		}
		m_inputSlotHeld = event.type == Type::NoteOn && !map.latch ? slot : -1;
		selectSlot(map, slot, event.channel, event.offset, out);
		return true;
	}

	void MidiSequencer::selectSlot(
		const ArticulationMap& map, int slot, uint8_t channel, uint32_t offset, graph::EventBuffer& out) noexcept
	{
		if (slot == m_inputSlot)
			return;
		// As in playback (rendersnapshot.cpp): the off events on the channel the slot's on events went to, the on
		// events on the slot's channel or the trigger's. A key switch is a note-on and its note-off.
		const auto send = [&](const SoundSlot& from, bool off, uint8_t on)
		{
			for (const OutputEvent& output : from.outputs)
			{
				if (output.off != off)
					continue;
				if (output.status == OutputStatus::Note)
				{
					graph::ProcessEvent note = noteEvent(graph::ProcessEvent::Type::NoteOn,
						static_cast<uint16_t>((on * 128) + (output.data1 % 128)), offset);
					note.velocity = midi::velocityFromMidi1(output.data2);
					out.add(note);
					note.type = graph::ProcessEvent::Type::NoteOff;
					note.velocity = 0;
					out.add(note);
				}
				else if (output.status == OutputStatus::Controller)
				{
					out.add(controllerEvent(
						ControllerType::Control, on, output.data1, midi::controllerFromMidi1(output.data2), offset));
				}
				else
				{
					out.add(controllerEvent(ControllerType::Program, on, output.data1, g_noBank, offset));
				}
			}
		};
		if (m_inputSlot >= 0 && static_cast<size_t>(m_inputSlot) < map.slots.size())
			send(map.slots[static_cast<size_t>(m_inputSlot)], true, m_inputSlotChannel);
		const SoundSlot& next = map.slots[static_cast<size_t>(slot)];
		m_inputSlot = slot;
		m_inputSlotChannel =
			next.channel >= 0 ? static_cast<uint8_t>(next.channel) : static_cast<uint8_t>(channel % 16);
		send(next, false, m_inputSlotChannel);
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
																 : !context.eventsOut.addEnd(event))
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
