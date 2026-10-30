// SPDX-License-Identifier: GPL-3.0-or-later
// Sending MIDI into tracks and receiving what instruments played, for tests and remote control (#29): MidiTap and the
// commands midi.inject and midi.capture.
#include "engine/audio/miditap.h"

#include "engine/audio/audioengine.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "platform/midi/port.h"

#include <algorithm>
#include <array>
#include <string>

namespace gnubase::engine
{
	namespace
	{
		using Type = graph::ProcessEvent::Type;

		constexpr uint32_t g_noBank = 0xFFFFFFFFU;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// A MIDI 1.0 channel message as an event; a note-on of velocity 0 is a note-off of velocity 64.
		graph::ProcessEvent toEvent(const audio::MidiMessage& message, uint32_t offset) noexcept
		{
			const uint8_t data1 = message.bytes[1];
			const uint8_t data2 = message.bytes[2];
			graph::ProcessEvent event;
			event.offset = offset;
			event.channel = static_cast<uint8_t>(message.bytes[0] & 0x0FU);
			event.key = data1;
			switch (message.bytes[0] & 0xF0U)
			{
			case 0x80U:
				event.type = Type::NoteOff;
				event.velocity = midi::velocityFromMidi1(data2);
				break;
			case 0x90U:
				event.type = data2 == 0 ? Type::NoteOff : Type::NoteOn;
				event.velocity = midi::velocityFromMidi1(data2 == 0 ? 64 : data2);
				break;
			case 0xA0U:
				event.type = Type::PolyPressure;
				event.value = midi::controllerFromMidi1(data2);
				break;
			case 0xB0U:
				event.type = Type::Controller;
				event.value = midi::controllerFromMidi1(data2);
				break;
			case 0xC0U:
				event.type = Type::Program;
				event.value = g_noBank;
				break;
			case 0xD0U:
				event.type = Type::ChannelPressure;
				event.key = 0;
				event.value = midi::controllerFromMidi1(data1);
				break;
			default: // 0xE0
				event.type = Type::PitchBend;
				event.key = 0;
				event.value = midi::pitchBendFromMidi1(static_cast<uint16_t>(data1 | (uint32_t{data2} << 7U)));
				break;
			}
			return event;
		}

		// An event as MIDI 1.0 messages: none for SysEx and parameter changes, three for a program change with a bank
		// (bank select MSB and LSB, then the program change), one for the others.
		size_t toMidi1(const graph::ProcessEvent& event, std::array<audio::MidiMessage, 3>& out) noexcept
		{
			size_t count = 0;
			const auto add = [&](uint32_t status, uint32_t data1, int size = 2, uint32_t data2 = 0)
			{
				audio::MidiMessage& message = out[count++];
				message.size = static_cast<uint8_t>(size);
				message.bytes = {static_cast<uint8_t>(status | (event.channel & 0x0FU)),
					static_cast<uint8_t>(data1 & 0x7FU), static_cast<uint8_t>(data2 & 0x7FU)};
			};
			switch (event.type)
			{
			case Type::NoteOn:
				add(0x90U, event.key, 3, midi::noteOnVelocityToMidi1(event.velocity));
				break;
			case Type::NoteOff:
				add(0x80U, event.key, 3, midi::velocityToMidi1(event.velocity));
				break;
			case Type::Controller:
				add(0xB0U, event.key, 3, midi::controllerToMidi1(event.value));
				break;
			case Type::PitchBend:
			{
				const uint32_t bend = midi::pitchBendToMidi1(event.value);
				add(0xE0U, bend, 3, bend >> 7U);
				break;
			}
			case Type::ChannelPressure:
				add(0xD0U, midi::controllerToMidi1(event.value));
				break;
			case Type::PolyPressure:
				add(0xA0U, event.key, 3, midi::controllerToMidi1(event.value));
				break;
			case Type::Program:
				if (event.value <= g_maxBank)
				{
					add(0xB0U, 0, 3, event.value >> 7U); // bank select MSB
					add(0xB0U, 32, 3, event.value);      // and LSB
				}
				add(0xC0U, event.key);
				break;
			case Type::SysEx:
			case Type::Parameter:
				break;
			}
			return count;
		}
	}

	MidiTap::MidiTap() : m_waiting(g_size)
	{
	}

	bool MidiTap::inject(const std::vector<Message>& messages)
	{
		if (m_injected.space() < messages.size())
			return false;
		for (const Message& message : messages)
			m_injected.tryPush(message); // fits: the audio thread only makes room
		return true;
	}

	std::vector<MidiTap::Message> MidiTap::take(size_t& dropped)
	{
		std::vector<Message> result;
		Message message;
		while (m_captured.tryPop(message))
			result.push_back(message);
		dropped = m_dropped.exchange(0, std::memory_order_relaxed);
		return result;
	}

	void MidiTap::clear()
	{
		Message message;
		while (m_injected.tryPop(message))
		{
		}
		m_waitingCount = 0;
	}

	void MidiTap::deliver(uint64_t position, uint32_t frames, const Sequencers& sequencers) noexcept
	{
		for (const auto& entry : sequencers)
			entry.second->input().clear();
		while (m_waitingCount < m_waiting.size() && m_injected.tryPop(m_waiting[m_waitingCount]))
			++m_waitingCount;

		const uint64_t end = position + frames;
		size_t kept = 0;
		for (size_t i = 0; i < m_waitingCount; ++i)
		{
			const Message& message = m_waiting[i];
			bool wait = message.position >= end;
			if (!wait)
			{
				const auto found = std::lower_bound(sequencers.begin(), sequencers.end(), message.channel,
					[](const auto& entry, ObjectId channel) { return entry.first < channel; });
				const uint32_t offset =
					message.position > position ? static_cast<uint32_t>(message.position - position) : 0;
				wait = found != sequencers.end() && found->first == message.channel &&
					!found->second->input().add(toEvent(message.message, offset));
			}
			if (wait)
				m_waiting[kept++] = message;
		}
		m_waitingCount = kept;
	}

	void MidiTap::capture(uint64_t position, const Instruments& instruments) noexcept
	{
		std::array<audio::MidiMessage, 3> messages;
		for (const auto& [channel, instrument] : instruments)
			for (const graph::ProcessEvent& event : instrument->played())
			{
				const size_t count = toMidi1(event, messages);
				for (size_t i = 0; i < count; ++i)
					if (!m_captured.tryPush(Message{channel, position + event.offset, messages[i]}))
						m_dropped.fetch_add(1, std::memory_order_relaxed);
			}
	}

	void AudioEngine::registerMidiTapCommands(Engine& engine)
	{
		engine.registerCommand(
			{"midi.inject", 1,
				"Plays MIDI 1.0 channel messages into a MIDI or instrument track's channel as if they came from the "
				"track's MIDI input: the track sends them on to its instrument, before the events of its parts, also "
				"while the transport stands. bytes: a status byte from 0x80 to 0xEF and its data bytes. position: the "
				"message's frame on the audio device, as audio.capture and midi.monitor count them; messages for "
				"frames the device has processed already play at the start of its next block, those for one frame in "
				"the order they came. They wait until the device processes their block, at most 4096 at a time, and "
				"are dropped when the device changes; a call whose messages don't all fit fails and injects none. "
				"midi.capture returns what instruments played.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["channel", "messages"],
					"properties": {"channel": {"type": "integer"}, "messages": {"type": "array", "items": {
						"type": "object", "additionalProperties": false, "required": ["position", "bytes"],
						"properties": {"position": {"type": "integer", "minimum": 0}, "bytes": {"type": "array",
							"items": {"type": "integer", "minimum": 0, "maximum": 255}}}}}}})"),
				Json::object(), false},
			[this](CommandContext& context)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				const Json& parameters = context.parameters();
				const auto channel = parameters.at("channel").get<ObjectId>();
				require(m_sequencers.count(channel) != 0,
					"channel " + std::to_string(channel) + " is not a MIDI or instrument track's channel");
				const Json& messages = parameters.at("messages");
				require(messages.size() <= MidiTap::g_size, "at most 4096 messages at a time");
				std::vector<MidiTap::Message> injected;
				for (size_t i = 0; i < messages.size(); ++i)
				{
					const std::string where = "messages[" + std::to_string(i) + "]: ";
					const auto bytes = messages[i].at("bytes").get<std::vector<uint8_t>>();
					require(!bytes.empty() && bytes[0] >= 0x80 && bytes[0] < 0xF0,
						where + "not a MIDI 1.0 channel message, which starts with a status byte from 0x80 to 0xEF");
					const int size = gnubase::midi::messageSize(bytes[0]);
					require(bytes.size() == static_cast<size_t>(size),
						where + "a message with the status byte " + std::to_string(bytes[0]) + " has " +
							std::to_string(size) + " bytes");
					require(std::all_of(bytes.begin() + 1, bytes.end(), [](uint8_t data) { return data < 0x80; }),
						where + "data bytes must be below 0x80");
					MidiTap::Message message{channel, messages[i].at("position").get<uint64_t>(), {}};
					message.message.size = static_cast<uint8_t>(size);
					std::copy(bytes.begin(), bytes.end(), message.message.bytes.begin());
					injected.push_back(message);
				}
				require(m_tap.inject(injected),
					"too many MIDI messages wait for the audio device; none were injected, try again later");
				return Json::object();
			});
		engine.registerCommand(
			{"midi.capture", 1,
				"What instruments played since the last call, at most 4096 MIDI 1.0 messages, oldest first, each with "
				"its instrument track's channel, its frame position on the audio device and its bytes: the events of "
				"the track's parts, of the MIDI tracks routed to it and what midi.inject played into them, with "
				"controller automation and the note-offs of a stop. A program change with a bank comes as bank "
				"select MSB and LSB, then the program change; SysEx isn't captured. dropped counts the messages that "
				"didn't fit: while nobody reads, newer messages are dropped. start: true starts a fresh capture; it "
				"drops what was captured so far and returns nothing. Each call empties what it returns, so a repeated "
				"call doesn't return the same messages again.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"properties": {"start": {"type": "boolean"}}})"),
				Json::parse(R"({"type": "object", "properties": {"captured": {"type": "array"},
					"dropped": {"type": "integer"}}, "required": ["captured", "dropped"]})"),
				true},
			[this](CommandContext& context)
			{
				size_t dropped = 0;
				std::vector<MidiTap::Message> messages = m_tap.take(dropped);
				if (context.parameters().value("start", false))
				{
					messages.clear();
					dropped = 0;
				}
				Json captured = Json::array();
				for (const MidiTap::Message& message : messages)
					captured.push_back({{"channel", message.channel}, {"position", message.position},
						{"bytes",
							std::vector<uint8_t>(
								message.message.bytes.begin(), message.message.bytes.begin() + message.message.size)}});
				return Json{{"captured", std::move(captured)}, {"dropped", dropped}};
			});
	}
}
