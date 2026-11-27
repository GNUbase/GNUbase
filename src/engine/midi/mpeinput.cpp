// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/mpeinput.h"

#include "engine/midi/midivalues.h"
#include "platform/text.h"
#include "plugin/events.h"

#include <algorithm>
#include <string>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr std::array<const char*, 5> g_modes = {"off", "lower", "upper", "rotation", "both"};
		constexpr std::array<const char*, 4> g_initials = {"off", "min", "center", "max"};
		// The dimensions' members in JSON, by MpeSetup::Dimension.
		constexpr std::array<const char*, 3> g_messageMembers = {"xMessage", "pressureMessage", "yMessage"};
		constexpr std::array<const char*, 3> g_expressionMembers = {"xExpression", "pressureExpression", "yExpression"};
		// A Note Expression Assignment's names: none, plugin::NoteExpression's, then MIDI PolyPressure (#1829).
		constexpr std::array<const char*, 9> g_assignments = {
			"none", "volume", "pan", "tuning", "vibrato", "expression", "brightness", "pressure", "polyPressure"};
		constexpr size_t g_polyPressure = 8; // its index in g_assignments

		// The known MPE input devices (p. 1295), which come from the factory with MPE's lower zone and its pitch range
		// of 48 semitones.
		MpeSetup lowerZone()
		{
			MpeSetup setup;
			setup.mode = MpeSetup::Mode::Lower;
			return setup;
		}

		const std::array<MpeDevice, 4>& devices()
		{
			static const std::array<MpeDevice, 4> known = {MpeDevice{"Seaboard", "seaboard", lowerZone()},
				MpeDevice{"LinnStrument", "linnstrument", lowerZone()},
				MpeDevice{"Sensel Morph", "sensel", lowerZone()}, MpeDevice{"Osmose", "osmose", lowerZone()}};
			return known;
		}

		// The index of the member's name in names, or fallback.
		template <size_t Size>
		size_t named(const Json& json, const char* member, const std::array<const char*, Size>& names, size_t fallback)
		{
			const auto found = json.find(member);
			if (found == json.end() || !found->is_string())
				return fallback;
			const auto name = found->get<std::string>();
			const auto at = std::find(names.begin(), names.end(), name);
			return at == names.end() ? fallback : static_cast<size_t>(at - names.begin());
		}

		int clampedInt(const Json& json, const char* member, int fallback, int low, int high)
		{
			return std::clamp(json.value(member, fallback), low, high);
		}

		uint8_t messageOf(const Json& json, const char* member, uint8_t fallback)
		{
			const auto found = json.find(member);
			if (found == json.end() || !found->is_string())
				return fallback;
			return messageNamed(found->get<std::string>()).value_or(fallback);
		}

		// A dimension's assignment's index in g_assignments, and back.
		size_t assignmentIndex(const MpeSetup& setup, size_t dimension)
		{
			if (setup.polyPressure.at(dimension))
				return g_polyPressure;
			const std::optional<plugin::NoteExpression> expression = setup.expressions.at(dimension);
			return expression.has_value() ? static_cast<size_t>(*expression) + 1 : 0;
		}

		void assign(MpeSetup& setup, size_t dimension, size_t index)
		{
			setup.polyPressure.at(dimension) = index == g_polyPressure;
			setup.expressions.at(dimension) = index == 0 || index >= g_polyPressure
				? std::nullopt
				: std::optional(static_cast<plugin::NoteExpression>(index - 1));
		}

		bool addEvent(graph::EventBuffer& out, const graph::ProcessEvent& event) noexcept
		{
			return graph::endsNotes(event) ? out.addEnd(event) : out.add(event);
		}

		// The value a dimension starts each note at with Send Initial Value, as its message would carry it.
		uint32_t initialValue(uint8_t message, MpeSetup::Initial initial) noexcept
		{
			const bool bend = message == MpeSetup::g_pitchBend;
			if (initial == MpeSetup::Initial::Center)
				return bend ? pitchBendFromMidi1(8192) : controllerFromMidi1(64);
			if (initial == MpeSetup::Initial::Max)
				return bend ? pitchBendFromMidi1(16383) : controllerFromMidi1(127);
			return 0;
		}

		// Whether the event is the message (MpeSetup::messages).
		bool isMessage(uint8_t message, const graph::ProcessEvent& event) noexcept
		{
			using Type = graph::ProcessEvent::Type;
			if (message == MpeSetup::g_pitchBend)
				return event.type == Type::PitchBend;
			if (message == MpeSetup::g_aftertouch)
				return event.type == Type::ChannelPressure;
			return event.type == Type::Controller && event.key == message;
		}

		// A message's value as the expression's: tuning by the pitch range (a controller's center being no bend),
		// volume's linear gain 0-4, the others 0-1.
		double expressionValue(plugin::NoteExpression expression, uint32_t value, int pitchRange) noexcept
		{
			if (expression == plugin::NoteExpression::Tuning)
				return bendToSemitones(value, pitchRange);
			const double unit = controllerToUnit(value);
			return expression == plugin::NoteExpression::Volume ? unit * 4.0 : unit;
		}
	}

	std::string messageName(uint8_t message)
	{
		if (message == MpeSetup::g_pitchBend)
			return "pitchBend";
		if (message == MpeSetup::g_aftertouch)
			return "aftertouch";
		return "cc" + std::to_string(message);
	}

	std::optional<uint8_t> messageNamed(const std::string& name)
	{
		if (name == "pitchBend")
			return MpeSetup::g_pitchBend;
		if (name == "aftertouch")
			return MpeSetup::g_aftertouch;
		if (name.size() < 3 || name.size() > 5 || name.compare(0, 2, "cc") != 0 ||
			!std::all_of(name.begin() + 2, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
			return std::nullopt;
		const int number = std::stoi(name.substr(2));
		if (number > 127)
			return std::nullopt;
		return static_cast<uint8_t>(number);
	}

	MpeSetup MpeSetup::fromJson(const Json& json, const MpeSetup& defaults)
	{
		MpeSetup setup = defaults;
		setup.mode = static_cast<Mode>(named(json, "mode", g_modes, static_cast<size_t>(defaults.mode)));
		// With both zones, the upper one keeps its member channels and the lower one gets at most the rest of 14, as
		// MPE shrinks the zone set up first when the other one overlaps it.
		setup.upperChannels = clampedInt(json, "upperChannels", setup.upperChannels, 1, 13);
		int most = 16;
		if (setup.mode == Mode::Both)
			most = 14 - setup.upperChannels;
		else if (setup.mode == Mode::Lower || setup.mode == Mode::Upper)
			most = 15;
		setup.channels = clampedInt(json, "channels", setup.channels, 1, most);
		setup.firstChannel = clampedInt(json, "firstChannel", setup.firstChannel, 1, 16);
		setup.pitchRange = clampedInt(json, "pitchRange", setup.pitchRange, 1, 96);
		setup.tuning = json.value("tuning", setup.tuning);
		setup.initialX =
			static_cast<Initial>(named(json, "initialX", g_initials, static_cast<size_t>(defaults.initialX)));
		if (setup.initialX != Initial::Center)
			setup.initialX = Initial::Off; // X starts absolute or centered
		setup.initialY =
			static_cast<Initial>(named(json, "initialY", g_initials, static_cast<size_t>(defaults.initialY)));
		for (size_t i = 0; i < setup.messages.size(); ++i)
		{
			setup.messages.at(i) = messageOf(json, g_messageMembers.at(i), defaults.messages.at(i));
			assign(setup, i, named(json, g_expressionMembers.at(i), g_assignments, assignmentIndex(defaults, i)));
		}
		if (setup.expressions.at(X) == plugin::NoteExpression::Tuning)
			setup.expressions.at(X) = std::nullopt;
		return setup;
	}

	Json MpeSetup::toJson() const
	{
		Json json = {{"mode", g_modes.at(static_cast<size_t>(mode))}, {"channels", channels},
			{"upperChannels", upperChannels}, {"firstChannel", firstChannel}, {"pitchRange", pitchRange},
			{"tuning", tuning}, {"initialX", g_initials.at(static_cast<size_t>(initialX))},
			{"initialY", g_initials.at(static_cast<size_t>(initialY))}};
		for (size_t i = 0; i < messages.size(); ++i)
		{
			json[g_messageMembers.at(i)] = messageName(messages.at(i));
			json[g_expressionMembers.at(i)] = g_assignments.at(assignmentIndex(*this, i));
		}
		return json;
	}

	uint64_t MpeSetup::pack() const
	{
		uint64_t packed = static_cast<uint64_t>(mode) | (static_cast<uint64_t>(channels - 1) << 3U) |
			(static_cast<uint64_t>(firstChannel - 1) << 7U) | (static_cast<uint64_t>(pitchRange) << 11U) |
			(static_cast<uint64_t>(tuning) << 18U) | (static_cast<uint64_t>(initialX) << 19U) |
			(static_cast<uint64_t>(initialY) << 21U) | (static_cast<uint64_t>(upperChannels - 1) << 59U);
		// From bit 23 on, per dimension its message's 8 bits and its assignment's 4; the upper zone's channels from
		// bit 59.
		for (size_t i = 0; i < messages.size(); ++i)
			packed |= (static_cast<uint64_t>(messages.at(i)) | (uint64_t{assignmentIndex(*this, i)} << 8U))
				<< (23U + (12U * i));
		return packed;
	}

	MpeSetup MpeSetup::unpack(uint64_t packed)
	{
		MpeSetup setup;
		setup.mode = static_cast<Mode>(packed & 7U);
		setup.channels = static_cast<int>((packed >> 3U) & 15U) + 1;
		setup.firstChannel = static_cast<int>((packed >> 7U) & 15U) + 1;
		setup.pitchRange = static_cast<int>((packed >> 11U) & 127U);
		setup.tuning = ((packed >> 18U) & 1U) != 0;
		setup.initialX = static_cast<Initial>((packed >> 19U) & 3U);
		setup.initialY = static_cast<Initial>((packed >> 21U) & 3U);
		setup.upperChannels = static_cast<int>((packed >> 59U) & 15U) + 1;
		for (size_t i = 0; i < setup.messages.size(); ++i)
		{
			const uint64_t dimension = packed >> (23U + (12U * i));
			setup.messages.at(i) = static_cast<uint8_t>(dimension & 255U);
			assign(setup, i, (dimension >> 8U) & 15U);
		}
		return setup;
	}

	bool MpeSetup::notesChannel(uint8_t channel) const
	{
		const int number = channel + 1; // 1-16
		const int members = std::min(channels, 15);
		switch (mode)
		{
		case Mode::Lower:
			return number >= 2 && number <= 1 + members;
		case Mode::Upper:
			return number <= 15 && number >= 16 - members;
		case Mode::Rotation:
			return number >= firstChannel && number < firstChannel + channels;
		case Mode::Both:
			return (number >= 2 && number <= 1 + channels) || (number <= 15 && number >= 16 - upperChannels);
		case Mode::Off:
			break;
		}
		return false;
	}

	std::optional<plugin::NoteExpression> MpeSetup::expression(size_t dimension) const
	{
		const std::optional<plugin::NoteExpression> assigned = expressions.at(dimension);
		if (dimension != X)
			return assigned;
		// Use for Tuning sets X's assignment to tuning (p. 1297) over X's own, which is never tuning.
		if (tuning)
			return plugin::NoteExpression::Tuning;
		return assigned == plugin::NoteExpression::Tuning ? std::nullopt : assigned;
	}

	const MpeDevice* detectMpeDevice(const std::string& portName)
	{
		const std::string lower = text::asciiLower(portName);
		for (const MpeDevice& device : devices())
			if (lower.find(device.match) != std::string::npos)
				return &device;
		return nullptr;
	}

	Json mpeSchema()
	{
		Json schema = Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
			"mode": {"type": "string", "enum": ["off", "lower", "upper", "rotation", "both"]},
			"channels": {"type": "integer", "minimum": 1, "maximum": 16},
			"upperChannels": {"type": "integer", "minimum": 1, "maximum": 13},
			"firstChannel": {"type": "integer", "minimum": 1, "maximum": 16},
			"pitchRange": {"type": "integer", "minimum": 1, "maximum": 96},
			"tuning": {"type": "boolean"},
			"initialX": {"type": "string", "enum": ["off", "center"]},
			"initialY": {"type": "string", "enum": ["off", "min", "center", "max"]}}})");
		const Json message = {{"type", "string"},
			{"pattern", "^(pitchBend|aftertouch|cc(12[0-7]|1[01][0-9]|[1-9]?[0-9]))$"}, {"maxLength", 10}};
		const Json assignment = {{"type", "string"}, {"enum", g_assignments}};
		for (size_t i = 0; i < g_messageMembers.size(); ++i)
		{
			schema["properties"][g_messageMembers.at(i)] = message;
			schema["properties"][g_expressionMembers.at(i)] = assignment;
		}
		return schema;
	}

	size_t MpeInput::maxEvents(const graph::ProcessEvent& event) const noexcept
	{
		const Channel& state = m_channels.at(event.channel % 16U);
		// The event itself, and per dimension a point for each note the channel's expression reaches, a new one too.
		return 1 + (3 * ((state.sounding | state.released).count() + 1));
	}

	void MpeInput::process(const MpeSetup& setup, const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept
	{
		using Type = graph::ProcessEvent::Type;
		const uint8_t channel = event.channel % 16U;
		if (!setup.notesChannel(channel))
		{
			addEvent(out, event);
			return;
		}
		Channel& state = m_channels.at(channel);
		// Whether the dimension plays something: an expression or poly pressure.
		const auto plays = [&setup](size_t dimension)
		{ return setup.expression(dimension).has_value() || setup.playsPolyPressure(dimension); };
		// A point of the dimension's expression, which it has, for the note, or the note's poly pressure.
		const auto expression = [&event, &setup, &state](uint8_t key, size_t dimension)
		{
			graph::ProcessEvent point;
			point.offset = event.offset;
			point.channel = event.channel;
			point.key = key;
			if (setup.playsPolyPressure(dimension))
			{
				point.type = Type::PolyPressure;
				point.value = state.values.at(dimension);
				return point;
			}
			const plugin::NoteExpression type = setup.expression(dimension).value_or(plugin::NoteExpression::Pressure);
			point.type = Type::NoteExpression;
			point.parameter = plugin::expressionId(type);
			point.parameterValue = expressionValue(type, state.values.at(dimension), setup.pitchRange);
			return point;
		};
		// The notes the channel's expression applies to: those that sound and those that fade.
		const auto toNotes = [&](size_t dimension)
		{
			const std::bitset<128> notes = state.sounding | state.released;
			for (size_t key = 0; key < notes.size(); ++key)
				if (notes.test(key))
					out.add(expression(static_cast<uint8_t>(key), dimension));
		};
		if (event.type == Type::NoteOn)
		{
			// The device gives the channel to a new note: as far as it is concerned, the released ones have faded.
			state.released.reset();
			state.sounding.set(event.key % 128U);
			addEvent(out, event);
			// Its initial values follow it, as playback sends them. Pressure has no Send Initial Value.
			const std::array<MpeSetup::Initial, 3> initials = {setup.initialX, MpeSetup::Initial::Off, setup.initialY};
			for (size_t i = 0; i < state.values.size(); ++i)
			{
				if (!plays(i))
					continue;
				const MpeSetup::Initial initial = initials.at(i);
				if (initial != MpeSetup::Initial::Off)
				{
					// The channel's value, so its other notes get it too.
					state.values.at(i) = initialValue(setup.messages.at(i), initial);
					state.known.set(i);
					toNotes(i);
				}
				else if (state.known.test(i))
				{
					out.add(expression(event.key, i));
				}
			}
			return;
		}
		if (event.type == Type::NoteOff && state.sounding.test(event.key % 128U))
		{
			state.sounding.reset(event.key % 128U);
			state.released.set(event.key % 128U);
		}
		// Each dimension of the message, two of the same one each; a message of none passes as it is.
		bool expressed = false;
		for (size_t i = 0; i < state.values.size(); ++i)
		{
			if (!plays(i) || !isMessage(setup.messages.at(i), event))
				continue;
			expressed = true;
			state.values.at(i) = event.value;
			state.known.set(i);
			toNotes(i);
		}
		if (!expressed)
			addEvent(out, event);
	}
}
