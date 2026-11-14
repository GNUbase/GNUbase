// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sequencer.h"
#include "engine/document/document.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/processors/midieffects.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "plugin/checkedplugin.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// 120 BPM at 48 kHz: a beat is 24000 samples, a sixteenth 6000.
	constexpr int64_t g_beat = 24000;
	constexpr int64_t g_beatTicks = 1720320;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	Json message(int64_t position, std::vector<int> bytes)
	{
		return Json{{"position", position}, {"bytes", std::move(bytes)}};
	}

	// An instrument track playing the note probe, with a part of four beats from 0.
	struct EffectTrack
	{
		explicit EffectTrack(int blockSize = 64) : engine(gnubase::audio::makeBackends())
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}});
			track = added.at("track").at("id").get<ObjectId>();
			part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_beatTicks}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
			run(client, "midi.capture", {{"start", true}});
		}

		// Adds notes to the part: position and length in beats, pitch and MIDI 1.0 velocity.
		void add(const std::vector<std::vector<double>>& notes)
		{
			Json list = Json::array();
			for (const std::vector<double>& note : notes)
				list.push_back({{"position", static_cast<int64_t>(note[0] * g_beatTicks)},
					{"length", static_cast<int64_t>(note[1] * g_beatTicks)}, {"pitch", static_cast<int>(note[2])},
					{"velocity", note[3] / 127.0}});
			run(client, "note.add", {{"part", part}, {"notes", std::move(list)}});
		}

		Json set(const char* section, Json rack, ObjectId on = 0)
		{
			return run(client, "channel.set", {{"channel", on == 0 ? channel : on}, {section, std::move(rack)}})
				.at("channel")
				.at(section);
		}

		void inject(std::vector<Json> messages)
		{
			run(client, "midi.inject", {{"channel", channel}, {"messages", std::move(messages)}});
		}

		// Renders in steps of at most a second, as audio.render takes.
		void render(int64_t frames)
		{
			for (; frames > 0; frames -= 48000)
				run(client, "audio.render", {{"frames", std::min<int64_t>(frames, 48000)}});
		}

		// What the instruments played since the last call, a line per message: its position and its bytes in hex.
		std::string captured() { return capturedBy()[0]; }

		// The same by instrument track channel, and all of it at 0.
		std::map<ObjectId, std::string> capturedBy()
		{
			std::map<ObjectId, std::string> texts;
			const Json capture = run(client, "midi.capture");
			for (const Json& c : capture.at("captured"))
			{
				std::string text = std::to_string(c.at("position").get<int64_t>());
				for (const int byte : c.at("bytes").get<std::vector<int>>())
				{
					char hex[4] = {};
					std::snprintf(hex, sizeof hex, " %02x", byte);
					text += hex;
				}
				text += "\n";
				texts[0] += text;
				texts[c.at("channel").get<ObjectId>()] += text;
			}
			return texts;
		}

		// Adds an instrument track playing the note probe; returns its channel.
		ObjectId addInstrument()
		{
			const ObjectId added =
				run(client, "track.add", {{"type", "instrument"}}).at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", added}, {"instrument", "noteProbe"}});
			return added;
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
		ObjectId track = 0;
		ObjectId part = 0;
	};

	Json slot(const char* effect, Json parameters = Json::object())
	{
		return {{"effect", effect}, {"parameters", std::move(parameters)}};
	}

	void checkGolden(const std::string& name, const std::string& text)
	{
		const std::string report = gnubase::test::compareTextWithGolden(name, text);
		INFO(report);
		CHECK(report.empty());
	}

	// C4, E4 and G4 played into the track at frame 0 in the order E, C, G, released at frame 48000.
	void playChord(EffectTrack& track)
	{
		track.inject({message(0, {0x90, 64, 100}), message(0, {0x90, 60, 80}), message(0, {0x90, 67, 90}),
			message(48000, {0x80, 64, 64}), message(48000, {0x80, 60, 64}), message(48000, {0x80, 67, 64})});
	}
}

TEST_CASE("midiEffect.list lists the built-in MIDI effects with their parameters", "[integration]")
{
	EffectTrack track;
	const Json effects = run(track.client, "midiEffect.list").at("effects");
	REQUIRE(effects.size() == 3);
	CHECK(effects[0].at("id") == "arpeggiator");
	CHECK(effects[1].at("id") == "chordGenerator");
	CHECK(effects[2].at("id") == "transformer");
	const Json& mode = effects[0].at("parameters")[0];
	CHECK(mode.at("key") == "mode");
	CHECK(mode.at("options") == Json{"Up", "Down", "Up/Down", "As played"});
	for (const processors::BuiltinInfo& info : processors::midiEffects())
		for (size_t i = 0; i < info.parameters.size(); ++i)
			CHECK(info.parameters[i].id == i);
}

TEST_CASE("the sequencer remembers the automated values of every MIDI effect's parameters", "[unit]")
{
	// A parameter beyond them would play its static value in a new instance of its effect.
	for (const processors::BuiltinInfo& info : processors::midiEffects())
		for (const processors::ParameterInfo& parameter : info.parameters)
		{
			INFO(info.id << " " << parameter.name);
			CHECK(parameter.id < MidiSequencer::g_automated);
		}
}

TEST_CASE("the arpeggiator plays the held notes in each mode at its rate, gate and octaves", "[integration]")
{
	std::string text;
	for (const char* mode : {"Up", "Down", "Up/Down", "As played"})
	{
		EffectTrack track;
		track.set("midiInserts",
			{{"slots", {slot("arpeggiator", {{"mode", mode}, {"rate", "1/16"}, {"gate", 50}, {"octaves", 2}})}}});
		playChord(track);
		track.render(50000);
		text += std::string("# ") + mode + "\n" + track.captured();
	}
	checkGolden("midi-effect-arpeggiator", text);
}

TEST_CASE("the chord generator plays a chord for every note, shared pitches once", "[integration]")
{
	EffectTrack track;
	track.set("midiInserts", {{"slots", {slot("chordGenerator", {{"chord", "Major"}})}}});
	track.inject({message(100, {0x90, 60, 100})});
	track.render(1000);
	// A new chord while C sounds: its note-off still ends the major chord, and E minor shares E and G with it.
	track.set("midiInserts", {{"slots", {slot("chordGenerator", {{"chord", "Minor"}})}}});
	track.inject({message(1100, {0x90, 64, 90}), message(1200, {0x80, 60, 64}), message(1300, {0x80, 64, 64}),
		message(1400, {0x90, 127, 70}), message(1500, {0x80, 127, 64})});
	track.render(1000);
	checkGolden("midi-effect-chord-generator", track.captured());
}

TEST_CASE(
	"the transformer changes the notes that meet its condition, note-offs where their note-ons went", "[integration]")
{
	std::string text;
	const std::vector<std::pair<const char*, Json>> settings = {
		{"velocity below 64 up an octave",
			{{"condition", "Velocity"}, {"maximum", 63}, {"action", "Transpose"}, {"value", 12}}},
		{"pitch from 62 deleted", {{"condition", "Pitch"}, {"minimum", 62}, {"action", "Delete"}}},
		{"all set to velocity 30", {{"action", "Set velocity"}, {"value", 30}}},
		{"all 20 louder", {{"action", "Add velocity"}, {"value", 20}}},
		{"channel 1 to channel 3", {{"condition", "Channel"}, {"maximum", 1}, {"action", "Set channel"}, {"value", 3}}},
	};
	for (const auto& [name, parameters] : settings)
	{
		EffectTrack track;
		track.set("midiInserts", {{"slots", {slot("transformer", parameters)}}});
		track.inject({message(10, {0x90, 60, 50}), message(20, {0x91, 62, 120}), message(30, {0xA0, 60, 40}),
			message(40, {0xB0, 1, 99}), message(50, {0x80, 60, 64}), message(60, {0x81, 62, 64})});
		track.render(128);
		text += std::string("# ") + name + "\n" + track.captured();
	}
	checkGolden("midi-effect-transformer", text);
}

TEST_CASE("inserts run in order on the part's notes, and sends play beside them, before or after the inserts",
	"[integration]")
{
	std::string text;
	for (const bool pre : {false, true})
	{
		EffectTrack track;
		track.add({{0, 1, 60, 100}, {2, 1, 62, 80}});
		track.set("midiInserts",
			{{"slots",
				{slot("chordGenerator", {{"chord", "Major"}}),
					slot("arpeggiator", {{"rate", "1/8"}, {"gate", 100}})}}});
		track.set(
			"midiSends", {{"pre", pre}, {"slots", {slot("transformer", {{"action", "Transpose"}, {"value", 24}})}}});
		run(track.client, "transport.play");
		track.render(4 * g_beat);
		text += std::string("# sends ") + (pre ? "pre" : "post") + "\n" + track.captured();
	}
	checkGolden("midi-effect-inserts-and-sends", text);
}

TEST_CASE("slots that are off, a bypassed section and an empty slot play the events as they are", "[integration]")
{
	EffectTrack plain;
	plain.inject({message(10, {0x90, 60, 100}), message(30, {0x80, 60, 64})});
	plain.render(64);
	const std::string expected = plain.captured();

	const Json transpose = slot("transformer", {{"value", 7}});
	for (const Json& rack : {Json{{"slots", {{{"effect", "transformer"}, {"on", false}}}}},
			 Json{{"bypass", true}, {"slots", {transpose}}}, Json{{"slots", {slot("")}}}})
	{
		EffectTrack track;
		track.set("midiInserts", rack);
		track.set("midiSends", rack);
		track.inject({message(10, {0x90, 60, 100}), message(30, {0x80, 60, 64})});
		track.render(64);
		CHECK(track.captured() == expected);
	}
}

TEST_CASE("an arpeggio plays the same for every block size", "[integration]")
{
	std::string first;
	for (const int blockSize : {64, 999})
	{
		EffectTrack track(blockSize);
		track.add({{0, 2, 60, 100}, {0, 2, 64, 100}, {1, 2, 67, 100}});
		track.set("midiInserts", {{"slots", {slot("arpeggiator", {{"mode", "Up/Down"}, {"gate", 30}})}}});
		run(track.client, "transport.play");
		track.render(4 * g_beat);
		const std::string text = track.captured();
		if (first.empty())
			first = text;
		CHECK(text == first);
	}
	CHECK(!first.empty());
}

TEST_CASE("changing the effects while their notes sound ends those notes", "[integration]")
{
	EffectTrack track;
	track.set("midiInserts", {{"slots", {slot("chordGenerator")}}});
	track.inject({message(10, {0x90, 60, 100})});
	track.render(64);
	track.set("midiInserts", {{"slots", {slot("")}}});
	track.render(64);
	track.inject({message(200, {0x80, 60, 64})});
	track.render(128);
	CHECK(track.captured() ==
		"10 90 3c 64\n10 90 40 64\n10 90 43 64\n64 80 3c 00\n64 80 40 00\n64 80 43 00\n"
		"200 80 3c 40\n");
}

TEST_CASE("a MIDI track's effects apply to its events, the instrument track's only to its own", "[integration]")
{
	EffectTrack track;
	const ObjectId midi = run(track.client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	run(track.client, "channel.set", {{"channel", midi}, {"output", track.channel}});
	track.set("midiInserts", {{"slots", {slot("transformer", {{"value", 12}})}}});
	track.set("midiInserts", {{"slots", {slot("transformer", {{"value", 1}})}}}, midi);
	run(track.client, "midi.inject", {{"channel", midi}, {"messages", {message(10, {0x90, 60, 100})}}});
	track.inject({message(20, {0x90, 60, 100})});
	track.render(64);
	CHECK(track.captured() == "10 90 3d 64\n20 90 48 64\n");
}

TEST_CASE("a MIDI track routed away from an instrument ends the notes its effects played there", "[integration]")
{
	EffectTrack track;
	const ObjectId midi = run(track.client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	run(track.client, "channel.set", {{"channel", midi}, {"output", track.channel}});
	track.set("midiInserts", {{"slots", {slot("chordGenerator", {{"chord", "Major"}})}}}, midi);
	run(track.client, "midi.inject", {{"channel", midi}, {"messages", {message(10, {0x90, 60, 100})}}});
	track.render(64);
	run(track.client, "channel.set", {{"channel", midi}, {"output", 0}});
	track.render(64);
	CHECK(track.captured() == "10 90 3c 64\n10 90 40 64\n10 90 43 64\n64 80 3c 00\n64 80 40 00\n64 80 43 00\n");
}

TEST_CASE("the arpeggiator returns when its output is full, and ends its note once there is room", "[unit]")
{
	using namespace gnubase::engine::graph;
	EventBuffer in(4);
	EventBuffer out(1);
	TransportBlock transport;
	transport.tempo = 120.0;
	const std::shared_ptr<processors::MidiEffect> arpeggiator = processors::createMidiEffect("arpeggiator");
	arpeggiator->prepareFor(48000.0, 48000);
	for (const int key : {60, 64})
	{
		ProcessEvent note;
		note.type = ProcessEvent::Type::NoteOn;
		note.key = static_cast<uint8_t>(key);
		note.velocity = 0x8000;
		in.add(note);
	}
	// A sixteenth is 6000 frames: a block of a second holds eight steps, each with a note-on and a note-off.
	arpeggiator->process({nullptr, 0, 48000, in, out, transport});
	REQUIRE(out.size() == 1);
	CHECK(out.begin()->type == ProcessEvent::Type::NoteOn);
	CHECK(out.begin()->key == 60);
	in.clear();
	out.clear();
	arpeggiator->process({nullptr, 0, 48000, in, out, transport});
	REQUIRE(out.size() == 1);
	CHECK(out.begin()->type == ProcessEvent::Type::NoteOff);
	CHECK(out.begin()->key == 60);
	CHECK(out.begin()->offset == 0);
	for (int block = 0; block < 4; ++block)
	{
		out.clear();
		arpeggiator->process({nullptr, 0, 48000, in, out, transport});
		CHECK(out.size() == 1);
	}
}

namespace
{
	using graph::EventBuffer;
	using graph::ProcessEvent;

	ProcessEvent noteAt(ProcessEvent::Type type, uint32_t offset, int key, uint8_t channel = 0)
	{
		ProcessEvent note;
		note.offset = offset;
		note.type = type;
		note.channel = channel;
		note.key = static_cast<uint8_t>(key);
		note.velocity = 0x8000;
		return note;
	}

	// Follows out's notes in held, as channel * 128 + pitch.
	void follow(const EventBuffer& out, std::vector<int>& held)
	{
		for (const ProcessEvent& e : out)
		{
			const int index = (e.channel * 128) + e.key;
			if (e.type == ProcessEvent::Type::NoteOn)
				held.push_back(index);
			else if (e.type == ProcessEvent::Type::NoteOff)
				held.erase(std::remove(held.begin(), held.end(), index), held.end());
		}
	}

	// The notes out still holds after its events, as channel * 128 + pitch.
	std::vector<int> hanging(const EventBuffer& out)
	{
		std::vector<int> held;
		follow(out, held);
		return held;
	}
}

TEST_CASE("the arpeggiator skips the steps it missed with its output full instead of playing them at once", "[unit]")
{
	EventBuffer in(4);
	EventBuffer full(1);
	EventBuffer out(64);
	graph::TransportBlock transport;
	transport.tempo = 120.0;
	const std::shared_ptr<processors::MidiEffect> arpeggiator = processors::createMidiEffect("arpeggiator");
	arpeggiator->prepareFor(48000.0, 48000);
	in.add(noteAt(ProcessEvent::Type::NoteOn, 0, 60));
	in.add(noteAt(ProcessEvent::Type::NoteOn, 0, 64));
	// Eight steps of a sixteenth in a second; only the first note-on fits.
	arpeggiator->process({nullptr, 0, 48000, in, full, transport});
	in.clear();
	arpeggiator->process({nullptr, 0, 48000, in, out, transport});
	// The first note ends at once; the steps of the first block are gone, those of this one play on time.
	REQUIRE(out.size() == 17);
	CHECK(out[0].type == ProcessEvent::Type::NoteOff);
	CHECK(out[0].offset == 0);
	for (size_t i = 1; i < out.size(); i += 2)
	{
		CHECK(out[i].type == ProcessEvent::Type::NoteOn);
		CHECK(out[i].offset == (i / 2) * 6000);
		CHECK(out[i + 1].type == ProcessEvent::Type::NoteOff);
		CHECK(out[i + 1].offset == ((i / 2) * 6000) + 3000);
	}
}

TEST_CASE("an arpeggio follows a tempo ramp step by step, the same for every block size", "[integration]")
{
	std::string first;
	for (const int blockSize : {64, 999})
	{
		EffectTrack track(blockSize);
		run(track.client, "project.set", {{"tempoTrackActive", true}});
		run(track.client, "tempo.add", {{"position", 4 * g_beatTicks}, {"bpm", 240.0}, {"ramp", true}});
		track.add({{0, 4, 60, 100}, {0, 4, 64, 100}});
		track.set("midiInserts", {{"slots", {slot("arpeggiator", {{"rate", "1/32"}})}}});
		run(track.client, "transport.play");
		track.render(3 * g_beat);
		const std::string text = track.captured();
		if (first.empty())
			first = text;
		CHECK(text == first);
	}
	// The steps get shorter as the tempo rises: from 120 BPM to about 210 after three beats.
	std::vector<int64_t> starts;
	for (size_t line = 0; line < first.size(); line = first.find('\n', line) + 1)
		if (first.compare(first.find(' ', line), 3, " 90") == 0)
			starts.push_back(std::stoll(first.substr(line, first.find(' ', line) - line)));
	REQUIRE(starts.size() > 10);
	const int64_t firstStep = starts[1] - starts[0];
	const int64_t lastStep = starts.back() - starts[starts.size() - 2];
	CHECK(firstStep == 3000);
	CHECK(lastStep < firstStep * 3 / 4);
	CHECK(lastStep > firstStep / 2);
}

TEST_CASE("the chord generator and the transformer give note-offs room in a full output", "[unit]")
{
	EventBuffer in(4);
	const graph::TransportBlock transport;
	SECTION("chord generator")
	{
		// Three note-ons fill all but one place; the note-offs take the places of note-ons and the controller.
		EventBuffer out(4);
		const std::shared_ptr<processors::MidiEffect> chords = processors::createMidiEffect("chordGenerator");
		in.add(noteAt(ProcessEvent::Type::NoteOn, 0, 60));
		in.add(noteAt(ProcessEvent::Type::Controller, 5, 1));
		in.add(noteAt(ProcessEvent::Type::NoteOff, 10, 60));
		chords->process({nullptr, 0, 64, in, out, transport});
		CHECK(hanging(out).empty());
		CHECK(out[out.size() - 1].type == ProcessEvent::Type::NoteOff);
	}
	SECTION("transformer")
	{
		EventBuffer out(2);
		const std::shared_ptr<processors::MidiEffect> transformer = processors::createMidiEffect("transformer");
		transformer->setParameter(processors::Transformer::Value, 12);
		in.add(noteAt(ProcessEvent::Type::NoteOn, 0, 60));
		in.add(noteAt(ProcessEvent::Type::Controller, 5, 1));
		in.add(noteAt(ProcessEvent::Type::NoteOff, 10, 60));
		transformer->process({nullptr, 0, 64, in, out, transport});
		REQUIRE(out.size() == 2);
		CHECK(out[0].type == ProcessEvent::Type::NoteOn);
		CHECK(out[1].type == ProcessEvent::Type::NoteOff);
		CHECK(out[1].key == 72);
	}
}

TEST_CASE("the transformer ends a note two notes share when the last of them ends", "[integration]")
{
	// Set channel: C4 on channels 1 and 2 both play on channel 3, once, until both are released.
	EffectTrack track;
	track.set("midiInserts", {{"slots", {slot("transformer", {{"action", "Set channel"}, {"value", 3}})}}});
	track.inject({message(10, {0x90, 60, 100}), message(20, {0x91, 60, 90}), message(30, {0x80, 60, 64}),
		message(40, {0x81, 60, 64})});
	track.render(64);
	CHECK(track.captured() == "10 92 3c 64\n40 82 3c 40\n");
	// Transpose by pitch: C4 up an octave meets the C5 that passes.
	EffectTrack transpose;
	transpose.set(
		"midiInserts", {{"slots", {slot("transformer", {{"condition", "Pitch"}, {"maximum", 60}, {"value", 12}})}}});
	transpose.inject({message(10, {0x90, 72, 100}), message(20, {0x90, 60, 90}), message(30, {0x80, 72, 64}),
		message(40, {0x80, 60, 64})});
	transpose.render(64);
	CHECK(transpose.captured() == "10 90 48 64\n40 80 48 40\n");
}

TEST_CASE("sends play the notes their effects make, the track's other events reach the output once", "[integration]")
{
	for (const bool pre : {false, true})
	{
		EffectTrack track;
		track.set("midiSends",
			{{"pre", pre}, {"slots", {slot("transformer", {{"value", 12}}), slot("transformer", {{"value", 24}})}}});
		track.inject({message(10, {0x90, 60, 100}), message(20, {0xB0, 1, 99}), message(25, {0xE0, 0, 80}),
			message(30, {0xA0, 60, 40}), message(40, {0x80, 60, 64})});
		track.render(64);
		INFO("pre " << pre);
		CHECK(track.captured() ==
			"10 90 3c 64\n10 90 48 64\n10 90 54 64\n20 b0 01 63\n25 e0 00 50\n30 a0 3c 28\n30 a0 48 28\n"
			"30 a0 54 28\n40 80 3c 40\n40 80 48 40\n40 80 54 40\n");
	}
}

TEST_CASE(
	"channel.set changes a track's MIDI inserts and sends, which project.get shows and undo restores", "[integration]")
{
	EffectTrack track;
	const Json channel = run(track.client, "project.get").at("channels")[0];
	const Json empty = {{"effect", ""}, {"on", true}, {"parameters", Json::object()}};
	Json send = empty;
	send["output"] = 0;
	CHECK(
		channel.at("midiInserts") == Json{{"bypass", false}, {"divider", 0}, {"slots", {empty, empty, empty, empty}}});
	CHECK(channel.at("midiSends") == Json{{"bypass", false}, {"pre", false}, {"slots", {send, send, send, send}}});

	Json set = track.set("midiInserts", {{"slots", {Json::object(), slot("arpeggiator", {{"octaves", 3}})}}});
	CHECK(set.at("slots")[0] == empty);
	CHECK(set.at("slots")[1] ==
		Json{{"effect", "arpeggiator"}, {"on", true},
			{"parameters", {{"mode", 0}, {"rate", 2}, {"gate", 50.0}, {"octaves", 3}}}});
	// Only what it has changes; another effect starts from its defaults.
	set = track.set("midiInserts", {{"bypass", true}, {"slots", {Json::object(), {{"parameters", {{"mode", 1}}}}}}});
	CHECK(set.at("bypass") == true);
	CHECK(set.at("slots")[1].at("parameters") == Json{{"mode", 1}, {"rate", 2}, {"gate", 50.0}, {"octaves", 3}});
	set = track.set("midiInserts", {{"slots", {Json::object(), {{"effect", "chordGenerator"}, {"on", false}}}}});
	CHECK(set.at("slots")[1] == Json{{"effect", "chordGenerator"}, {"on", false}, {"parameters", {{"chord", 0}}}});
	CHECK(run(track.client, "project.get").at("channels")[0].at("midiInserts") == set);
	CHECK(track.set("midiSends", {{"pre", true}}).at("pre") == true);
	CHECK(track.set("midiInserts", {{"divider", 2}}).at("divider") == 2);

	run(track.client, "history.undo");
	run(track.client, "history.undo");
	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("channels")[0].at("midiInserts").at("slots")[1].at("effect") ==
		"arpeggiator");
	run(track.client, "history.undo");
	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("channels")[0] == channel);
}

TEST_CASE("channel.set refuses unknown MIDI effects, parameters out of range and tracks without notes", "[integration]")
{
	EffectTrack track;
	const auto refused = [&track](Json rack)
	{ return reject(track.client, "channel.set", {{"channel", track.channel}, {"midiInserts", std::move(rack)}}); };
	CHECK_THAT(refused({{"slots", {slot("chorus")}}}), ContainsSubstring("midiInserts.slots[0].effect"));
	CHECK_THAT(refused({{"slots", {slot("arpeggiator", {{"octaves", 5}})}}}), ContainsSubstring("1 to 4"));
	CHECK_THAT(refused({{"slots", {slot("arpeggiator", {{"mode", "Sideways"}})}}}),
		ContainsSubstring("midiInserts.slots[0].parameters.mode"));
	CHECK_THAT(refused({{"slots", {slot("arpeggiator", {{"tempo", 1}})}}}), ContainsSubstring("no parameter tempo"));
	CHECK_THAT(refused({{"slots", {{{"parameters", {{"mode", 1}}}}}}}), ContainsSubstring("no effect"));
	CHECK_THAT(refused({{"slots", Json::array({slot(""), slot(""), slot(""), slot(""), slot("")})}}),
		ContainsSubstring("at most 4"));
	CHECK_THAT(refused({{"pre", true}}), ContainsSubstring("midiInserts.pre"));
	CHECK_THAT(refused({{"divider", 5}}), ContainsSubstring("0 to 4"));
	CHECK_THAT(refused({{"slots", {{{"output", track.channel}}}}}), ContainsSubstring("only the MIDI sends"));
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", track.channel}, {"midiSends", {{"divider", 1}}}}),
		ContainsSubstring("only the MIDI inserts"));
	const ObjectId midi = run(track.client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	Json sent = slot("transformer");
	sent["output"] = midi;
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", track.channel}, {"midiSends", {{"slots", {sent}}}}}),
		ContainsSubstring("instrument track's channel"));
	// Two instrument tracks can't send to each other.
	const ObjectId other = track.addInstrument();
	sent["output"] = other;
	track.set("midiSends", {{"slots", {sent}}});
	sent["output"] = track.channel;
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", other}, {"midiSends", {{"slots", {sent}}}}}),
		ContainsSubstring("loop"));
	CHECK_THAT(refused(5), ContainsSubstring("midiInserts must be object"));

	const ObjectId audio = run(track.client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", audio}, {"midiSends", {{"bypass", true}}}}),
		ContainsSubstring("only MIDI and instrument tracks"));
}

TEST_CASE("the MIDI inserts and sends are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "song.dawproject").u8string();
	EffectTrack track;
	const Json inserts = track.set("midiInserts",
		{{"bypass", true},
			{"slots", {slot("arpeggiator", {{"gate", 75.5}}), {{"effect", "transformer"}, {"on", false}}}}});
	const ObjectId other = track.addInstrument();
	Json sent = slot("chordGenerator");
	sent["output"] = other;
	const Json sends = track.set("midiSends", {{"pre", true}, {"slots", {Json::object(), sent}}});
	CHECK(sends.at("slots")[1].at("output") == other);
	track.set("midiInserts", {{"divider", 1}});
	run(track.client, "project.save", {{"path", file}});
	run(track.client, "project.new");
	run(track.client, "project.load", {{"path", file}});
	const Json channel = run(track.client, "project.get").at("channels")[0];
	Json divided = inserts;
	divided["divider"] = 1;
	CHECK(channel.at("midiInserts") == divided);
	CHECK(channel.at("midiSends") == sends);
}

TEST_CASE("a controller the rules turn into a note shares it with a held note and ends in a full output", "[unit]")
{
	using midi::LogicalTarget;
	// The sustain pedal plays C1, as a bass drum: a note-on while it is down, a note-off when it comes up.
	const std::array<midi::FilterLine, 1> filters = {{{0, LogicalTarget::Type, midi::LogicalCondition::Equal,
		static_cast<double>(midi::LogicalType::Controller), 0.0, 0, midi::LogicalBool::And}}};
	const std::array<midi::ActionLine, 2> actions = {
		{{LogicalTarget::Type, midi::LogicalOperation::SetFixed, static_cast<double>(midi::LogicalType::Note), 0.0},
			{LogicalTarget::Subtype, midi::LogicalOperation::SetFixed, 36.0, 0.0}}};
	const processors::LogicalRulesView rules{
		filters.data(), filters.size(), midi::LogicalFunction::Transform, actions.data(), actions.size()};
	const auto pedal = [](uint32_t offset, bool down)
	{
		ProcessEvent event = noteAt(ProcessEvent::Type::Controller, offset, 64);
		event.value = down ? 0xFFFFFFFFU : 0U;
		return event;
	};

	// Held by the key and by the pedal, C1 plays once and ends with the last of them.
	processors::LogicalStage stage;
	EventBuffer out(8);
	stage.handle(noteAt(ProcessEvent::Type::NoteOn, 0, 36), rules, out);
	stage.handle(pedal(1, true), rules, out);
	stage.handle(pedal(2, true), rules, out); // a pedal moved further down plays nothing new
	stage.handle(pedal(3, false), rules, out);
	CHECK(hanging(out) == std::vector<int>{36});
	CHECK(out.size() == 1);
	stage.handle(noteAt(ProcessEvent::Type::NoteOff, 4, 36), rules, out);
	CHECK(hanging(out).empty());
	CHECK(out.size() == 2);

	// The pedal's note-off takes the place of another event in a full output.
	processors::LogicalStage full;
	EventBuffer one(1);
	full.handle(pedal(0, true), rules, one);
	REQUIRE(one.size() == 1);
	full.handle(pedal(1, false), rules, one);
	REQUIRE(one.size() == 1);
	CHECK(one[0].type == ProcessEvent::Type::NoteOff);
	CHECK(one[0].key == 36);
}

TEST_CASE("sends before the modifiers get the parts' notes and the input as they are", "[integration]")
{
	std::string text;
	for (const bool pre : {false, true})
	{
		EffectTrack track;
		track.add({{0, 0.5, 60, 100}});
		run(track.client, "channel.set", {{"channel", track.channel}, {"modifiers", {{"transpose", 12}}}});
		track.set("midiSends", {{"pre", pre}, {"slots", {slot("transformer", {{"value", 1}})}}});
		run(track.client, "transport.play");
		track.inject({message(100, {0x90, 64, 100}), message(200, {0x80, 64, 64})});
		track.render(g_beat);
		text += std::string("# sends ") + (pre ? "pre" : "post") + "\n" + track.captured();
	}
	CHECK(text ==
		"# sends post\n0 90 48 64\n0 90 49 64\n100 90 4c 64\n100 90 4d 64\n200 80 4c 40\n200 80 4d 40\n"
		"12000 80 48 00\n12000 80 49 00\n"
		"# sends pre\n0 90 48 64\n0 90 3d 64\n100 90 4c 64\n100 90 41 64\n200 80 4c 40\n200 80 41 40\n"
		"12000 80 48 00\n12000 80 3d 00\n");
}

TEST_CASE("a send before the modifiers ends its notes when the modifiers stop changing them", "[integration]")
{
	EffectTrack track;
	track.add({{0, 2, 60, 100}});
	run(track.client, "channel.set", {{"channel", track.channel}, {"modifiers", {{"transpose", 12}}}});
	track.set("midiSends", {{"pre", true}, {"slots", {slot("arpeggiator")}}});
	run(track.client, "transport.play");
	track.render(g_beat);
	CHECK(track.captured().find(" 90 3c ") != std::string::npos); // the arpeggio plays the unmodified note
	// The send takes the track's own events now: an arpeggiator that kept the note it holds would never get its
	// note-off and play on.
	run(track.client, "channel.set", {{"channel", track.channel}, {"modifiers", {{"transpose", 0}}}});
	track.render(2 * g_beat);
	const std::string played = track.captured();
	std::string late; // note-ons after the part's note ended
	for (size_t line = 0; line < played.size(); line = played.find('\n', line) + 1)
		if (std::stoll(played.substr(line)) >= 2 * g_beat && played.compare(played.find(' ', line), 4, " 90 ") == 0)
			late += played.substr(line, played.find('\n', line) + 1 - line);
	CHECK(late.empty());
}

TEST_CASE("a send's buffer is there whenever it has an output, also while it doesn't play", "[unit]")
{
	MidiSequencer sequencer;
	auto sequence = std::make_unique<MidiSequence>();
	sequence->sends.bypass = true;
	sequence->sendOutputs[1] = 7;
	sequencer.setSequence(std::move(sequence));
	CHECK(sequencer.sendOutput(1).capacity() != 0);
}

TEST_CASE("a send plays its effect to another instrument track, with the track's other events", "[integration]")
{
	EffectTrack track;
	const ObjectId other = track.addInstrument();
	Json sent = slot("transformer", {{"value", 12}});
	sent["output"] = other;
	track.set("midiSends", {{"slots", {sent}}});
	track.inject({message(10, {0x90, 60, 100}), message(20, {0xB0, 64, 127}), message(25, {0xE0, 0, 80}),
		message(30, {0x80, 60, 64}), message(40, {0x90, 62, 100})});
	track.render(64);
	std::map<ObjectId, std::string> played = track.capturedBy();
	CHECK(played[track.channel] == "10 90 3c 64\n20 b0 40 7f\n25 e0 00 50\n30 80 3c 40\n40 90 3e 64\n");
	CHECK(played[other] == "10 90 48 64\n20 b0 40 7f\n25 e0 00 50\n30 80 48 40\n40 90 4a 64\n");

	// Back to the track's output: the note it played there ends there; the track's own note sounds on (#1157).
	sent["output"] = 0;
	track.set("midiSends", {{"slots", {sent}}});
	track.render(64);
	played = track.capturedBy();
	CHECK(played[other] == "64 80 4a 00\n");
	CHECK(played[track.channel].empty());
}

TEST_CASE(
	"a send to another instrument track ends its notes there when it goes, and its track when removed", "[integration]")
{
	EffectTrack track;
	const ObjectId other = track.addInstrument();
	Json sent = slot("chordGenerator", {{"chord", "Power"}});
	sent["output"] = other;
	track.set("midiSends", {{"slots", {sent}}});
	track.inject({message(10, {0x90, 60, 100})});
	track.render(64);
	CHECK(track.capturedBy()[other] == "10 90 3c 64\n10 90 43 64\n");
	// Bypassed, the send goes: the instrument there ends what it played.
	track.set("midiSends", {{"bypass", true}});
	track.render(64);
	CHECK(track.capturedBy()[other] == "64 80 3c 00\n64 80 43 00\n");

	// Removing the track it plays to clears its output.
	const auto channelOf = [&track](ObjectId id)
	{
		const Json project = run(track.client, "project.get");
		for (const Json& channel : project.at("channels"))
			if (channel.at("id") == id)
				return channel;
		return Json();
	};
	run(track.client, "track.remove", {{"track", channelOf(other).at("track")}});
	CHECK(channelOf(track.channel).at("midiSends").at("slots")[0].at("output") == 0);
}

TEST_CASE("automation plays a MIDI effect's parameters at their samples, and parameter.list has them", "[integration]")
{
	EffectTrack track;
	const ObjectId other = track.addInstrument();
	track.add({{0, 0.25, 60, 100}, {2, 0.25, 60, 100}, {3, 0.25, 60, 100}});
	track.set("midiInserts", {{"slots", {Json::object(), slot("transformer")}}});
	const Json target = {{"type", "midiEffect"}, {"number", 1}, {"parameter", processors::Transformer::Value}};
	const auto lane = run(track.client, "automationLane.add", {{"track", track.track}, {"target", target}})
						  .at("lane")
						  .at("id")
						  .get<ObjectId>();
	run(track.client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", 0}, {"shape", "step"}},
					{{"position", 2 * g_beatTicks}, {"value", 12}, {"shape", "step"}}}}});
	run(track.client, "transport.play");
	track.render((5 * g_beat) / 2);
	// An unrelated edit gives the track a new sequence, but the effect keeps its automated value.
	run(track.client, "channel.set", {{"channel", other}, {"recordEnable", true}});
	track.render(g_beat);
	// The second note plays where the value has changed, its note-off where its note-on went.
	CHECK(track.captured() ==
		"0 90 3c 64\n6000 80 3c 00\n48000 90 48 64\n54000 80 48 00\n72000 90 48 64\n78000 80 48 00\n");

	// parameter.list lists the effects' parameters; parameter.set sets the slot's.
	const Json parameters = run(track.client, "parameter.list", {{"channel", track.channel}}).at("parameters");
	const auto found = std::find_if(parameters.begin(), parameters.end(),
		[&target](const Json& parameter) { return parameter.at("target") == target; });
	REQUIRE(found != parameters.end());
	CHECK(found->at("name") == "Value");
	run(track.client, "parameter.set", {{"channel", track.channel}, {"target", target}, {"value", 5}});
	CHECK(run(track.client, "project.get")
			  .at("channels")[0]
			  .at("midiInserts")
			  .at("slots")[1]
			  .at("parameters")
			  .at("value") == 5);

	// Only slots with an effect, and only their parameters.
	CHECK_THAT(reject(track.client, "automationLane.add",
				   {{"track", track.track}, {"target", {{"type", "midiEffect"}, {"number", 0}, {"parameter", 0}}}}),
		ContainsSubstring("holds no effect"));
	CHECK_THAT(reject(track.client, "automationLane.add",
				   {{"track", track.track}, {"target", {{"type", "midiEffect"}, {"number", 1}, {"parameter", 9}}}}),
		ContainsSubstring("no parameter 9"));

	// Another effect in the slot: the lane goes with the transformer, and comes back with undo.
	track.set("midiInserts", {{"slots", {Json::object(), slot("chordGenerator")}}});
	CHECK(run(track.client, "project.get").at("automation").empty());
	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("automation").size() == 1);
}

TEST_CASE("an effect after a changed insert starts afresh, without the notes the old chain gave it", "[integration]")
{
	EffectTrack track;
	track.set("midiInserts",
		{{"slots", {slot("chordGenerator", {{"chord", "Major"}}), slot("arpeggiator", {{"rate", "1/16"}})}}});
	run(track.client, "transport.play");
	track.inject({message(10, {0x90, 60, 100})});
	track.render(g_beat);
	// The chord generator goes while the note sounds: an arpeggiator that kept the chord's notes would step on
	// through them after the note ends.
	track.set("midiInserts", {{"slots", {slot("")}}});
	track.render(g_beat);
	track.inject({message(2 * g_beat, {0x80, 60, 64})});
	track.render(2 * g_beat);
	const std::string played = track.captured();
	for (size_t line = 0; line < played.size(); line = played.find('\n', line) + 1)
		if (std::stoll(played.substr(line)) > 2 * g_beat)
			CHECK(played.compare(played.find(' ', line), 4, " 90 ") != 0);
}

TEST_CASE("a send whose source changes ends its own notes only, not the track's", "[integration]")
{
	EffectTrack track;
	track.add({{0, 2, 60, 100}});
	track.set("midiSends", {{"pre", true}, {"slots", {slot("transformer", {{"value", 12}})}}});
	run(track.client, "transport.play");
	track.render(g_beat);
	CHECK(track.captured() == "0 90 3c 64\n0 90 48 64\n");
	// Switching Pre gives the send a new instance: its note ends, the part's note sounds on to its end.
	track.set("midiSends", {{"pre", false}});
	track.render(g_beat + 64);
	const std::string played = track.captured();
	CHECK(played.find(std::to_string(g_beat) + " 80 48 00\n") == 0);
	CHECK(played.find("80 3c") == played.find(std::to_string(2 * g_beat) + " 80 3c") + 6);
}

TEST_CASE("an effect that starts afresh takes its automated values, not the slot's", "[integration]")
{
	EffectTrack track;
	track.set("midiInserts", {{"slots", {slot("transformer")}}});
	const Json target = {{"type", "midiEffect"}, {"number", 0}, {"parameter", processors::Transformer::Value}};
	const auto lane = run(track.client, "automationLane.add", {{"track", track.track}, {"target", target}})
						  .at("lane")
						  .at("id")
						  .get<ObjectId>();
	run(track.client, "automationPoint.add",
		{{"lane", lane}, {"points", {{{"position", 0}, {"value", 12}, {"shape", "step"}}}}});
	run(track.client, "transport.play");
	track.render(64);
	// Switched off and on, the slot plays a new instance, which the lane doesn't send its value again.
	track.set("midiInserts", {{"slots", {{{"on", false}}}}});
	track.render(64);
	track.set("midiInserts", {{"slots", {{{"on", true}}}}});
	track.render(64);
	track.inject({message(200, {0x90, 60, 100})});
	track.render(64);
	CHECK(track.captured() == "200 90 48 64\n");
	// Moved across the divider, the insert plays a new instance too.
	track.set("midiInserts", {{"divider", 1}});
	track.render(64);
	track.inject({message(330, {0x90, 62, 100})});
	track.render(64);
	CHECK(track.captured() == "256 80 48 00\n330 90 4a 64\n"); // the old instance's note ends
}

TEST_CASE("a send that starts afresh when Pre is switched takes its automated values", "[integration]")
{
	EffectTrack track;
	track.set("midiSends", {{"slots", {slot("transformer")}}});
	const Json target = {{"type", "midiEffect"}, {"number", 4}, {"parameter", processors::Transformer::Value}};
	const auto lane = run(track.client, "automationLane.add", {{"track", track.track}, {"target", target}})
						  .at("lane")
						  .at("id")
						  .get<ObjectId>();
	run(track.client, "automationPoint.add",
		{{"lane", lane}, {"points", {{{"position", 0}, {"value", 12}, {"shape", "step"}}}}});
	run(track.client, "transport.play");
	track.render(64);
	track.set("midiSends", {{"pre", true}});
	track.render(64);
	// The track's own note and the send's, an octave up.
	track.inject({message(150, {0x90, 60, 100})});
	track.render(64);
	const std::string played = track.captured();
	CHECK(played.find("150 90 3c 64\n") != std::string::npos);
	CHECK(played.find("150 90 48 64\n") != std::string::npos);
}

TEST_CASE("the inserts above the divider take their automation at its sample", "[integration]")
{
	EffectTrack track;
	track.set("midiInserts", {{"divider", 1}, {"slots", {slot("transformer")}}});
	const Json target = {{"type", "midiEffect"}, {"number", 0}, {"parameter", processors::Transformer::Value}};
	const auto lane = run(track.client, "automationLane.add", {{"track", track.track}, {"target", target}})
						  .at("lane")
						  .at("id")
						  .get<ObjectId>();
	run(track.client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", 0}, {"shape", "step"}},
					{{"position", g_beatTicks + (25 * g_beatTicks / g_beat)}, {"value", 12}, {"shape", "step"}}}}});
	run(track.client, "transport.play");
	track.render(g_beat);
	// The value changes 25 samples into the block, between its two notes.
	track.inject({message(g_beat + 10, {0x90, 60, 100}), message(g_beat + 40, {0x90, 62, 100})});
	track.render(64);
	CHECK(
		track.captured() == std::to_string(g_beat + 10) + " 90 3c 64\n" + std::to_string(g_beat + 40) + " 90 4a 64\n");
}

TEST_CASE("a send that goes leaves the notes another track's send holds on the instrument", "[integration]")
{
	EffectTrack track;
	const ObjectId second = track.addInstrument();
	const ObjectId target = track.addInstrument();
	Json sent = slot("transformer");
	sent["output"] = target;
	track.set("midiSends", {{"slots", {sent}}});
	track.set("midiSends", {{"slots", {sent}}}, second);
	track.inject({message(10, {0x90, 60, 100})});
	run(track.client, "midi.inject", {{"channel", second}, {"messages", {message(20, {0x90, 60, 100})}}});
	track.render(64);
	CHECK(track.capturedBy()[target] == "10 90 3c 64\n20 90 3c 64\n");
	// The second track goes with its send; the first one's still holds the note there.
	const Json project = run(track.client, "project.get");
	for (const Json& channel : project.at("channels"))
		if (channel.at("id") == second)
			run(track.client, "track.remove", {{"track", channel.at("track")}});
	track.render(64);
	CHECK(track.capturedBy()[target] == "");
}

TEST_CASE(
	"a send slot without an effect has no output, also in an older project, and counts for no loop", "[integration]")
{
	EffectTrack track;
	const ObjectId other = track.addInstrument();
	Json sent = slot("transformer");
	sent["output"] = other;
	track.set("midiSends", {{"slots", {sent}}});
	const auto refused = [&](const Json& slot)
	{ return reject(track.client, "channel.set", {{"channel", track.channel}, {"midiSends", {{"slots", {slot}}}}}); };
	// Removing the effect with an output given is refused too, and the slot keeps its effect and its output.
	CHECK_THAT(refused({{"effect", ""}, {"output", other}}), ContainsSubstring("without an effect has no output"));
	Json kept;
	const Json project = run(track.client, "project.get");
	for (const Json& channel : project.at("channels"))
		if (channel.at("id") == track.channel)
			kept = channel.at("midiSends").at("slots")[0];
	CHECK(kept.at("effect") == "transformer");
	CHECK(kept.at("output") == other);
	CHECK(track.set("midiSends", {{"slots", {slot("")}}}).at("slots")[0].at("output") == 0);
	// Nor does an empty slot take an output, with or without the effect given.
	CHECK_THAT(refused({{"output", other}}), ContainsSubstring("without an effect has no output"));
	CHECK_THAT(refused({{"effect", ""}, {"output", other}}), ContainsSubstring("without an effect has no output"));
	// A project saved with an output on an empty slot loads without it.
	Json saved = run(track.client, "project.get");
	for (Json& channel : saved.at("channels"))
		if (channel.at("id") == track.channel)
			channel.at("midiSends").at("slots")[0]["output"] = other;
	CHECK(projectFromJson(saved).channels.at(track.channel).midiSends.slots[0].output == 0);
	sent["output"] = track.channel;
	CHECK(track.set("midiSends", {{"slots", {sent}}}, other).at("slots")[0].at("output") == track.channel);
}

namespace
{
	namespace plugin = gnubase::plugin;

	// Makes the built-in MIDI effects, each behind a plugin::CheckedPlugin that counts the calls on the wrong thread or
	// in the wrong state, and behind a FailingEffect that fails its blocks while failing is set. The audio thread is
	// the real-time context; while idle, no block processes the effects, so they may stop processing there too, as a
	// plugin slot stops its plugin before it deactivates it.
	class TestEffects final : public plugin::Factory, public plugin::ThreadCheck
	{
	public:
		explicit TestEffects(bool fail = false) : failing(fail) {}

		std::atomic<bool> failing{false};
		std::atomic<bool> idle{true};
		std::atomic<uint32_t> violations{0};
		std::atomic<const char*> lastViolation{nullptr};

		size_t count() const override { return m_builtins.count(); }
		const plugin::Descriptor& descriptor(size_t index) const override { return m_builtins.descriptor(index); }
		std::unique_ptr<plugin::Plugin> create(std::string_view id, plugin::Host& host) override;

		bool isMainThread() const noexcept override { return !gnubase::rt::isRealtimeContext(); }
		bool isAudioThread() const noexcept override { return gnubase::rt::isRealtimeContext() || idle; }

	private:
		static void violated(const char* message, void* context) noexcept
		{
			auto* self = static_cast<TestEffects*>(context);
			self->violations.fetch_add(1);
			self->lastViolation.store(message);
		}

		processors::BuiltinFactory m_builtins;
	};

	// A plugin that fails every block it gets while failing is set, as a third-party one might.
	class FailingEffect final : public plugin::Plugin
	{
	public:
		FailingEffect(std::unique_ptr<plugin::Plugin> plugin, const std::atomic<bool>& failing)
			: m_plugin(std::move(plugin)), m_failing(failing)
		{
		}

		const plugin::Descriptor& descriptor() const override { return m_plugin->descriptor(); }
		bool init() override { return m_plugin->init(); }
		bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) override
		{
			return m_plugin->activate(sampleRate, minFrames, maxFrames);
		}
		void deactivate() override { m_plugin->deactivate(); }
		bool startProcessing() noexcept override { return m_plugin->startProcessing(); }
		void stopProcessing() noexcept override { m_plugin->stopProcessing(); }
		void reset() noexcept override { (*m_plugin).reset(); }
		plugin::ProcessStatus process(const plugin::Process& process) noexcept override
		{
			return m_failing ? plugin::ProcessStatus::Error : m_plugin->process(process);
		}
		void* extension(std::string_view id) override { return m_plugin->extension(id); }

	private:
		std::unique_ptr<plugin::Plugin> m_plugin;
		const std::atomic<bool>& m_failing;
	};

	std::unique_ptr<plugin::Plugin> TestEffects::create(std::string_view id, plugin::Host& host)
	{
		std::unique_ptr<plugin::Plugin> made = m_builtins.create(id, host);
		if (made)
			made = std::make_unique<FailingEffect>(std::move(made), failing);
		if (made)
			made = std::make_unique<plugin::CheckedPlugin>(std::move(made), *this, &TestEffects::violated, this);
		return made;
	}

	// A sequence with the effect in the first insert slot, or none.
	std::unique_ptr<MidiSequence> withInsert(const std::string& effect)
	{
		auto sequence = std::make_unique<MidiSequence>();
		sequence->inserts.slots[0].effect = effect;
		return sequence;
	}

	// Plays a block of 128 frames with the events as the sequencer's input, on the audio thread, while stopped.
	EventBuffer playBlock(MidiSequencer& sequencer, const std::vector<ProcessEvent>& events)
	{
		const EventBuffer in(4);
		EventBuffer out(256);
		const graph::TransportBlock transport;
		for (const ProcessEvent& event : events)
			sequencer.input().add(event);
		{
			const gnubase::rt::ScopedRealtimeContext audioThread;
			sequencer.process({nullptr, 0, 128, in, out, transport});
		}
		sequencer.input().clear();
		return out;
	}

	// The log's text so far.
	std::string logged(const std::filesystem::path& file)
	{
		gnubase::logging::flush();
		return gnubase::test::readText(file);
	}

	// How often the text has the part.
	size_t occurrences(const std::string& text, std::string_view part)
	{
		size_t count = 0;
		for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
			++count;
		return count;
	}
}

TEST_CASE(
	"the sequencer starts its effects on the audio thread and stops and deactivates them before they go", "[unit]")
{
	auto effects = std::make_shared<TestEffects>();
	{
		auto sequencer = std::make_shared<MidiSequencer>(effects);
		sequencer->prepareFor(48000.0, 128);
		effects->idle = false;
		sequencer->setSequence(withInsert("transformer"));
		CHECK(hanging(playBlock(*sequencer, {noteAt(ProcessEvent::Type::NoteOn, 0, 60)})) == std::vector<int>{60});
		// The effect leaves its slot; it goes once the audio thread took the sequence without it.
		sequencer->setSequence(withInsert(""));
		playBlock(*sequencer, {});
		effects->idle = true;
		sequencer->setSequence(withInsert(""));
		// Again, and the sequencer goes with the effect active.
		effects->idle = false;
		sequencer->setSequence(withInsert("chordGenerator"));
		CHECK(!playBlock(*sequencer, {noteAt(ProcessEvent::Type::NoteOn, 0, 60)}).empty());
		// A new sample rate activates it again; the audio thread starts it again.
		effects->idle = true;
		sequencer->prepareFor(44100.0, 128);
		effects->idle = false;
		CHECK(!playBlock(*sequencer, {noteAt(ProcessEvent::Type::NoteOn, 0, 62)}).empty());
		effects->idle = true;
	}
	const char* last = effects->lastViolation.load();
	INFO((last != nullptr ? last : "none"));
	CHECK(effects->violations == 0);
}

TEST_CASE("an effect that fails a block passes its events on as they came, and the log says so", "[unit]")
{
	const gnubase::test::TempDir dir;
	const std::filesystem::path file = dir.path() / "gnubase.log";
	const gnubase::logging::Session log(file);
	auto effects = std::make_shared<TestEffects>(true);
	MidiSequencer sequencer(effects);
	sequencer.prepareFor(48000.0, 128);
	sequencer.setSequence(withInsert("chordGenerator"));
	effects->idle = false;
	const EventBuffer on = playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOn, 5, 60)});
	REQUIRE(on.size() == 1);
	CHECK(on[0].type == ProcessEvent::Type::NoteOn);
	CHECK(on[0].key == 60);
	CHECK(on[0].offset == 5);
	const EventBuffer off = playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOff, 7, 60)});
	REQUIRE(off.size() == 1);
	CHECK(off[0].type == ProcessEvent::Type::NoteOff);
	effects->idle = true;
	const std::string text = logged(file);
	CHECK_THAT(text, ContainsSubstring("chordGenerator in MIDI insert 1 failed"));
	// Once while it fails, not every block.
	CHECK(text.find("failed") == text.rfind("failed"));
}

TEST_CASE("an insert that starts to fail ends the notes it played, which would hang otherwise", "[unit]")
{
	auto effects = std::make_shared<TestEffects>();
	MidiSequencer sequencer(effects);
	sequencer.prepareFor(48000.0, 128);
	sequencer.setSequence(withInsert("chordGenerator"));
	effects->idle = false;
	std::vector<int> held;
	follow(playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOn, 5, 60)}), held);
	REQUIRE(held.size() > 1); // the chord
	// It fails from now on: the notes of the chord end, and the note-off that passes it ends the note again.
	effects->failing = true;
	follow(playBlock(sequencer, {}), held);
	CHECK(held.empty());
	follow(playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOff, 7, 60)}), held);
	CHECK(held.empty());
	effects->idle = true;
}

TEST_CASE("a send that starts to fail ends its notes and plays nothing while it fails", "[unit]")
{
	auto effects = std::make_shared<TestEffects>();
	MidiSequencer sequencer(effects);
	sequencer.prepareFor(48000.0, 128);
	auto sequence = std::make_unique<MidiSequence>();
	sequence->sends.slots[0].effect = "chordGenerator";
	sequencer.setSequence(std::move(sequence));
	effects->idle = false;
	std::vector<int> held;
	follow(playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOn, 5, 60)}), held);
	REQUIRE(held.size() > 2); // the note, and the chord the send played
	effects->failing = true;
	// The send's chord ends; the track's own note sounds on, and a new note plays once, not again through the send.
	const EventBuffer failed = playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOn, 5, 62)});
	follow(failed, held);
	std::sort(held.begin(), held.end());
	held.erase(std::unique(held.begin(), held.end()), held.end()); // both played 60
	CHECK(held == std::vector<int>{60, 62});
	CHECK(std::count_if(failed.begin(), failed.end(),
			  [](const ProcessEvent& e) { return e.type == ProcessEvent::Type::NoteOn && e.key == 62; }) == 1);
	follow(
		playBlock(sequencer, {noteAt(ProcessEvent::Type::NoteOff, 7, 60), noteAt(ProcessEvent::Type::NoteOff, 7, 62)}),
		held);
	CHECK(held.empty());
	effects->idle = true;
}

TEST_CASE("events that don't fit an effect's event lists are dropped, and the log says so", "[unit]")
{
	const gnubase::test::TempDir dir;
	const std::filesystem::path file = dir.path() / "gnubase.log";
	const gnubase::logging::Session log(file);
	MidiSequencer sequencer;
	sequencer.prepareFor(48000.0, 128);
	sequencer.setSequence(withInsert("transformer"));
	// More SysEx than a block's event lists hold.
	std::vector<uint8_t> dump(70000, 0x10);
	dump.front() = 0xF0;
	dump.back() = 0xF7;
	ProcessEvent sysex;
	sysex.type = ProcessEvent::Type::SysEx;
	sysex.data = dump.data();
	sysex.size = static_cast<uint32_t>(dump.size());
	CHECK(playBlock(sequencer, {sysex}).empty());
	// Once while it drops them, not every block, and again when it drops some after a block without.
	playBlock(sequencer, {sysex});
	playBlock(sequencer, {});
	playBlock(sequencer, {sysex});
	const std::string text = logged(file);
	CHECK_THAT(text, ContainsSubstring("transformer in MIDI insert 1 dropped events"));
	CHECK(occurrences(text, "dropped events") == 2);
}
