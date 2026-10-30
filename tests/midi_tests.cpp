// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sequencer.h"
#include "engine/document/rendersnapshot.h"
#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using graph::ProcessEvent;
using Type = graph::ProcessEvent::Type;

namespace
{
	// An event as a test expects it: its frame counted from the first block, and what it is.
	struct Heard
	{
		int64_t frame = 0;
		Type type = Type::NoteOn;
		int channel = 0;
		int key = 0;
		uint32_t value = 0;
		std::vector<uint8_t> data; // SysEx

		bool operator==(const Heard& other) const
		{
			return frame == other.frame && type == other.type && channel == other.channel && key == other.key &&
				value == other.value && data == other.data;
		}
	};

	std::ostream& operator<<(std::ostream& out, const Heard& heard)
	{
		return out << "{frame " << heard.frame << ", type " << static_cast<int>(heard.type) << ", channel "
				   << heard.channel << ", key " << heard.key << ", value " << heard.value << "}";
	}

	Heard on(int64_t frame, int key, int channel = 0)
	{
		return {frame, Type::NoteOn, channel, key, 0, {}};
	}

	Heard off(int64_t frame, int key, int channel = 0)
	{
		return {frame, Type::NoteOff, channel, key, 0, {}};
	}

	RenderNote note(int64_t start, int64_t end, int pitch, int channel = 0)
	{
		RenderNote result;
		result.start = start;
		result.end = end;
		result.pitch = static_cast<uint8_t>(pitch);
		result.velocity = 40000;
		result.channel = static_cast<uint8_t>(channel);
		return result;
	}

	RenderController control(int64_t position, int number, uint32_t value)
	{
		RenderController result;
		result.position = position;
		result.number = static_cast<uint8_t>(number);
		result.value = value;
		return result;
	}

	// Drives a processor block by block with the transport's cycle model and collects what it sends on.
	class Player
	{
	public:
		Player(std::shared_ptr<graph::Processor> processor, uint32_t blockSize)
			: m_processor(std::move(processor)), m_blockSize(blockSize), m_in(graph::g_maxEventsPerBlock),
			  m_out(graph::g_maxEventsPerBlock)
		{
			m_processor->prepareFor(48000.0, blockSize);
			for (int c = 0; c < 2; ++c)
			{
				m_buffers[c].assign(blockSize, 0.0f);
				m_audio[c] = m_buffers[c].data();
			}
		}

		void setCycle(int64_t left, int64_t right)
		{
			m_cycleLeft = left;
			m_cycleRight = right;
		}

		void locate(int64_t position) { m_position = position; }

		// Plays frames from the current position, block by block.
		void play(int64_t frames)
		{
			while (frames > 0)
			{
				const auto block = static_cast<uint32_t>(std::min<int64_t>(m_blockSize, frames));
				std::vector<graph::TransportSegment> segments;
				uint32_t offset = 0;
				while (offset < block)
				{
					uint32_t length = block - offset;
					const bool cycles = m_cycleRight > m_cycleLeft && m_position < m_cycleRight;
					if (cycles)
						length = static_cast<uint32_t>(std::min<int64_t>(length, m_cycleRight - m_position));
					segments.push_back({offset, length, m_position});
					offset += length;
					m_position += length;
					if (cycles && m_position == m_cycleRight)
						m_position = m_cycleLeft;
				}
				process({true, segments.front().position, segments.data(), segments.size()}, block);
				frames -= block;
			}
		}

		// One stopped block.
		void stop() { process({false, m_position, nullptr, 0}, m_blockSize); }

		// Events for the processor's input in the next block.
		void send(const ProcessEvent& event) { m_in.add(event); }

		std::vector<Heard> heard;

	private:
		void process(const graph::TransportBlock& transport, uint32_t frames)
		{
			m_out.clear();
			m_processor->process({m_audio.data(), 2, frames, m_in, m_out, transport});
			m_in.clear();
			for (const ProcessEvent& event : m_out)
			{
				Heard h{m_frame + event.offset, event.type, event.channel, event.key, event.value, {}};
				if (event.type == Type::NoteOn || event.type == Type::NoteOff)
					h.value = 0;
				if (event.type == Type::SysEx)
					h.data.assign(event.data, event.data + event.size);
				heard.push_back(std::move(h));
			}
			m_frame += frames;
		}

		std::shared_ptr<graph::Processor> m_processor;
		uint32_t m_blockSize;
		graph::EventBuffer m_in;
		graph::EventBuffer m_out;
		std::array<std::vector<float>, 2> m_buffers;
		std::array<float*, 2> m_audio{};
		int64_t m_cycleLeft = 0;
		int64_t m_cycleRight = 0;
		int64_t m_position = 0;
		int64_t m_frame = 0;
	};

	std::shared_ptr<MidiSequencer> sequencer(
		std::vector<RenderNote> notes, std::vector<RenderController> controllers = {})
	{
		auto result = std::make_shared<MidiSequencer>();
		auto sequence = std::make_unique<MidiSequence>();
		sequence->notes = std::move(notes);
		sequence->controllers = std::move(controllers);
		result->setSequence(std::move(sequence));
		return result;
	}
}

TEST_CASE("MIDI 1.0 values scale to the internal resolution and back without loss", "[unit]")
{
	using namespace midi;
	for (uint32_t v = 0; v < 128; ++v)
	{
		CHECK(velocityToMidi1(velocityFromMidi1(static_cast<uint8_t>(v))) == v);
		CHECK(controllerToMidi1(controllerFromMidi1(static_cast<uint8_t>(v))) == v);
	}
	for (uint32_t v = 0; v < 16384; ++v)
		CHECK(pitchBendToMidi1(pitchBendFromMidi1(static_cast<uint16_t>(v))) == v);

	// Zero, the center and the maximum stay where they are.
	CHECK(velocityFromMidi1(0) == 0);
	CHECK(velocityFromMidi1(64) == 0x8000);
	CHECK(velocityFromMidi1(127) == 0xFFFF);
	CHECK(controllerFromMidi1(64) == 0x80000000U);
	CHECK(controllerFromMidi1(127) == 0xFFFFFFFFU);
	CHECK(pitchBendFromMidi1(8192) == g_pitchBendCenter);
	CHECK(pitchBendFromMidi1(16383) == 0xFFFFFFFFU);

	// Rising MIDI 1.0 values stay rising.
	for (uint32_t v = 1; v < 128; ++v)
		CHECK(controllerFromMidi1(static_cast<uint8_t>(v)) > controllerFromMidi1(static_cast<uint8_t>(v - 1)));

	// A soft note-on stays a note-on in MIDI 1.0, where velocity 0 means note-off.
	CHECK(noteOnVelocityToMidi1(1) == 1);
	CHECK(noteOnVelocityToMidi1(0xFFFF) == 127);
}

TEST_CASE("the sequencer sends notes and controllers at their sample offsets, for any block size", "[unit]")
{
	const uint32_t blockSize = GENERATE(1U, 7U, 16U, 64U);
	INFO("block size " << blockSize);
	Player player(
		sequencer({note(10, 30, 60), note(15, 40, 64), note(30, 31, 60)}, {control(12, 7, 99), control(30, 1, 5)}),
		blockSize);
	player.play(50);

	const std::vector<Heard> expected = {on(10, 60), {12, Type::Controller, 0, 7, 99, {}}, on(15, 64),
		// At one sample: note-offs first, then controllers, then note-ons.
		off(30, 60), {30, Type::Controller, 0, 1, 5, {}}, on(30, 60), off(31, 60), off(40, 64)};
	CHECK(player.heard == expected);
}

TEST_CASE("stopping and locating end the notes that sound", "[unit]")
{
	Player player(sequencer({note(0, 100, 62), note(5, 100, 60, 3), note(200, 300, 67), note(250, 260, 69)}), 16);
	player.play(32); // both notes sound
	player.stop();   // note-offs at the stopped block's start, by channel and pitch
	player.stop();   // nothing more
	CHECK(player.heard == std::vector<Heard>{on(0, 62), on(5, 60, 3), off(32, 62), off(32, 60, 3)});

	player.heard.clear();
	player.locate(240);
	player.play(16); // frames 64 to 80; 67 started before the locate position: not played
	player.locate(10);
	player.play(16); // the locate ends 69 at the block's start
	CHECK(player.heard == std::vector<Heard>{on(74, 69), off(80, 69)});
}

TEST_CASE("cycle playback ends notes at the jump and plays the notes after the left locator again", "[unit]")
{
	const uint32_t blockSize = GENERATE(1U, 16U, 64U, 100U);
	INFO("block size " << blockSize);
	// Cycle 100 to 200: a note from before the left locator, one across the right one, one at the left locator.
	Player player(sequencer({note(90, 120, 50), note(100, 110, 52), note(180, 250, 54)}), blockSize);
	player.setCycle(100, 200);
	player.locate(80);
	player.play(320); // 80 to 200, 100 to 200, 100 to 200

	const std::vector<Heard> expected = {on(10, 50), on(20, 52), off(30, 52), off(40, 50), on(100, 54),
		// The jump at frame 120 ends 54; the note at the left locator starts there.
		off(120, 54), on(120, 52), off(130, 52), on(200, 54), off(220, 54), on(220, 52), off(230, 52), on(300, 54)};
	CHECK(player.heard == expected);
}

TEST_CASE("a note that starts again while it sounds ends first, and a removed note still ends", "[unit]")
{
	auto sequence = sequencer({note(0, 50, 60), note(20, 30, 60)});
	Player player(sequence, 16);
	player.play(16);
	auto empty = std::make_unique<MidiSequence>();
	sequence->setSequence(std::move(empty)); // the note at 0 is gone from the project while it sounds
	player.play(48);
	CHECK(player.heard == std::vector<Heard>{on(0, 60), off(50, 60)});

	Player again(sequencer({note(0, 50, 60), note(20, 30, 60)}), 16);
	again.play(64);
	CHECK(again.heard == std::vector<Heard>{on(0, 60), off(20, 60), on(20, 60), off(30, 60)});
}

TEST_CASE("every kind of MIDI event reaches the instrument with its values", "[unit]")
{
	std::vector<RenderController> controllers(6);
	controllers[0] = control(1, 74, 0x12345678);
	controllers[1].position = 2;
	controllers[1].type = ControllerType::PitchBend;
	controllers[1].value = midi::g_pitchBendCenter + 5;
	controllers[1].channel = 2;
	controllers[2].position = 3;
	controllers[2].type = ControllerType::ChannelPressure;
	controllers[2].value = 77;
	controllers[3].position = 4;
	controllers[3].type = ControllerType::PolyPressure;
	controllers[3].number = 61;
	controllers[3].value = 88;
	controllers[4].position = 5;
	controllers[4].type = ControllerType::Program;
	controllers[4].number = 12;
	controllers[4].value = 300; // the bank
	controllers[5].position = 6;
	controllers[5].type = ControllerType::SysEx;
	controllers[5].data = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};

	Player player(sequencer({}, controllers), 4);
	ProcessEvent incoming;
	incoming.offset = 1;
	incoming.type = Type::NoteOn;
	incoming.key = 40;
	player.send(incoming); // from a MIDI track routed here: passed on in offset order, note-ons after controllers
	player.play(8);

	const std::vector<Heard> expected = {{1, Type::Controller, 0, 74, 0x12345678, {}}, on(1, 40),
		{2, Type::PitchBend, 2, 0, midi::g_pitchBendCenter + 5, {}}, {3, Type::ChannelPressure, 0, 0, 77, {}},
		{4, Type::PolyPressure, 0, 61, 88, {}}, {5, Type::Program, 0, 12, 300, {}},
		{6, Type::SysEx, 0, 0, 0, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}};
	CHECK(player.heard == expected);
}

namespace
{
	// An instrument that hands on what it gets, so the test sees what reached it.
	class Echo : public graph::Processor
	{
	public:
		void process(const graph::ProcessContext& context) noexcept override
		{
			for (const ProcessEvent& event : context.eventsIn)
				context.eventsOut.add(event);
		}
	};
}

namespace
{
	ProcessEvent noteOnEvent(uint32_t offset, int key)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = Type::NoteOn;
		event.key = static_cast<uint8_t>(key);
		return event;
	}

	// Fills the processor's input for the next block, as many MIDI tracks with dense controller lanes would.
	void fillInput(Player& player)
	{
		ProcessEvent event;
		event.type = Type::Controller;
		for (size_t i = 0; i < graph::g_maxEventsPerBlock; ++i)
			player.send(event);
	}

	std::vector<Heard> notesIn(const std::vector<Heard>& heard)
	{
		std::vector<Heard> notes;
		std::copy_if(heard.begin(), heard.end(), std::back_inserter(notes),
			[](const Heard& h) { return h.type == Type::NoteOn || h.type == Type::NoteOff; });
		return notes;
	}
}

TEST_CASE("an instrument channel plays its own parts and its MIDI tracks", "[unit]")
{
	auto channel = std::make_shared<InstrumentChannel>(sequencer({note(4, 100, 70)}), std::make_shared<Echo>());
	Player player(channel, 8);
	player.send(noteOnEvent(2, 30)); // from a MIDI track routed here
	player.play(8);
	CHECK(player.heard == std::vector<Heard>{on(2, 30), on(4, 70)});
}

TEST_CASE("a note that starts where another source's note of the same pitch ends sounds", "[unit]")
{
	// The instrument track's own note ends at 8, where a MIDI track routed to it starts the same pitch.
	auto channel = std::make_shared<InstrumentChannel>(sequencer({note(0, 8, 60)}), std::make_shared<Echo>());
	Player player(channel, 8);
	player.play(8);
	player.send(noteOnEvent(0, 60));
	player.play(8);
	CHECK(player.heard == std::vector<Heard>{on(0, 60), off(8, 60), on(8, 60)});
}

TEST_CASE("when a MIDI track leaves, its instrument ends the notes only that track holds", "[unit]")
{
	// Two MIDI tracks routed to the instrument track: one stays, one is removed or rerouted.
	auto staying = sequencer({note(0, 100, 31)});
	auto departed = sequencer({note(0, 100, 30), note(0, 100, 31), note(0, 100, 40)});
	Player stayingPlayer(staying, 8);
	Player departedPlayer(departed, 8);
	stayingPlayer.play(8);
	departedPlayer.play(8);

	auto channel = std::make_shared<InstrumentChannel>(sequencer({note(4, 100, 70)}), std::make_shared<Echo>());
	Player player(channel, 8);
	for (const int key : {30, 31, 31, 40})
		player.send(noteOnEvent(0, key)); // what the two tracks sent
	player.play(8);
	REQUIRE(player.heard == std::vector<Heard>{on(0, 30), on(0, 31), on(0, 31), on(0, 40), on(4, 70)});

	// The audio thread does this when it starts the program without the departed track. The staying track still
	// holds 31 and the instrument track its own 70.
	player.heard.clear();
	channel->releaseNotesOf(*departed, {staying});
	player.play(8);
	CHECK(player.heard == std::vector<Heard>{off(8, 30), off(8, 40)});

	// The departed track forgot its notes: wherever it goes now, it doesn't end them again.
	departedPlayer.heard.clear();
	departedPlayer.stop();
	CHECK(departedPlayer.heard.empty());
}

TEST_CASE("an instrument channel ends every note it holds when the transport stops", "[unit]")
{
	auto channel = std::make_shared<InstrumentChannel>(sequencer({note(4, 100, 70)}), std::make_shared<Echo>());
	Player player(channel, 8);
	player.send(noteOnEvent(2, 30)); // from a MIDI track that is gone by now, so nothing else ends it
	player.play(8);
	player.stop();
	player.stop(); // nothing more
	CHECK(player.heard == std::vector<Heard>{on(2, 30), on(4, 70), off(8, 70), off(8, 30)});
}

TEST_CASE("a full event buffer delays note-offs to a later block instead of dropping them", "[unit]")
{
	SECTION("the sequencer's own controller events fill the block")
	{
		const std::vector<RenderController> dense(graph::g_maxEventsPerBlock + 100, control(2, 1, 0));
		Player player(sequencer({note(0, 10, 60), note(5, 70, 62)}, dense), 64);
		player.play(128);
		// The block holds the first note-on and controllers up to its capacity; the rest of the block is skipped,
		// and the note that ended in it ends where the next block starts.
		CHECK(player.heard.size() == graph::g_maxEventsPerBlock + 1);
		CHECK(notesIn(player.heard) == std::vector<Heard>{on(0, 60), off(64, 60)});
	}
	SECTION("a note ends in a block that incoming events fill")
	{
		Player player(sequencer({note(0, 20, 60)}), 16);
		player.play(16);
		fillInput(player);
		player.play(16); // the note ends at 20 but there is no room
		player.play(16);
		CHECK(notesIn(player.heard) == std::vector<Heard>{on(0, 60), off(32, 60)});
	}
	SECTION("the transport stops in a full block")
	{
		Player player(sequencer({note(0, 1000, 60)}), 16);
		player.play(16);
		fillInput(player);
		player.stop();
		player.stop();
		CHECK(notesIn(player.heard) == std::vector<Heard>{on(0, 60), off(32, 60)});
	}
	SECTION("playback jumps in a full block")
	{
		// Cycle 0 to 100: the note doesn't end before the jump, and after it its end is never reached.
		Player player(sequencer({note(90, 300, 60)}), 16);
		player.setCycle(0, 100);
		player.locate(80);
		player.play(16);
		fillInput(player);
		player.play(32); // jumps at frame 20, in the full block
		CHECK(notesIn(player.heard) == std::vector<Heard>{on(10, 60), off(32, 60)});
	}
}

namespace
{
	// An instrument in a graph that notes what reaches it and which keys it holds.
	class Recorder : public graph::Processor
	{
	public:
		void process(const graph::ProcessContext& context) noexcept override
		{
			for (const ProcessEvent& event : context.eventsIn)
			{
				const bool note = event.type == Type::NoteOn || event.type == Type::NoteOff;
				heard.push_back(
					{m_frame + event.offset, event.type, event.channel, event.key, note ? 0 : event.value, {}});
				if (note)
					held[((event.channel % 16) * 128) + (event.key % 128)] = event.type == Type::NoteOn;
			}
			m_frame += context.frames;
		}

		std::vector<Heard> heard;
		std::array<bool, size_t{16} * 128> held{};

	private:
		int64_t m_frame = 0;
	};

	// Plays a graph from frame 0, block by block.
	void play(const graph::CompiledGraph& compiled, uint32_t blockSize, int64_t frames)
	{
		for (int64_t position = 0; position < frames; position += blockSize)
		{
			const graph::TransportSegment segment{0, blockSize, position};
			compiled.process(blockSize, {true, position, &segment, 1});
		}
	}

	graph::NodeDesc instrumentNode(std::shared_ptr<graph::Processor> instrument)
	{
		return {graph::NodeKind::Channel, 0, ChannelFormat::Stereo,
			std::make_shared<InstrumentChannel>(sequencer({}), std::move(instrument))};
	}
}

TEST_CASE("every note ends when several MIDI tracks fill an instrument's input in one block", "[unit]")
{
	// Four MIDI tracks start 256 notes each together, and at frame 400 end them where as many new notes on other
	// channels start: twice the events an instrument's input holds.
	const uint32_t blockSize = GENERATE(1u, 64u, 333u);
	CAPTURE(blockSize);
	auto recorder = std::make_shared<Recorder>();
	graph::GraphDesc desc;
	for (int track = 0; track < 4; ++track)
	{
		std::vector<RenderNote> notes;
		for (const auto& [start, end] : {std::pair{0, 400}, std::pair{400, 800}})
			for (int n = 0; n < 256; ++n)
				notes.push_back(note(start, end, n % 128, (start / 50) + (track * 2) + (n / 128)));
		desc.nodes.push_back({graph::NodeKind::Channel, 0, ChannelFormat::None, sequencer(std::move(notes))});
		desc.events.push_back({track, 4});
	}
	desc.nodes.push_back(instrumentNode(recorder));
	const graph::CompiledGraph compiled(desc, 48000.0, blockSize);
	play(compiled, blockSize, 1200);
	CHECK(std::count_if(recorder->heard.begin(), recorder->heard.end(),
			  [](const Heard& h) { return h.type == Type::NoteOn; }) >= 1024);
	CHECK(std::count(recorder->held.begin(), recorder->held.end(), true) == 0);
}

TEST_CASE("one source's events at one offset reach the instrument in their order", "[unit]")
{
	// Live MIDI input or a plugin on one MIDI track plays a note of no length, and a note-on followed by pressure.
	// Another MIDI track's note of the same pitch ends there, and its note-off still comes before the note-ons.
	auto recorder = std::make_shared<Recorder>();
	graph::GraphDesc desc;
	desc.nodes.push_back({graph::NodeKind::Channel, 0, ChannelFormat::None, nullptr});
	desc.nodes.push_back({graph::NodeKind::Channel, 0, ChannelFormat::None, nullptr});
	desc.nodes.push_back(instrumentNode(recorder));
	desc.events = {{0, 2}, {1, 2}};
	const graph::CompiledGraph compiled(desc, 48000.0, 8);
	const auto noteOffEvent = [](uint32_t offset, int key)
	{
		ProcessEvent event = noteOnEvent(offset, key);
		event.type = Type::NoteOff;
		return event;
	};
	ProcessEvent pressure;
	pressure.offset = 3;
	pressure.type = Type::ChannelPressure;
	pressure.value = 77;
	for (const ProcessEvent& event : {noteOnEvent(3, 60), noteOffEvent(3, 60), noteOnEvent(3, 62), pressure})
		REQUIRE(compiled.events(0).add(event));
	REQUIRE(compiled.events(1).add(noteOffEvent(3, 62)));
	play(compiled, 8, 8);
	CHECK(recorder->heard ==
		std::vector<Heard>{off(3, 62), on(3, 60), off(3, 60), on(3, 62), {3, Type::ChannelPressure, 0, 0, 77, {}}});
	CHECK(recorder->held[62]);
}

namespace
{
	RenderController controller(int64_t position, ControllerType type, int number, uint32_t value, int channel = 0)
	{
		RenderController result;
		result.position = position;
		result.type = type;
		result.number = static_cast<uint8_t>(number);
		result.value = value;
		result.channel = static_cast<uint8_t>(channel);
		return result;
	}

	MidiChase chaseAll()
	{
		MidiChase chase;
		chase.notes = true;
		chase.controllers = true;
		chase.programs = true;
		chase.pitchBend = true;
		return chase;
	}

	std::shared_ptr<MidiSequencer> chasing(MidiChase chase, std::vector<RenderNote> notes,
		std::vector<RenderController> controllers = {}, std::vector<RenderPart> parts = {})
	{
		auto result = std::make_shared<MidiSequencer>();
		auto sequence = std::make_unique<MidiSequence>();
		sequence->chase.push_back(makeChaseTrack(controllers, std::move(parts)));
		sequence->notes = std::move(notes);
		sequence->controllers = std::move(controllers);
		result->setSequence(std::move(sequence));
		result->setChase(chase);
		return result;
	}

	// Sends events of another kind for the processor's input in the next block, leaving room for count events.
	void fillInputBut(Player& player, size_t count)
	{
		ProcessEvent event;
		event.type = Type::ChannelPressure;
		for (size_t i = count; i < graph::g_maxEventsPerBlock; ++i)
			player.send(event);
	}

	std::vector<Heard> ofType(const std::vector<Heard>& heard, Type type)
	{
		std::vector<Heard> result;
		std::copy_if(
			heard.begin(), heard.end(), std::back_inserter(result), [type](const Heard& h) { return h.type == type; });
		return result;
	}

	// The types of the events heard at a frame, in their order.
	std::vector<Type> typesAt(const std::vector<Heard>& heard, int64_t frame)
	{
		std::vector<Type> types;
		for (const Heard& h : heard)
			if (h.frame == frame)
				types.push_back(h.type);
		return types;
	}
}

TEST_CASE("locating into held notes starts them there and ends them where they end, for any block size", "[unit]")
{
	const uint32_t blockSize = GENERATE(1U, 7U, 16U, 64U);
	INFO("block size " << blockSize);
	// At 120: 60 and 64 sound, 67 has ended, 62 starts there, and 70 started again at 100: that one is chased.
	Player player(chasing(chaseAll(),
					  {note(0, 300, 60), note(10, 400, 70), note(50, 200, 64), note(90, 110, 67), note(100, 130, 70),
						  note(120, 150, 62)}),
		blockSize);
	player.locate(120);
	player.play(200);
	const std::vector<Heard> expected = {
		on(0, 60), on(0, 64), on(0, 70), on(0, 62), off(10, 70), off(30, 62), off(80, 64), off(180, 60)};
	CHECK(player.heard == expected);
}

TEST_CASE("a pitch that started again sounds after a jump only while the later note does", "[unit]")
{
	// As when playing through: the note at 500 ends the one at 0, and from 600 the pitch is silent.
	Player player(chasing(chaseAll(), {note(0, 1000, 60), note(500, 600, 60)}), 16);
	player.locate(550);
	player.play(64);
	player.locate(700);
	player.play(64);
	CHECK(player.heard == std::vector<Heard>{on(0, 60), off(50, 60)});
}

TEST_CASE("a jump sends each chased controller's last value before it, in their order, then the notes", "[unit]")
{
	const uint32_t blockSize = GENERATE(1U, 16U, 64U);
	INFO("block size " << blockSize);
	const std::vector<RenderController> controllers = {control(10, 7, 50),
		controller(15, ControllerType::Program, 3, 0), controller(18, ControllerType::Control, 1, 9, 1),
		controller(20, ControllerType::PitchBend, 0, 1000), control(30, 7, 80),
		controller(40, ControllerType::ChannelPressure, 0, 5), control(120, 10, 5)};
	Player player(chasing(chaseAll(), {note(100, 200, 60), note(120, 130, 62)}, controllers), blockSize);
	player.locate(120);
	player.play(64);
	player.locate(25); // while playing: 60 ends first
	player.play(16);

	const std::vector<Heard> expected = {{0, Type::Program, 0, 3, 0, {}}, {0, Type::Controller, 1, 1, 9, {}},
		{0, Type::PitchBend, 0, 0, 1000, {}}, {0, Type::Controller, 0, 7, 80, {}},
		{0, Type::Controller, 0, 10, 5, {}}, // at the position itself: played, not chased
		on(0, 60), on(0, 62), off(10, 62), off(64, 60), {64, Type::Controller, 0, 7, 50, {}},
		{64, Type::Program, 0, 3, 0, {}}, {64, Type::Controller, 1, 1, 9, {}}, {64, Type::PitchBend, 0, 0, 1000, {}},
		{69, Type::Controller, 0, 7, 80, {}}, {79, Type::ChannelPressure, 0, 0, 5, {}}};
	CHECK(player.heard == expected);
}

TEST_CASE("only the chosen event types are chased", "[unit]")
{
	const std::vector<RenderController> controllers = {control(10, 7, 50),
		controller(15, ControllerType::Program, 3, 0), controller(20, ControllerType::PitchBend, 0, 1000),
		controller(25, ControllerType::ChannelPressure, 0, 5), controller(26, ControllerType::PolyPressure, 60, 5)};
	MidiChase chase;
	std::vector<Type> expected;
	SECTION("nothing")
	{
	}
	SECTION("notes")
	{
		chase.notes = true;
		expected = {Type::NoteOn};
	}
	SECTION("controllers")
	{
		chase.controllers = true;
		expected = {Type::Controller};
	}
	SECTION("program changes")
	{
		chase.programs = true;
		expected = {Type::Program};
	}
	SECTION("pitch bend")
	{
		chase.pitchBend = true;
		expected = {Type::PitchBend};
	}
	SECTION("everything: still no pressure")
	{
		chase = chaseAll();
		expected = {Type::Controller, Type::Program, Type::PitchBend, Type::NoteOn};
	}
	Player player(chasing(chase, {note(0, 100, 60)}, controllers), 16);
	player.locate(50);
	player.play(16);
	CHECK(typesAt(player.heard, 0) == expected);
}

TEST_CASE("chasing limited to parts takes only the part under the position", "[unit]")
{
	const std::vector<RenderController> controllers = {
		control(10, 7, 1), control(50, 1, 3), controller(110, ControllerType::Program, 4, 0), control(150, 7, 2)};
	const std::vector<RenderPart> parts = {{0, 100}, {100, 200}, {300, 400}};
	MidiChase chase = chaseAll();
	chase.beyondParts = false;
	Player player(chasing(chase, {}, controllers, parts), 16);
	player.locate(160);
	player.play(16);
	player.locate(250); // in no part
	player.play(16);
	player.locate(100); // at a part's start
	player.play(16);
	const std::vector<Heard> limited = {
		{0, Type::Program, 0, 4, 0, {}}, {0, Type::Controller, 0, 7, 2, {}}, {42, Type::Program, 0, 4, 0, {}}};
	CHECK(player.heard == limited);

	chase.beyondParts = true;
	Player beyond(chasing(chase, {}, controllers, parts), 16);
	beyond.locate(250);
	beyond.play(16);
	const std::vector<Heard> all = {
		{0, Type::Controller, 0, 1, 3, {}}, {0, Type::Program, 0, 4, 0, {}}, {0, Type::Controller, 0, 7, 2, {}}};
	CHECK(beyond.heard == all);
}

TEST_CASE("a locate while stopped chases the controllers but no notes", "[unit]")
{
	Player player(chasing(chaseAll(), {note(0, 500, 60)}, {control(10, 7, 1), control(100, 7, 2)}), 16);
	player.stop();
	player.locate(150);
	player.stop();   // frame 16
	player.stop();   // the same position: nothing
	player.play(16); // frame 48: starting playback chases again, with the notes
	player.stop();
	player.stop(); // where playback stopped: nothing
	const std::vector<Heard> expected = {
		{16, Type::Controller, 0, 7, 2, {}}, {48, Type::Controller, 0, 7, 2, {}}, on(48, 60), off(64, 60)};
	CHECK(player.heard == expected);
}

TEST_CASE("cycle playback chases at every jump to the left locator", "[unit]")
{
	const uint32_t blockSize = GENERATE(1U, 16U, 100U);
	INFO("block size " << blockSize);
	Player player(chasing(chaseAll(), {note(90, 300, 50)}, {control(50, 7, 1), control(150, 7, 2)}), blockSize);
	player.setCycle(100, 200);
	player.locate(100);
	player.play(200);
	const std::vector<Heard> expected = {{0, Type::Controller, 0, 7, 1, {}}, on(0, 50),
		{50, Type::Controller, 0, 7, 2, {}}, off(100, 50), {100, Type::Controller, 0, 7, 1, {}}, on(100, 50),
		{150, Type::Controller, 0, 7, 2, {}}};
	CHECK(player.heard == expected);
}

TEST_CASE("a jump into a full block chases in the next block that has room", "[unit]")
{
	Player player(chasing(chaseAll(), {note(0, 1000, 60), note(10, 1000, 62)}, {control(5, 7, 1)}), 16);
	player.play(16);
	fillInput(player);
	player.locate(500);
	player.play(16); // no room: the notes end in the next block, and it chases there
	player.play(16);
	player.stop();
	const std::vector<Heard> expected = {on(0, 60), {5, Type::Controller, 0, 7, 1, {}}, on(10, 62), off(32, 60),
		off(32, 62), {32, Type::Controller, 0, 7, 1, {}}, on(32, 60), on(32, 62), off(48, 60), off(48, 62)};
	std::vector<Heard> heard;
	std::copy_if(player.heard.begin(), player.heard.end(), std::back_inserter(heard),
		[](const Heard& h) { return h.frame < 16 || h.frame >= 32; }); // without the input that filled the block
	CHECK(heard == expected);
}

TEST_CASE("an instrument chases its MIDI tracks' controller events with its own, the latest of each kind", "[unit]")
{
	// The instrument track's own CC 7 is older than that of the MIDI track routed to it; at one position, the later
	// track's wins. Each kind of controller event is sent once.
	auto sequence = std::make_unique<MidiSequence>();
	const std::vector<RenderController> own = {control(10, 7, 100), control(40, 1, 1)};
	const std::vector<RenderController> routed = {control(40, 1, 2), control(50, 7, 30)};
	sequence->chase.push_back(makeChaseTrack(own, {}));
	sequence->chase.push_back(makeChaseTrack(routed, {}));
	auto instrument = std::make_shared<MidiSequencer>();
	instrument->setSequence(std::move(sequence));
	instrument->setChase(chaseAll());
	Player player(instrument, 16);
	player.locate(100);
	player.play(16);
	CHECK(player.heard == std::vector<Heard>{{0, Type::Controller, 0, 1, 2, {}}, {0, Type::Controller, 0, 7, 30, {}}});
}

TEST_CASE("a chase that doesn't fit into the event buffer goes on in the next blocks, each event once", "[unit]")
{
	SECTION("room for some")
	{
		std::vector<RenderController> controllers;
		controllers.reserve(10);
		for (int number = 0; number < 10; ++number)
			controllers.push_back(control(number, number, 1));
		Player player(chasing(chaseAll(), {}, controllers), 16);
		fillInputBut(player, 4);
		player.locate(100);
		player.play(32);
		std::vector<Heard> expected;
		expected.reserve(10);
		for (int number = 0; number < 10; ++number)
			expected.push_back({number < 4 ? 0 : 16, Type::Controller, 0, number, 1, {}});
		CHECK(ofType(player.heard, Type::Controller) == expected);
	}
	SECTION("more than a block holds")
	{
		// 16 channels of 128 controllers: two blocks; the note that starts at the position is chased in the third.
		std::vector<RenderController> controllers;
		for (int channel = 0; channel < 16; ++channel)
			for (int number = 0; number < 128; ++number)
			{
				RenderController c = control(channel, number, 1);
				c.channel = static_cast<uint8_t>(channel);
				controllers.push_back(c);
			}
		std::sort(controllers.begin(), controllers.end(),
			[](const RenderController& a, const RenderController& b) { return a.position < b.position; });
		Player player(chasing(chaseAll(), {note(100, 1000, 60)}, controllers), 16);
		player.locate(100);
		player.play(48);
		CHECK(ofType(player.heard, Type::Controller).size() == controllers.size());
		CHECK(notesIn(player.heard) == std::vector<Heard>{on(32, 60)});
	}
	SECTION("while stopped")
	{
		Player player(chasing(chaseAll(), {}, {control(1, 7, 1), control(2, 10, 2)}), 16);
		player.stop();
		fillInputBut(player, 1);
		player.locate(100);
		player.stop();
		player.stop();
		CHECK(ofType(player.heard, Type::Controller) ==
			std::vector<Heard>{{16, Type::Controller, 0, 7, 1, {}}, {32, Type::Controller, 0, 10, 2, {}}});
	}
}
