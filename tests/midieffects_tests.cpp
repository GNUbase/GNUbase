// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/processors/midieffects.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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

		// What the instrument played since the last call, a line per message: its position and its bytes in hex.
		std::string captured()
		{
			std::string text;
			const Json capture = run(client, "midi.capture");
			for (const Json& c : capture.at("captured"))
			{
				text += std::to_string(c.at("position").get<int64_t>());
				for (const int byte : c.at("bytes").get<std::vector<int>>())
				{
					char hex[4] = {};
					std::snprintf(hex, sizeof hex, " %02x", byte);
					text += hex;
				}
				text += "\n";
			}
			return text;
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

	// The notes out still holds after its events, as channel * 128 + pitch.
	std::vector<int> hanging(const EventBuffer& out)
	{
		std::vector<int> held;
		for (const ProcessEvent& e : out)
		{
			const int index = (e.channel * 128) + e.key;
			if (e.type == ProcessEvent::Type::NoteOn)
				held.push_back(index);
			else if (e.type == ProcessEvent::Type::NoteOff)
				held.erase(std::remove(held.begin(), held.end(), index), held.end());
		}
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
	CHECK(!first.empty());
}

TEST_CASE("the chord generator and the transformer give note-offs room in a full output", "[unit]")
{
	EventBuffer in(4);
	graph::TransportBlock transport;
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
	CHECK(channel.at("midiInserts") == Json{{"bypass", false}, {"slots", {empty, empty, empty, empty}}});
	CHECK(channel.at("midiSends") == Json{{"bypass", false}, {"pre", false}, {"slots", {empty, empty, empty, empty}}});

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
	const Json sends = track.set("midiSends", {{"pre", true}, {"slots", {Json::object(), slot("chordGenerator")}}});
	run(track.client, "project.save", {{"path", file}});
	run(track.client, "project.new");
	run(track.client, "project.load", {{"path", file}});
	const Json channel = run(track.client, "project.get").at("channels")[0];
	CHECK(channel.at("midiInserts") == inserts);
	CHECK(channel.at("midiSends") == sends);
}
