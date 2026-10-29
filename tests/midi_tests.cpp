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
