// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/mpeinput.h"

#include "engine/midi/midivalues.h"
#include "plugin/events.h"

#include <algorithm>
#include <string>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr std::array<const char*, 4> g_modes = {"off", "lower", "upper", "rotation"};
		constexpr std::array<const char*, 4> g_initials = {"off", "min", "center", "max"};
		// The expressions of pitch bend, channel pressure and CC 74, by MpeInput::Channel::values.
		constexpr std::array<plugin::NoteExpression, 3> g_expressions = {
			plugin::NoteExpression::Tuning, plugin::NoteExpression::Pressure, plugin::NoteExpression::Brightness};

		template <typename Enum, size_t Size>
		Enum named(const Json& json, const char* member, const std::array<const char*, Size>& names)
		{
			const auto name = json.value(member, std::string(names[0]));
			const auto found = std::find(names.begin(), names.end(), name);
			return static_cast<Enum>(found == names.end() ? 0 : found - names.begin());
		}

		int clampedInt(const Json& json, const char* member, int fallback, int low, int high)
		{
			return std::clamp(json.value(member, fallback), low, high);
		}

		bool addEvent(graph::EventBuffer& out, const graph::ProcessEvent& event) noexcept
		{
			return graph::endsNotes(event) ? out.addEnd(event) : out.add(event);
		}

		// The value an expression starts each note at with Send Initial Value: a centered bend for tuning.
		double initialValue(size_t index, MpeSetup::Initial initial)
		{
			if (index == 0 || initial == MpeSetup::Initial::Center)
				return index == 0 ? 0.0 : controllerToUnit(controllerFromMidi1(64));
			return initial == MpeSetup::Initial::Max ? 1.0 : 0.0;
		}

		// The index of the expression the event is on a notes' channel (MpeInput::Channel::values), or -1.
		int expressionIndex(const MpeSetup& setup, const graph::ProcessEvent& event)
		{
			using Type = graph::ProcessEvent::Type;
			if (event.type == Type::PitchBend && setup.tuning)
				return 0;
			if (event.type == Type::ChannelPressure)
				return 1;
			return event.type == Type::Controller && event.key == 74 ? 2 : -1;
		}
	}

	MpeSetup MpeSetup::fromJson(const Json& json)
	{
		MpeSetup setup;
		setup.mode = named<Mode>(json, "mode", g_modes);
		setup.channels = clampedInt(json, "channels", setup.channels, 1, 16);
		setup.firstChannel = clampedInt(json, "firstChannel", setup.firstChannel, 1, 16);
		setup.pitchRange = clampedInt(json, "pitchRange", setup.pitchRange, 1, 96);
		setup.tuning = json.value("tuning", setup.tuning);
		setup.initialX = named<Initial>(json, "initialX", g_initials);
		if (setup.initialX != Initial::Center)
			setup.initialX = Initial::Off; // pitch bend starts absolute or centered
		setup.initialY = named<Initial>(json, "initialY", g_initials);
		return setup;
	}

	Json MpeSetup::toJson() const
	{
		return {{"mode", g_modes.at(static_cast<size_t>(mode))}, {"channels", channels}, {"firstChannel", firstChannel},
			{"pitchRange", pitchRange}, {"tuning", tuning}, {"initialX", g_initials.at(static_cast<size_t>(initialX))},
			{"initialY", g_initials.at(static_cast<size_t>(initialY))}};
	}

	uint32_t MpeSetup::pack() const
	{
		return static_cast<uint32_t>(mode) | (static_cast<uint32_t>(channels - 1) << 2U) |
			(static_cast<uint32_t>(firstChannel - 1) << 6U) | (static_cast<uint32_t>(pitchRange) << 10U) |
			(static_cast<uint32_t>(tuning) << 17U) | (static_cast<uint32_t>(initialX) << 18U) |
			(static_cast<uint32_t>(initialY) << 20U);
	}

	MpeSetup MpeSetup::unpack(uint32_t packed)
	{
		MpeSetup setup;
		setup.mode = static_cast<Mode>(packed & 3U);
		setup.channels = static_cast<int>((packed >> 2U) & 15U) + 1;
		setup.firstChannel = static_cast<int>((packed >> 6U) & 15U) + 1;
		setup.pitchRange = static_cast<int>((packed >> 10U) & 127U);
		setup.tuning = ((packed >> 17U) & 1U) != 0;
		setup.initialX = static_cast<Initial>((packed >> 18U) & 3U);
		setup.initialY = static_cast<Initial>((packed >> 20U) & 3U);
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
		case Mode::Off:
			break;
		}
		return false;
	}

	Json mpeSchema()
	{
		return Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
			"mode": {"type": "string", "enum": ["off", "lower", "upper", "rotation"]},
			"channels": {"type": "integer", "minimum": 1, "maximum": 16},
			"firstChannel": {"type": "integer", "minimum": 1, "maximum": 16},
			"pitchRange": {"type": "integer", "minimum": 1, "maximum": 96},
			"tuning": {"type": "boolean"},
			"initialX": {"type": "string", "enum": ["off", "center"]},
			"initialY": {"type": "string", "enum": ["off", "min", "center", "max"]}}})");
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
		const auto expression = [&event](uint8_t key, size_t index, double value)
		{
			graph::ProcessEvent point;
			point.offset = event.offset;
			point.type = Type::NoteExpression;
			point.channel = event.channel;
			point.key = key;
			point.parameter = plugin::expressionId(g_expressions.at(index));
			point.parameterValue = value;
			return point;
		};
		if (event.type == Type::NoteOn)
		{
			state.sounding.set(event.key % 128U);
			addEvent(out, event);
			// Its initial values follow it, as playback sends them.
			for (size_t i = 0; i < state.values.size(); ++i)
			{
				if (i == 0 && !setup.tuning)
					continue;
				// Pressure has no Send Initial Value.
				const std::array<MpeSetup::Initial, 3> initials = {
					setup.initialX, MpeSetup::Initial::Off, setup.initialY};
				const MpeSetup::Initial initial = initials.at(i);
				if (initial != MpeSetup::Initial::Off)
				{
					state.values.at(i) = initialValue(i, initial);
					state.known.set(i);
				}
				if (state.known.test(i))
					out.add(expression(event.key, i, state.values.at(i)));
			}
			return;
		}
		if (event.type == Type::NoteOff)
			state.sounding.reset(event.key % 128U);
		const int index = expressionIndex(setup, event);
		if (index < 0)
		{
			addEvent(out, event);
			return;
		}
		const auto at = static_cast<size_t>(index);
		state.values.at(at) =
			index == 0 ? bendToSemitones(event.value, setup.pitchRange) : controllerToUnit(event.value);
		state.known.set(at);
		for (size_t key = 0; key < state.sounding.size(); ++key)
			if (state.sounding.test(key))
				out.add(expression(static_cast<uint8_t>(key), at, state.values.at(at)));
	}
}
