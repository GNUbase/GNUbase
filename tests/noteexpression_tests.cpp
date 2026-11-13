// SPDX-License-Identifier: GPL-3.0-or-later
// Per-note expression (#176, reference manual pp. 1293-1318): notes carry expression points that move with them, MPE
// input records into them, and playback sends them to the instrument, to an MPE instrument as MPE.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// 120 BPM at 48 kHz: a sample is 71.68 ticks.
	constexpr int64_t g_beatTicks = 1720320;

	int64_t ticksOf(int64_t samples)
	{
		return samples * g_beatTicks / 24000;
	}

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
		return {{"position", position}, {"bytes", std::move(bytes)}};
	}

	Json waitFor(Client& client, const std::string& name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> next = client.wait(std::chrono::milliseconds(50));
			const auto* event = next ? std::get_if<Event>(&*next) : nullptr;
			if (event != nullptr && event->name == name)
				return event->data;
		}
		return nullptr;
	}

	// An instrument track playing the note probe, record-enabled, on the dummy device.
	struct Setup
	{
		Setup() : engine(gnubase::audio::makeBackends(), {}, {})
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			track = added.at("track").at("id").get<ObjectId>();
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"recordEnable", true}});
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId channel = 0;
	};

	// A note's expression points as [type, position, value * 1000 rounded], sorted.
	using Points = std::vector<std::tuple<std::string, int64_t, int64_t>>;

	Points pointsOf(const Json& note)
	{
		Points points;
		for (const Json& point : note.value("expressions", Json::array()))
			points.emplace_back(point.at("type").get<std::string>(), point.at("position").get<int64_t>(),
				std::llround(point.at("value").get<double>() * 1000.0));
		std::sort(points.begin(), points.end());
		return points;
	}

	int64_t milli(double value)
	{
		return std::llround(value * 1000.0);
	}

	// What a MIDI 1.0 value records as: a controller's 0-1, and a pitch bend's semitones of MPE's 48.
	int64_t unit(uint8_t value)
	{
		return milli(midi::controllerToUnit(midi::controllerFromMidi1(value)));
	}

	int64_t semitones(uint16_t bend)
	{
		return milli(midi::bendToSemitones(midi::pitchBendFromMidi1(bend), 48.0));
	}
}

TEST_CASE("MPE input is recorded as per-note expression and played back to an MPE instrument", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "settings.set", {{"key", "midi.mpeInput"}, {"value", true}});
	CHECK(run(client, "transport.record").at("recording") == true);
	// MPE's lower zone: channel 1 (0) is the master channel, the notes come on channels 2 to 16, each with its own
	// pitch bend, channel pressure and CC 74, sent before its note-on for its initial state.
	run(client, "midi.inject",
		{{"channel", setup.channel},
			{"messages",
				{message(900, {0xE1, 0, 96}), message(900, {0xD1, 40}), message(900, {0xB1, 74, 64}),
					message(1000, {0x91, 60, 100}), message(1500, {0x92, 64, 90}), message(2000, {0xE1, 0, 80}),
					message(2200, {0xD2, 100}), message(2400, {0xB0, 1, 64}), message(2500, {0xB1, 74, 100}),
					message(3000, {0x81, 60, 0}),
					// No note sounds on channel 2: its state for the next note, nothing recorded.
					message(3200, {0xE1, 0, 64}), message(3500, {0x82, 64, 0})}}});
	run(client, "audio.render", {{"frames", 4000}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());

	const Json parts = run(client, "project.get").at("midiParts");
	REQUIRE(parts.size() == 1);
	const Json& part = parts[0];
	const int64_t partStart = part.at("position").get<int64_t>();
	REQUIRE(part.at("notes").size() == 2);
	Json first = part.at("notes")[0];
	Json second = part.at("notes")[1];
	if (first.at("pitch") != 60)
		std::swap(first, second);
	CHECK(first.at("position").get<int64_t>() + partStart == ticksOf(1000));
	CHECK(first.at("channel") == 1);
	// Pitch bend is tuning in semitones, by the MPE pitch range of 48; channel pressure is pressure, CC 74 brightness.
	CHECK(pointsOf(first) ==
		Points{{"brightness", 0, unit(64)}, {"brightness", ticksOf(1500), unit(100)}, {"pressure", 0, unit(40)},
			{"tuning", 0, semitones(96 << 7)}, {"tuning", ticksOf(1000), semitones(80 << 7)}});
	CHECK(semitones(96 << 7) == 24003); // a quarter of 14 bits up, by MIDI 2.0's scaling
	CHECK(unit(64) == 500);
	CHECK(second.at("channel") == 2);
	CHECK(pointsOf(second) == Points{{"pressure", ticksOf(700), unit(100)}});
	// The master channel's messages stay controller events.
	REQUIRE(part.at("controllers").size() == 1);
	CHECK(part.at("controllers")[0].at("type") == "controller");
	CHECK(part.at("controllers")[0].at("number") == 1);

	// Played back, the instrument gets each note on its channel with its expression as that channel's pitch bend,
	// channel pressure and CC 74, where it was played.
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.locate", {{"position", 0}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4000}});
	std::vector<std::vector<int64_t>> played;
	const Json capture = run(client, "midi.capture");
	for (const Json& captured : capture.at("captured"))
	{
		std::vector<int64_t> each{captured.at("position").get<int64_t>() - start};
		for (const Json& byte : captured.at("bytes"))
			each.push_back(byte.get<int64_t>());
		played.push_back(std::move(each));
	}
	std::sort(played.begin(), played.end());
	CHECK(played ==
		std::vector<std::vector<int64_t>>{{1000, 0x91, 60, 100}, {1000, 0xB1, 74, 64}, {1000, 0xD1, 40},
			{1000, 0xE1, 0, 96}, {1500, 0x92, 64, 90}, {2000, 0xE1, 0, 80}, {2200, 0xD2, 100}, {2400, 0xB0, 1, 64},
			{2500, 0xB1, 74, 100}, {3000, 0x81, 60, 0}, {3500, 0x82, 64, 0}});
}

TEST_CASE("without MPE input, MIDI on every channel records as controller events", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "transport.record");
	run(client, "midi.inject",
		{{"channel", setup.channel},
			{"messages",
				{message(1000, {0x91, 60, 100}), message(2000, {0xE1, 0, 80}), message(3000, {0x81, 60, 0})}}});
	run(client, "audio.render", {{"frames", 4000}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());
	const Json part = run(client, "project.get").at("midiParts").at(0);
	CHECK(pointsOf(part.at("notes").at(0)).empty());
	REQUIRE(part.at("controllers").size() == 1);
	CHECK(part.at("controllers")[0].at("type") == "pitchBend");
}

TEST_CASE(
	"notes take expression points through note.add and note.set, and keep them in the project file", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part =
		run(client, "midiPart.add", {{"track", setup.track}, {"position", 0}, {"length", 4 * g_beatTicks}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions",
						{{{"type", "tuning"}, {"position", 100}, {"value", -2.5}},
							{{"type", "volume"}, {"position", 0}, {"value", 2.0}}}}}}}});
	const Json note = added.at("notes").at(0);
	// Sorted by position.
	CHECK(note.at("expressions") == Json::parse(R"([{"type": "volume", "position": 0, "value": 2.0},
			{"type": "tuning", "position": 100, "value": -2.5}])"));

	// Each expression has its range; positions are from the note's start.
	const auto invalid = [&](Json point)
	{
		return reject(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", 0}, {"length", 10}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions", Json::array({std::move(point)})}}}}});
	};
	CHECK_THAT(invalid({{"type", "tuning"}, {"position", 0}, {"value", 121.0}}), ContainsSubstring("-120 to 120"));
	CHECK_THAT(invalid({{"type", "volume"}, {"position", 0}, {"value", 4.5}}), ContainsSubstring("0 to 4"));
	CHECK_THAT(invalid({{"type", "pressure"}, {"position", 0}, {"value", -0.1}}), ContainsSubstring("0 to 1"));
	CHECK_THAT(invalid({{"type", "pressure"}, {"position", -1}, {"value", 0.1}}), ContainsSubstring("negative"));
	CHECK(reject(client, "note.add",
			  {{"part", part},
				  {"notes",
					  {{{"position", 0}, {"length", 10}, {"pitch", 60}, {"velocity", 0.5},
						  {"expressions", {{{"type", "wobble"}, {"position", 0}, {"value", 0.0}}}}}}}})
			  .size() > 0);

	// note.set replaces them; undo brings them back.
	const Json set = run(client, "note.set",
		{{"part", part},
			{"notes",
				{{{"id", note.at("id")}, {"expressions", {{{"type", "pan"}, {"position", 5}, {"value", 0.25}}}}}}}});
	CHECK(pointsOf(set.at("notes").at(0)) == Points{{"pan", 5, 250}});
	run(client, "history.undo");
	CHECK(
		run(client, "project.get").at("midiParts").at(0).at("notes").at(0).at("expressions") == note.at("expressions"));

	// Moving a note moves its expression with it: the points are from its start.
	run(client, "note.set", {{"part", part}, {"notes", {{{"id", note.at("id")}, {"position", 960}}}}});
	const Json moved = run(client, "project.get").at("midiParts").at(0).at("notes").at(0);
	CHECK(moved.at("expressions") == note.at("expressions"));

	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "expressions.dawproject").u8string();
	const Json before = run(client, "project.get").at("midiParts");
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(run(client, "project.get").at("midiParts") == before);
}

TEST_CASE("playback sends a note's expression points inside the note, after its note-on", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part =
		run(client, "midiPart.add", {{"track", setup.track}, {"position", 0}, {"length", 4 * g_beatTicks}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	// 1000 samples long on channel 3; a point at the note's end and one past it don't play.
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", ticksOf(100)}, {"length", ticksOf(1000)}, {"pitch", 62}, {"velocity", 0.5},
					{"channel", 3},
					{"expressions",
						{{{"type", "tuning"}, {"position", 0}, {"value", -48.0}},
							{{"type", "brightness"}, {"position", ticksOf(500)}, {"value", 1.0}},
							{{"type", "volume"}, {"position", ticksOf(600)}, {"value", 1.0}},
							{{"type", "pressure"}, {"position", ticksOf(1000)}, {"value", 1.0}},
							{{"type", "pressure"}, {"position", ticksOf(5000)}, {"value", 1.0}}}}}}}});
	run(client, "audio.render", {{"frames", 64}});
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 7000}});
	std::vector<std::vector<int64_t>> played;
	const Json capture = run(client, "midi.capture");
	for (const Json& captured : capture.at("captured"))
		played.push_back({captured.at("position").get<int64_t>() - start, captured.at("bytes").at(0).get<int64_t>(),
			captured.at("bytes").size() > 1 ? captured.at("bytes").at(1).get<int64_t>() : -1});
	// Volume has no MPE message; the note-on comes before its expression at the same frame.
	CHECK(played ==
		std::vector<std::vector<int64_t>>{{100, 0x93, 62}, {100, 0xE3, 0}, {600, 0xB3, 74}, {1100, 0x83, 62}});
}
