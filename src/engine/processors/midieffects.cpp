// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/midieffects.h"

#include "engine/midi/midivalues.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
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

	double Arpeggiator::stepFrames(uint32_t offset, const graph::TransportBlock& transport) const noexcept
	{
		static constexpr std::array<double, 4> quarters = {1.0, 0.5, 0.25, 0.125};
		double tempo = transport.tempo;
		for (size_t s = 0; transport.tempoMap != nullptr && s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			if (offset >= segment.offset && offset - segment.offset < segment.frames)
			{
				const int64_t position = segment.position + (offset - segment.offset);
				tempo = transport.tempoMap->bpmAt(transport.tempoMap->toTicks(position, m_sampleRate));
				break;
			}
		}
		if (!(tempo > 0.0))
			tempo = 120.0;
		return m_sampleRate * 60.0 / tempo * quarters[std::min<size_t>(m_rate, quarters.size() - 1)];
	}

	void Arpeggiator::advance(uint32_t to, const graph::ProcessContext& context) noexcept
	{
		// Steps missed in an earlier block, whose note-ons found no room, are left out rather than played late.
		while (m_running && static_cast<int64_t>(std::ceil(m_next)) < m_time)
		{
			m_next += stepFrames(0, context.transport);
			++m_step;
		}
		const int64_t until = m_time + to;
		constexpr int64_t never = std::numeric_limits<int64_t>::max();
		for (;;)
		{
			const int64_t stepAt = m_running ? static_cast<int64_t>(std::ceil(m_next)) : never;
			const int64_t endAt = m_sounding ? m_end : never;
			if (std::min(stepAt, endAt) >= until)
				return;
			// With the output full, an end that can't be added is tried again on the next call or in the next block.
			const auto offset = static_cast<uint32_t>(std::max(std::min(stepAt, endAt), m_time) - m_time);
			const bool added = endAt <= stepAt ? end(offset, context.eventsOut)
											   : step(offset, stepFrames(offset, context.transport), context.eventsOut);
			if (!added)
				return;
		}
	}

	bool Arpeggiator::step(uint32_t offset, double frames, graph::EventBuffer& out) noexcept
	{
		if (!end(offset, out))
			return false;
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
			return true;
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
			return true; // the step is left out
		m_sounding = true;
		m_playing = note;
		m_end = std::max(m_time + offset + 1, static_cast<int64_t>(std::ceil(start + (frames * m_gate / 100.0))));
		return true;
	}

	bool Arpeggiator::end(uint32_t offset, graph::EventBuffer& out) noexcept
	{
		if (m_sounding &&
			out.add(
				noteEvent(Type::NoteOff, static_cast<uint16_t>((m_playing.channel * 128) + m_playing.key), 0, offset)))
			m_sounding = false;
		return !m_sounding;
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
				context.eventsOut.addEnd(event); // a note from before the effect
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
				out.addEnd(
					noteEvent(Type::NoteOff, note, event.type == Type::NoteOff ? event.velocity : 0, event.offset));
		}
		m_chordOf[index] = g_none;
	}

	namespace
	{
		using midi::LogicalEvent;
		using midi::LogicalType;

		// An event as the rules see it; false for those they don't: note-offs, SysEx and parameter changes.
		bool toLogical(const graph::ProcessEvent& event, LogicalEvent& out) noexcept
		{
			LogicalType type = LogicalType::Note;
			switch (event.type)
			{
			case Type::NoteOn:
				break;
			case Type::PolyPressure:
				type = LogicalType::PolyPressure;
				break;
			case Type::Controller:
				type = LogicalType::Controller;
				break;
			case Type::Program:
				type = LogicalType::Program;
				break;
			case Type::ChannelPressure:
				type = LogicalType::Aftertouch;
				break;
			case Type::PitchBend:
				type = LogicalType::PitchBend;
				break;
			default:
				return false;
			}
			out = LogicalEvent{};
			out.channel = (event.channel % 16) + 1;
			midi::setLogicalValues(type, event.key, event.value, out);
			if (type == LogicalType::Note)
				out.mainValue = midi::noteOnVelocityToMidi1(event.velocity);
			return true;
		}

		// The event the rules made of source, which was original: values they didn't change keep their full
		// resolution.
		graph::ProcessEvent fromLogical(
			const LogicalEvent& changed, const LogicalEvent& original, const graph::ProcessEvent& source) noexcept
		{
			graph::ProcessEvent out = source;
			out.channel = static_cast<uint8_t>(changed.channel - 1);
			switch (changed.type)
			{
			case LogicalType::Note:
			{
				const auto main = static_cast<uint8_t>(changed.mainValue);
				out.key = static_cast<uint8_t>(changed.subtype);
				if (changed.type != original.type)
					out.type = main > 0 ? Type::NoteOn : Type::NoteOff;
				if (out.type == Type::NoteOff)
					out.velocity = midi::velocityFromMidi1(64);
				else if (changed.type != original.type || changed.mainValue != original.mainValue)
					out.velocity = midi::velocityFromMidi1(std::max<uint8_t>(main, 1));
				return out;
			}
			case LogicalType::PolyPressure:
				out.type = Type::PolyPressure;
				break;
			case LogicalType::Controller:
				out.type = Type::Controller;
				break;
			case LogicalType::Program:
				out.type = Type::Program;
				break;
			case LogicalType::Aftertouch:
				out.type = Type::ChannelPressure;
				break;
			case LogicalType::PitchBend:
				out.type = Type::PitchBend;
				break;
			}
			midi::logicalValues(changed, original, out.key, out.value);
			return out;
		}

		// Adds an event that ends notes (a note-off, a pedal's release) before the other events in a full buffer, any
		// other event as it fits.
		void put(graph::EventBuffer& out, const graph::ProcessEvent& event) noexcept
		{
			if (graph::endsNotes(event))
				out.addEnd(event);
			else
				out.add(event);
		}

		graph::ProcessEvent movedTo(graph::ProcessEvent event, uint16_t index) noexcept
		{
			event.channel = static_cast<uint8_t>(index / 128);
			event.key = static_cast<uint8_t>(index % 128);
			return event;
		}
	}

	LogicalStage::LogicalStage() = default;

	void LogicalStage::reset() noexcept
	{
		m_held.fill(Held{});
		m_sounding.fill(0);
		m_made.fill(false);
		m_random = {};
	}

	void LogicalStage::playMade(const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept
	{
		if (event.type != Type::NoteOn && event.type != Type::NoteOff)
		{
			out.add(event);
			return;
		}
		// The note another event made is held until another one makes its note-off, and counts as a held note does.
		const uint16_t note = indexOf(event);
		const bool on = event.type == Type::NoteOn;
		if (m_made[note] == on)
		{
			if (!on && m_sounding[note] == 0)
				put(out, event); // ends nothing that sounds
			return;
		}
		m_made[note] = on;
		if (on ? m_sounding[note]++ == 0 : release(note))
			put(out, event);
	}

	bool LogicalStage::uses(const Held& held, uint16_t source, uint16_t note) noexcept
	{
		return held.down && ((held.passed && note == source) || (held.to != g_none && held.to == note));
	}

	bool LogicalStage::release(uint16_t note) noexcept
	{
		if (m_sounding[note] > 0)
			--m_sounding[note];
		return m_sounding[note] == 0;
	}

	void LogicalStage::handle(
		const graph::ProcessEvent& event, const LogicalRulesView& rules, graph::EventBuffer& out) noexcept
	{
		using midi::LogicalFunction;
		const uint16_t index = indexOf(event);
		Held& held = m_held[index];
		LogicalEvent original;
		const bool logical = toLogical(event, original);
		const bool found = logical && midi::matches(rules.filters, rules.filterCount, original);
		// The pressure of a held note the rules don't find follows its note-on. Where two notes went to one note, the
		// note-off of the last of them ends it.
		if (event.type == Type::NoteOff || (event.type == Type::PolyPressure && held.down && !found))
		{
			const bool off = event.type == Type::NoteOff;
			// A note from before the rules, or one that passed.
			if (!held.down || (held.passed && (!off || release(index))))
				put(out, event);
			if (held.down && held.to != g_none && (!held.passed || held.to != index) && (!off || release(held.to)))
				put(out, movedTo(event, held.to));
			if (off)
				held.down = false;
			return;
		}
		if (!logical)
		{
			out.add(event);
			return;
		}
		const LogicalFunction function = rules.function;
		bool pass = function == LogicalFunction::Insert || function == LogicalFunction::Select ||
			(!found && function != LogicalFunction::InsertExclusive);
		bool emit = found &&
			(function == LogicalFunction::Transform || function == LogicalFunction::Insert ||
				function == LogicalFunction::InsertExclusive);
		graph::ProcessEvent changed;
		if (emit)
		{
			LogicalEvent result = original;
			emit = midi::transform(rules.actions, rules.actionCount, result, m_random);
			if (emit)
				changed = fromLogical(result, original, event);
		}
		if (event.type != Type::NoteOn)
		{
			if (pass)
				out.add(event);
			if (emit)
				playMade(changed, out);
			return;
		}
		// A note-on that doesn't fit goes, and so does its note-off, which then leaves room for those of the notes
		// that do sound.
		if (out.size() + (pass ? 1 : 0) + (emit ? 1 : 0) > out.capacity())
			pass = emit = false;
		const uint16_t to = emit && changed.type == Type::NoteOn ? indexOf(changed) : g_none;
		const Held previous = held;
		held = {to, pass, true};
		// Played again before its note-off: ends what the earlier one started and this one doesn't, and plays again
		// what both do.
		for (const uint16_t note : {index, previous.to != index ? previous.to : g_none})
		{
			if (note == g_none || !uses(previous, index, note))
				continue;
			if (uses(held, index, note))
				--m_sounding[note]; // counted again below
			else if (release(note))
				put(out, noteEvent(Type::NoteOff, note, 0, event.offset));
		}
		// A note another note already plays sounds once.
		const auto start = [&](uint16_t note) { return m_sounding[note]++ == 0 || uses(previous, index, note); };
		const bool playItself = pass && start(index);
		if (playItself)
			out.add(event);
		if (emit && (to == g_none || (pass && to == index ? playItself : start(to))))
			out.add(changed);
	}

	Transformer::Transformer() : MidiEffect("transformer")
	{
		update();
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
		update();
	}

	void Transformer::update() noexcept
	{
		using midi::LogicalCondition;
		using midi::LogicalFunction;
		using midi::LogicalOperation;
		using midi::LogicalTarget;
		m_filters[0] = {0, LogicalTarget::Type, LogicalCondition::Equal, static_cast<double>(LogicalType::Note), 0.0, 0,
			midi::LogicalBool::And};
		constexpr std::array<LogicalTarget, 4> tested = {
			LogicalTarget::Subtype, LogicalTarget::Subtype, LogicalTarget::MainValue, LogicalTarget::Channel};
		m_filters[1] = {0, tested[m_condition % 4U], LogicalCondition::InsideRange, static_cast<double>(m_minimum),
			static_cast<double>(m_maximum), 0, midi::LogicalBool::And};
		const auto value = static_cast<double>(m_value);
		switch (m_action)
		{
		case Transpose:
			m_change = {LogicalTarget::Subtype, LogicalOperation::Add, value, 0.0};
			break;
		case SetVelocity:
			m_change = {LogicalTarget::MainValue, LogicalOperation::SetFixed, value, 0.0};
			break;
		case AddVelocity:
			m_change = {LogicalTarget::MainValue, LogicalOperation::Add, value, 0.0};
			break;
		default: // SetChannel, and Delete, which has no action
			m_change = {LogicalTarget::Channel, LogicalOperation::SetFixed, value, 0.0};
			break;
		}
		const bool remove = m_action == Delete;
		m_rules = {m_filters.data(), m_condition == AllNotes ? size_t{1} : size_t{2},
			remove ? LogicalFunction::Delete : LogicalFunction::Transform, &m_change, remove ? size_t{0} : size_t{1}};
	}

	void Transformer::reset() noexcept
	{
		m_stage.reset();
	}

	void Transformer::handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept
	{
		m_stage.handle(event, m_rules, context.eventsOut);
	}

	InputTransformerProcessor::InputTransformerProcessor(size_t capacity) : m_first(capacity), m_second(capacity)
	{
	}

	void InputTransformerProcessor::process(
		graph::EventBuffer& events, const std::vector<midi::LogicalRules>& modules) noexcept
	{
		if (events.empty())
			return;
		const graph::EventBuffer* in = &events;
		for (size_t m = 0; m < m_stages.size(); ++m)
		{
			graph::EventBuffer& out = in == &m_first ? m_second : m_first;
			out.clear();
			LogicalRulesView view{nullptr, 0, midi::LogicalFunction::Select, nullptr, 0}; // passes events on
			if (m < modules.size())
			{
				const midi::LogicalRules& rules = modules[m];
				view = {rules.filters.data(), rules.filters.size(), rules.function, rules.actions.data(),
					rules.actions.size()};
			}
			for (const graph::ProcessEvent& event : *in)
				m_stages[m].handle(event, view, out);
			in = &out;
		}
		events.clear();
		for (const graph::ProcessEvent& event : *in)
			events.add(event);
	}
}
