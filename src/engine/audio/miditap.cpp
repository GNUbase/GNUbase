// SPDX-License-Identifier: GPL-3.0-or-later
// Sending MIDI into tracks and receiving what instruments played, for tests and remote control (#29): MidiTap and the
// commands midi.inject and midi.capture.
#include "engine/audio/miditap.h"

#include "engine/audio/audioengine.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "engine/settings/usersettings.h"
#include "platform/midi/port.h"
#include "plugin/events.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

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

		// A MIDI 1.0 channel message from a command's parameters; where names it in errors.
		audio::MidiMessage channelMessage(const Json& value, const std::string& where)
		{
			const auto bytes = value.get<std::vector<uint8_t>>();
			require(!bytes.empty() && bytes[0] >= 0x80 && bytes[0] < 0xF0,
				where + "not a MIDI 1.0 channel message, which starts with a status byte from 0x80 to 0xEF");
			const int size = gnubase::midi::messageSize(bytes[0]);
			require(bytes.size() == static_cast<size_t>(size),
				where + "a message with the status byte " + std::to_string(bytes[0]) + " has " + std::to_string(size) +
					" bytes");
			require(std::all_of(bytes.begin() + 1, bytes.end(), [](uint8_t data) { return data < 0x80; }),
				where + "data bytes must be below 0x80");
			audio::MidiMessage message;
			message.size = static_cast<uint8_t>(size);
			std::copy(bytes.begin(), bytes.end(), message.bytes.begin());
			return message;
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
			case Type::NoteExpression:
				// MPE on the note's channel (docs/features/note-expression.md); the other expressions have no MIDI
				// 1.0 message.
				if (event.parameter == plugin::expressionId(plugin::NoteExpression::Tuning))
				{
					const uint32_t bend =
						midi::pitchBendToMidi1(midi::semitonesToBend(event.parameterValue, midi::g_mpePitchRange));
					add(0xE0U, bend, 3, bend >> 7U);
				}
				else if (event.parameter == plugin::expressionId(plugin::NoteExpression::Pressure))
				{
					add(0xD0U, midi::controllerToMidi1(midi::controllerFromUnit(event.parameterValue)));
				}
				else if (event.parameter == plugin::expressionId(plugin::NoteExpression::Brightness))
				{
					add(0xB0U, 74, 3, midi::controllerToMidi1(midi::controllerFromUnit(event.parameterValue)));
				}
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

	void MidiTap::thru(
		const audio::MidiBuffer& input, const Sequencers& sequencers, const std::vector<uint8_t>& thru) noexcept
	{
		if (input.empty())
			return;
		for (size_t i = 0; i < sequencers.size() && i < thru.size(); ++i)
		{
			if (thru[i] == 0)
				continue;
			graph::EventBuffer& events = sequencers[i].second->input();
			events.nextSource();
			for (const audio::MidiMessage& message : input)
				if (message.size == gnubase::midi::messageSize(message.bytes[0]) && message.bytes[0] >= 0x80 &&
					message.bytes[0] < 0xF0)
					events.add(toEvent(message, message.offset));
		}
	}

	void MidiTap::capture(uint64_t position, const Instruments& instruments) noexcept
	{
		std::array<audio::MidiMessage, 3> messages;
		for (const auto& [channel, instrument] : instruments)
		{
			const auto keep = [&, channel = channel](const graph::ProcessEvent& event)
			{
				const size_t count = toMidi1(event, messages);
				for (size_t i = 0; i < count; ++i)
					if (!m_captured.tryPush(Message{channel, position + event.offset, messages[i]}))
						m_dropped.fetch_add(1, std::memory_order_relaxed);
			};
			// The expression of a note that starts at the same frame, which follows its note-on, as MPE before it:
			// its initial pitch bend, pressure and CC 74 (docs/features/note-expression.md).
			const graph::EventBuffer& played = instrument->played();
			const auto initial = [&played](size_t expression, size_t noteOn)
			{
				const graph::ProcessEvent& a = played[expression];
				const graph::ProcessEvent& b = played[noteOn];
				return a.type == graph::ProcessEvent::Type::NoteExpression &&
					b.type == graph::ProcessEvent::Type::NoteOn && a.offset == b.offset && a.key == b.key &&
					a.channel == b.channel;
			};
			for (size_t i = 0; i < played.size(); ++i)
			{
				bool sent = false; // before its note-on
				for (size_t j = i; j-- > 0 && played[j].offset == played[i].offset && !sent;)
					sent = initial(i, j);
				if (sent)
					continue;
				for (size_t j = i + 1; j < played.size() && played[j].offset == played[i].offset; ++j)
					if (initial(j, i))
						keep(played[j]);
				keep(played[i]);
			}
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
				injected.reserve(messages.size());
				for (size_t i = 0; i < messages.size(); ++i)
				{
					injected.push_back({channel, messages[i].at("position").get<uint64_t>(),
						channelMessage(messages[i].at("bytes"), "messages[" + std::to_string(i) + "]: ")});
				}
				require(m_tap.inject(injected),
					"too many MIDI messages wait for the audio device; none were injected, try again later");
				return Json::object();
			});
		engine.registerCommand(
			{"midi.keyboard", 1,
				"Plays MIDI 1.0 channel messages from the on-screen keyboard, one of all MIDI inputs: they arrive now, "
				"as a MIDI input port's do, and the audio device places them in its next block. The MIDI and "
				"instrument tracks set to all MIDI inputs play them while they monitor or are record-enabled (MIDI "
				"thru), and the record-enabled ones record them. bytes: a status byte from 0x80 to 0xEF and its data "
				"bytes. The UI's on-screen keyboard window plays its notes through this. With chordPads they are the "
				"chord pads' notes instead (chordPads.trigger plays through this), of player (0 to 7, the chord pads' "
				"player whose voicings they are): they play into the tracks whose "
				"MIDI input is the chord pads, and with Chord Pad Output Mode on (chordPads.set outputMode) into "
				"those of all MIDI inputs too. If the input waiting for the audio device has no room left, the call "
				"fails and says how many of its messages were played.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["messages"],
					"properties": {"chordPads": {"type": "boolean"}, "player": {"type": "integer", "minimum": 0, "maximum": 7},
					"messages": {"type": "array", "items": {"type": "object",
						"additionalProperties": false, "required": ["bytes"], "properties": {"bytes": {"type": "array",
							"items": {"type": "integer", "minimum": 0, "maximum": 255}}}}}}})"),
				Json::object(), false},
			[this](CommandContext& context)
			{
				const Json& messages = context.parameters().at("messages");
				std::vector<audio::MidiMessage> played;
				played.reserve(messages.size());
				for (size_t i = 0; i < messages.size(); ++i)
					played.push_back(channelMessage(messages[i].at("bytes"), "messages[" + std::to_string(i) + "]: "));
				const bool pads = context.parameters().value("chordPads", false);
				const auto player = context.parameters().value("player", size_t{0});
				for (size_t i = 0; i < played.size(); ++i)
					require(pads ? m_midi.playPads(played[i], player) : m_midi.play(played[i]),
						"too much MIDI input waits for the audio device; " + std::to_string(i) +
							" of the messages were played");
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

		// The channel of a MIDI or instrument track.
		const auto midiChannel = [this](const Json& track)
		{
			const auto id = track.get<ObjectId>();
			for (const RenderChannel& channel : m_snapshots.latest().channels)
				if (channel.track == id && (channel.type == TrackType::Midi || channel.type == TrackType::Instrument))
					return channel.id;
			throw CommandError("track " + std::to_string(id) + " is not a MIDI or instrument track");
		};
		engine.registerCommand(
			{"midi.insertRetrospective", 1,
				"Retrospective recording (reference manual pp. 353-355): inserts what the MIDI or instrument track's "
				"MIDI input played while it didn't record, which its retrospective buffer kept (at most the user "
				"setting midi.retrospectiveEvents, by default 10000 events), as one MIDI part, through recording.add "
				"with the new parts mode and the user settings midi.snapPartsToBars and midi.recordFilter. What "
				"came during playback goes where it played; what came in stop mode goes at the project cursor, with "
				"its timing. Input after a pause of over 30 s in stop mode starts the buffer afresh, and so does "
				"input after an insert. Returns what recording.add returns.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["track"],
					"properties": {"track": {"type": "integer"}}})"),
				Json::parse(R"({"type": "object", "properties": {"parts": {"type": "array"}},
					"required": ["parts"]})"),
				false},
			[this, midiChannel](CommandContext& context)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				const Json& track = context.parameters().at("track");
				const std::vector<RetrospectiveEvent> events = m_recorder.retrospective(midiChannel(track));
				const MidiRecordFilter filter = MidiRecordFilter::fromJson(m_settings.value("midi.recordFilter"));
				const double ratio = m_snapshots.latest().sampleRate / m_device->format().sampleRate;
				const time::Samples cursor = m_transport.position();
				MidiTakeBuilder builder;
				builder.setMpe(mpePitchRange(m_settings));
				time::Samples start = -1;
				int64_t stopped = -1; // the device frame of the first input in stop mode
				for (const RetrospectiveEvent& kept : events)
				{
					time::Samples at = kept.position;
					if (at < 0)
					{
						if (stopped < 0)
							stopped = static_cast<int64_t>(kept.deviceFrame);
						at = cursor +
							std::llround(static_cast<double>(static_cast<int64_t>(kept.deviceFrame) - stopped) * ratio);
					}
					if (!filter.records(kept.event))
						continue;
					start = start < 0 ? at : std::min(start, at);
					builder.add(at, kept.event);
				}
				require(!builder.empty(), "the track's retrospective buffer holds nothing to insert");
				builder.endNotes(builder.last() + 1);
				const Json take = {{"position", start}, {"length", builder.last() - start + 1}};
				return context.call("recording.add",
					{{"midiMode", "newParts"}, {"snapToBars", m_settings.get(settings::g_midiSnapPartsToBars)},
						{"tracks",
							Json::array({{{"track", track}, {"takes", Json::array({take})}, {"notes", builder.notes()},
								{"controllers", builder.controllers()}}})}});
			});
		engine.registerCommand(
			{"midi.clearRetrospective", 1,
				"Empties the retrospective buffer of a MIDI or instrument track, or of every track without track.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"properties": {"track": {"type": "integer"}}})"),
				Json::object(), false},
			[this, midiChannel](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				m_recorder.clearRetrospective(parameters.contains("track") ? midiChannel(parameters.at("track")) : 0);
				return Json::object();
			});
	}
}
