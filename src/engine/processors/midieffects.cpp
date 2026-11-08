// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/midieffects.h"

#include "engine/midi/midivalues.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace gnubase::engine::processors
{
	namespace
	{
		using Type = graph::ProcessEvent::Type;

		ParameterInfo ranged(uint32_t id, std::string name, double minimum, double maximum, double defaultValue,
			std::string unit = {}, bool stepped = true)
		{
			return {id, std::move(name), minimum, maximum, defaultValue, std::move(unit), stepped, {}};
		}

		ParameterInfo choice(uint32_t id, std::string name, std::vector<std::string> options)
		{
			const auto maximum = static_cast<double>(options.size() - 1);
			return {id, std::move(name), 0.0, maximum, 0.0, {}, true, std::move(options)};
		}

		uint16_t indexOf(const graph::ProcessEvent& event)
		{
			return static_cast<uint16_t>(((event.channel % 16) * 128) + (event.key % 128));
		}

		graph::ProcessEvent noteEvent(Type type, uint16_t index, uint16_t velocity, uint32_t offset)
		{
			graph::ProcessEvent event;
			event.offset = offset;
			event.type = type;
			event.channel = static_cast<uint8_t>(index / 128);
			event.key = static_cast<uint8_t>(index % 128);
			event.velocity = velocity;
			return event;
		}

		// The chords' intervals above the note, in semitones; 0xFF ends a chord.
		constexpr uint8_t g_end = 0xFF;
		constexpr std::array<std::array<uint8_t, 4>, 11> g_chords = {{
			{0, 4, 7, g_end},  // major
			{0, 3, 7, g_end},  // minor
			{0, 3, 6, g_end},  // diminished
			{0, 4, 8, g_end},  // augmented
			{0, 2, 7, g_end},  // suspended second
			{0, 5, 7, g_end},  // suspended fourth
			{0, 4, 7, 11},     // major seventh
			{0, 3, 7, 10},     // minor seventh
			{0, 4, 7, 10},     // dominant seventh
			{0, 7, g_end, 0},  // power chord
			{0, 12, g_end, 0}, // octave
		}};

		struct Effect
		{
			BuiltinInfo info;
			std::function<std::shared_ptr<MidiEffect>()> create;
		};

		const std::vector<Effect>& registry()
		{
			static const std::vector<Effect> effects = {
				{{"arpeggiator", "Arpeggiator",
					 "Plays the notes held one after another at a note value of the tempo, over one to four octaves.",
					 false,
					 {choice(Arpeggiator::Mode, "Mode", {"Up", "Down", "Up/Down", "As played"}),
						 []
						 {
							 ParameterInfo rate = choice(Arpeggiator::Rate, "Rate", {"1/4", "1/8", "1/16", "1/32"});
							 rate.defaultValue = 2.0;
							 return rate;
						 }(),
						 ranged(Arpeggiator::Gate, "Gate", 1.0, 100.0, 50.0, "%", false),
						 ranged(Arpeggiator::Octaves, "Octaves", 1.0, 4.0, 1.0)}},
					[] { return std::make_shared<Arpeggiator>(); }},
				{{"chordGenerator", "Chord generator", "Plays a chord for every note, the note its root.", false,
					 {choice(ChordGenerator::Chord, "Chord",
						 {"Major", "Minor", "Diminished", "Augmented", "Sus2", "Sus4", "Major 7", "Minor 7", "7",
							 "Power", "Octave"})}},
					[] { return std::make_shared<ChordGenerator>(); }},
				{{"transformer", "Transformer",
					 "Transposes, changes the velocity or channel of, or deletes the notes that meet a condition.",
					 false,
					 {choice(Transformer::Condition, "Condition", {"All notes", "Pitch", "Velocity", "Channel"}),
						 ranged(Transformer::Minimum, "Minimum", 0.0, 127.0, 0.0),
						 ranged(Transformer::Maximum, "Maximum", 0.0, 127.0, 127.0),
						 choice(Transformer::Action, "Action",
							 {"Transpose", "Set velocity", "Add velocity", "Set channel", "Delete"}),
						 ranged(Transformer::Value, "Value", -127.0, 127.0, 0.0)}},
					[] { return std::make_shared<Transformer>(); }},
			};
			return effects;
		}

	}

	const std::vector<BuiltinInfo>& midiEffects()
	{
		static const std::vector<BuiltinInfo> infos = []
		{
			std::vector<BuiltinInfo> result;
			for (const Effect& effect : registry())
				result.push_back(effect.info);
			return result;
		}();
		return infos;
	}

	const BuiltinInfo* findMidiEffect(const std::string& id)
	{
		const std::vector<BuiltinInfo>& infos = midiEffects();
		const auto found =
			std::find_if(infos.begin(), infos.end(), [&id](const BuiltinInfo& info) { return info.id == id; });
		return found != infos.end() ? &*found : nullptr;
	}

	std::string parameterKey(const ParameterInfo& parameter)
	{
		std::string key = parameter.name;
		if (!key.empty())
			key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
		return key;
	}

	std::shared_ptr<MidiEffect> createMidiEffect(const std::string& id)
	{
		for (const Effect& effect : registry())
			if (effect.info.id == id)
				return effect.create();
		throw std::invalid_argument("no built-in MIDI effect " + id);
	}

	MidiEffect::MidiEffect(const std::string& id) : m_info(findMidiEffect(id))
	{
	}

	double MidiEffect::clamped(uint32_t id, double value) const noexcept
	{
		const ParameterInfo* parameter = m_info != nullptr ? findParameter(*m_info, id) : nullptr;
		if (parameter == nullptr || !std::isfinite(value))
			return parameter != nullptr ? parameter->defaultValue : 0.0;
		value = std::clamp(value, parameter->minimum, parameter->maximum);
		return parameter->stepped ? std::round(value) : value;
	}

	void MidiEffect::process(const graph::ProcessContext& context) noexcept
	{
		for (const graph::ProcessEvent& event : context.eventsIn)
		{
			advance(std::min(event.offset, context.frames), context);
			if (event.type == Type::Parameter)
				setParameter(event.parameter, event.parameterValue);
			else
				handle(event, context);
		}
		advance(context.frames, context);
		endBlock(context.frames);
	}

	Arpeggiator::Arpeggiator() : MidiEffect("arpeggiator")
	{
	}

	void Arpeggiator::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
	}

	void Arpeggiator::setParameter(uint32_t id, double value) noexcept
	{
		value = clamped(id, value);
		switch (id)
		{
		case Mode:
			m_mode = static_cast<uint8_t>(value);
			break;
		case Rate:
			m_rate = static_cast<uint8_t>(value);
			break;
		case Gate:
			m_gate = value;
			break;
		case Octaves:
			m_octaves = static_cast<int>(value);
			break;
		default:
			break;
		}
	}

	void Arpeggiator::reset() noexcept
	{
		m_heldCount = 0;
		m_running = false;
		m_sounding = false;
		m_step = 0;
	}

	void Arpeggiator::handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept
	{
		Note* const first = m_held.data();
		Note* const last = first + m_heldCount;
		Note* const held = std::find_if(
			first, last, [&event](const Note& note) { return note.channel == event.channel && note.key == event.key; });
		const bool isHeld = held != last;
		if (event.type == Type::NoteOn)
		{
			if (isHeld)
				held->velocity = event.velocity;
			else if (m_heldCount < m_held.size())
				m_held[m_heldCount++] = {event.channel, event.key, event.velocity};
			if (!m_running)
			{
				m_running = true;
				m_step = 0;
				m_next = static_cast<double>(m_time + event.offset);
			}
		}
		else if (event.type == Type::NoteOff)
		{
			if (!isHeld)
				return;
			std::move(held + 1, last, held);
			if (--m_heldCount == 0)
			{
				m_running = false;
				end(event.offset, context.eventsOut);
			}
		}
		else
		{
			context.eventsOut.add(event);
		}
	}

	void Arpeggiator::advance(uint32_t to, const graph::ProcessContext& context) noexcept
	{
		static constexpr std::array<double, 4> quarters = {1.0, 0.5, 0.25, 0.125};
		const double tempo = context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;
		const double frames = m_sampleRate * 60.0 / tempo * quarters[std::min<size_t>(m_rate, quarters.size() - 1)];
		const int64_t until = m_time + to;
		constexpr int64_t never = std::numeric_limits<int64_t>::max();
		for (;;)
		{
			const int64_t stepAt = m_running ? static_cast<int64_t>(std::ceil(m_next)) : never;
			const int64_t endAt = m_sounding ? m_end : never;
			if (std::min(stepAt, endAt) >= until)
				return;
			if (endAt <= stepAt)
				end(static_cast<uint32_t>(std::max(endAt, m_time) - m_time), context.eventsOut);
			else
				step(static_cast<uint32_t>(std::max(stepAt, m_time) - m_time), frames, context.eventsOut);
		}
	}

	void Arpeggiator::step(uint32_t offset, double frames, graph::EventBuffer& out) noexcept
	{
		end(offset, out);
		std::array<Note, g_held> notes = m_held;
		if (m_mode != AsPlayed)
			std::sort(notes.data(), notes.data() + m_heldCount,
				[](const Note& a, const Note& b) { return std::tie(a.key, a.channel) < std::tie(b.key, b.channel); });
		std::array<Note, g_held * 4> pattern{};
		size_t count = 0;
		for (int octave = 0; octave < m_octaves; ++octave)
			for (size_t n = 0; n < m_heldCount; ++n)
				if (notes[n].key + (12 * octave) <= 127)
					pattern[count++] = {
						notes[n].channel, static_cast<uint8_t>(notes[n].key + (12 * octave)), notes[n].velocity};
		if (m_mode == Down)
			std::reverse(pattern.data(), pattern.data() + count);
		const double start = m_next;
		m_next += frames;
		const uint64_t number = m_step++;
		if (count == 0)
			return;
		size_t index = 0;
		if (m_mode == UpDown && count > 1)
		{
			index = static_cast<size_t>(number % ((2 * count) - 2));
			if (index >= count)
				index = (2 * count) - 2 - index;
		}
		else
		{
			index = static_cast<size_t>(number % count);
		}
		const Note& note = pattern[index];
		if (!out.add(
				noteEvent(Type::NoteOn, static_cast<uint16_t>((note.channel * 128) + note.key), note.velocity, offset)))
			return;
		m_sounding = true;
		m_playing = note;
		m_end = std::max(m_time + offset + 1, static_cast<int64_t>(std::ceil(start + (frames * m_gate / 100.0))));
	}

	void Arpeggiator::end(uint32_t offset, graph::EventBuffer& out) noexcept
	{
		if (m_sounding &&
			out.add(
				noteEvent(Type::NoteOff, static_cast<uint16_t>((m_playing.channel * 128) + m_playing.key), 0, offset)))
			m_sounding = false;
	}

	ChordGenerator::ChordGenerator() : MidiEffect("chordGenerator")
	{
		m_chordOf.fill(g_none);
	}

	void ChordGenerator::setParameter(uint32_t id, double value) noexcept
	{
		if (id == Chord)
			m_chord = static_cast<uint8_t>(clamped(id, value));
	}

	void ChordGenerator::reset() noexcept
	{
		m_chordOf.fill(g_none);
		m_sounding.fill(0);
	}

	void ChordGenerator::handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept
	{
		const uint16_t index = indexOf(event);
		if (event.type == Type::NoteOff)
		{
			if (m_chordOf[index] == g_none)
				context.eventsOut.add(event); // a note from before the effect
			else
				release(index, event, context.eventsOut);
			return;
		}
		if (event.type != Type::NoteOn)
		{
			context.eventsOut.add(event);
			return;
		}
		if (m_chordOf[index] != g_none)
			release(index, event, context.eventsOut);
		m_chordOf[index] = m_chord;
		for (const uint8_t interval : g_chords[m_chord])
		{
			if (interval == g_end)
				break;
			if (event.key + interval > 127)
				continue;
			const auto note = static_cast<uint16_t>(index + interval);
			if (m_sounding[note]++ == 0)
				context.eventsOut.add(noteEvent(Type::NoteOn, note, event.velocity, event.offset));
		}
	}

	void ChordGenerator::release(uint16_t index, const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept
	{
		for (const uint8_t interval : g_chords[m_chordOf[index]])
		{
			if (interval == g_end)
				break;
			if (event.key + interval > 127)
				continue;
			const auto note = static_cast<uint16_t>(index + interval);
			if (m_sounding[note] == 0)
				continue;
			--m_sounding[note];
			if (m_sounding[note] == 0)
				out.add(noteEvent(Type::NoteOff, note, event.type == Type::NoteOff ? event.velocity : 0, event.offset));
		}
		m_chordOf[index] = g_none;
	}

	Transformer::Transformer() : MidiEffect("transformer")
	{
		std::iota(m_to.begin(), m_to.end(), uint16_t{0});
	}

	void Transformer::setParameter(uint32_t id, double value) noexcept
	{
		value = clamped(id, value);
		switch (id)
		{
		case Condition:
			m_condition = static_cast<uint8_t>(value);
			break;
		case Minimum:
			m_minimum = static_cast<int>(value);
			break;
		case Maximum:
			m_maximum = static_cast<int>(value);
			break;
		case Action:
			m_action = static_cast<uint8_t>(value);
			break;
		case Value:
			m_value = static_cast<int>(value);
			break;
		default:
			break;
		}
	}

	void Transformer::reset() noexcept
	{
		std::iota(m_to.begin(), m_to.end(), uint16_t{0});
		m_down.fill(false);
	}

	uint16_t Transformer::transform(const graph::ProcessEvent& event, uint16_t& velocity) const noexcept
	{
		const int velocity1 = midi::velocityToMidi1(event.velocity);
		int tested = event.key;
		if (m_condition == Velocity)
			tested = velocity1;
		else if (m_condition == Channel)
			tested = event.channel + 1;
		velocity = event.velocity;
		int channel = event.channel;
		int key = event.key;
		if (m_condition == AllNotes || (tested >= m_minimum && tested <= m_maximum))
			switch (m_action)
			{
			case Transpose:
				key += m_value;
				break;
			case SetVelocity:
				velocity = midi::velocityFromMidi1(static_cast<uint8_t>(std::clamp(m_value, 1, 127)));
				break;
			case AddVelocity:
				velocity = midi::velocityFromMidi1(static_cast<uint8_t>(std::clamp(velocity1 + m_value, 1, 127)));
				break;
			case SetChannel:
				channel = std::clamp(m_value, 1, 16) - 1;
				break;
			default: // Delete
				return static_cast<uint16_t>(g_notes);
			}
		if (key < 0 || key > 127)
			return static_cast<uint16_t>(g_notes);
		return static_cast<uint16_t>((channel * 128) + key);
	}

	void Transformer::handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept
	{
		const uint16_t index = indexOf(event);
		graph::ProcessEvent out = event;
		if (event.type == Type::NoteOn)
		{
			const uint16_t to = transform(event, out.velocity);
			if (m_down[index] && m_to[index] != g_notes && m_to[index] != to)
				context.eventsOut.add(noteEvent(Type::NoteOff, m_to[index], 0, event.offset));
			m_down[index] = true;
			m_to[index] = to;
		}
		else if (event.type == Type::NoteOff || event.type == Type::PolyPressure)
		{
			if (!m_down[index]) // a note from before the effect
				m_to[index] = index;
			if (event.type == Type::NoteOff)
				m_down[index] = false;
		}
		else
		{
			context.eventsOut.add(event);
			return;
		}
		const uint16_t to = m_to[index];
		if (to == g_notes)
			return;
		out.channel = static_cast<uint8_t>(to / 128);
		out.key = static_cast<uint8_t>(to % 128);
		context.eventsOut.add(out);
	}
}
