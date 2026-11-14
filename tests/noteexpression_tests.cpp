// SPDX-License-Identifier: GPL-3.0-or-later
// Per-note expression (#176, reference manual pp. 1293-1318): notes carry expression points that move with them, MPE
// input records into them, and playback sends them to the instrument, to an MPE instrument as MPE.
#include "engine/document/document.h"
#include "engine/document/serialize.h"
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

	int64_t semitones(uint16_t bend, double range = 48.0)
	{
		return milli(midi::bendToSemitones(midi::pitchBendFromMidi1(bend), range));
	}

	// What the instruments played since capturing started, as [position from start, bytes].
	std::vector<std::vector<int64_t>> capturedFrom(Client& client, int64_t start)
	{
		std::vector<std::vector<int64_t>> played;
		const Json capture = run(client, "midi.capture");
		for (const Json& captured : capture.at("captured"))
		{
			std::vector<int64_t> each{captured.at("position").get<int64_t>() - start};
			for (const Json& byte : captured.at("bytes"))
				each.push_back(byte.get<int64_t>());
			played.push_back(std::move(each));
		}
		return played;
	}

	ObjectId addPart(Client& client, ObjectId track)
	{
		return run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_beatTicks}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	}

	Json point(const char* type, int64_t position, double value)
	{
		return {{"type", type}, {"position", position}, {"value", value}};
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
	// channel pressure and CC 74, where it was played. The second note wasn't pressed before its note-on: it starts
	// without pressure, MPE's initial value.
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.locate", {{"position", 0}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4000}});
	std::vector<std::vector<int64_t>> played = capturedFrom(client, start);
	std::sort(played.begin(), played.end());
	CHECK(played ==
		std::vector<std::vector<int64_t>>{{1000, 0x91, 60, 100}, {1000, 0xB1, 74, 64}, {1000, 0xD1, 40},
			{1000, 0xE1, 0, 96}, {1500, 0x92, 64, 90}, {1500, 0xD2, 0}, {2000, 0xE1, 0, 80}, {2200, 0xD2, 100},
			{2400, 0xB0, 1, 64}, {2500, 0xB1, 74, 100}, {3000, 0x81, 60, 0}, {3500, 0x82, 64, 0}});
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

TEST_CASE("playback sends a note's expression points inside the note, as MIDI 1.0 its initial ones before its note-on",
	"[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	// 1000 samples long on channel 3; a point at the note's end and one past it don't play. The next note on the
	// channel has no points: it starts at MPE's initial values of what the channel's notes express.
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", ticksOf(100)}, {"length", ticksOf(1000)}, {"pitch", 62}, {"velocity", 0.5},
					 {"channel", 3},
					 {"expressions",
						 {point("tuning", 0, -48.0), point("brightness", ticksOf(500), 1.0),
							 point("volume", ticksOf(600), 1.0), point("pressure", ticksOf(1000), 1.0),
							 point("pressure", ticksOf(5000), 1.0)}}},
					{{"position", ticksOf(2000)}, {"length", ticksOf(500)}, {"pitch", 64}, {"velocity", 0.5},
						{"channel", 3}}}}});
	run(client, "audio.render", {{"frames", 64}});
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 7000}});
	std::vector<std::vector<int64_t>> played;
	for (const std::vector<int64_t>& each : capturedFrom(client, start))
		played.push_back({each[0], each[1], each.size() > 2 ? each[2] : -1});
	// Volume has no MPE message. MPE puts a note's initial pitch bend, pressure and CC 74 before its note-on, the
	// note's own or MPE's initial ones (bend centered, CC 74 at 64), so that its voice doesn't start at the bend
	// that the channel's previous note left.
	CHECK(played ==
		std::vector<std::vector<int64_t>>{{100, 0xE3, 0}, {100, 0xB3, 74}, {100, 0x93, 62}, {600, 0xB3, 74},
			{1100, 0x83, 62}, {2000, 0xE3, 0}, {2000, 0xB3, 74}, {2000, 0x93, 64}, {2500, 0x83, 64}});
}

TEST_CASE("a note's point within its first sample, not at its start, is its initial one", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	// Its tuning point lies 10 ticks after its start, within the sample it starts at, and it has none at 0.
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", ticksOf(100)}, {"length", ticksOf(1000)}, {"pitch", 62}, {"velocity", 0.5},
					{"channel", 3}, {"expressions", {point("tuning", 10, -48.0)}}}}}});
	run(client, "audio.render", {{"frames", 64}});
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 2000}});
	std::vector<std::vector<int64_t>> played;
	for (const std::vector<int64_t>& each : capturedFrom(client, start))
		if (each.size() > 3 && (each[1] == 0xE3 || each[1] == 0x93))
			played.push_back({each[0], each[1], each[1] == 0xE3 ? each[2] : 0, each[1] == 0xE3 ? each[3] : 0});
	// Its own bend, all the way down, before its note-on, and no centered one after it.
	CHECK(played == std::vector<std::vector<int64_t>>{{100, 0xE3, 0, 0}, {100, 0x93, 0, 0}});
}

TEST_CASE("splitting a part's notes keeps their expression where it was", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions",
						{point("tuning", 0, 0.0), point("pressure", 0, 0.5), point("tuning", g_beatTicks / 2, 2.0),
							point("brightness", g_beatTicks, 1.0), point("tuning", 3 * g_beatTicks / 2, 4.0)}}}}}});
	// At the first beat, 24000 samples.
	run(client, "event.split", {{"events", {part}}, {"positions", {24000}}, {"splitNotes", true}});
	const Json parts = run(client, "project.get").at("midiParts");
	REQUIRE(parts.size() == 2);
	Json left = parts[0];
	Json right = parts[1];
	if (left.at("position") != 0)
		std::swap(left, right);
	CHECK(pointsOf(left.at("notes").at(0)) ==
		Points{{"pressure", 0, 500}, {"tuning", 0, 0}, {"tuning", g_beatTicks / 2, 2000}});
	// The rest starts with the values at the split and goes on from there.
	CHECK(pointsOf(right.at("notes").at(0)) ==
		Points{{"brightness", 0, 1000}, {"pressure", 0, 500}, {"tuning", 0, 2000}, {"tuning", g_beatTicks / 2, 4000}});
}

TEST_CASE("note.split cuts notes with their expression where it was, note.glue joins it again", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5}, {"channel", 2},
					 {"probability", 0.5},
					 {"expressions",
						 {point("tuning", 0, 0.0), point("pressure", 0, 0.5), point("tuning", g_beatTicks / 2, 2.0),
							 point("brightness", g_beatTicks, 1.0), point("tuning", 3 * g_beatTicks / 2, 4.0)}}},
					{{"position", 3 * g_beatTicks}, {"length", g_beatTicks}, {"pitch", 62}, {"velocity", 0.5}}}}});
	const Json note = added.at("notes").at(0);
	const Json other = added.at("notes").at(1);
	// The cut at the first beat cuts the first note only; the rest keeps its values.
	const Json rests = run(
		client, "note.split", {{"part", part}, {"notes", {note.at("id"), other.at("id")}}, {"position", g_beatTicks}})
						   .at("notes");
	REQUIRE(rests.size() == 1);
	const Json& rest = rests[0];
	CHECK(rest.at("position") == g_beatTicks);
	CHECK(rest.at("length") == g_beatTicks);
	CHECK(rest.at("pitch") == 60);
	CHECK(rest.at("channel") == 2);
	CHECK(rest.at("probability") == 0.5);
	CHECK(pointsOf(rest) ==
		Points{{"brightness", 0, 1000}, {"pressure", 0, 500}, {"tuning", 0, 2000}, {"tuning", g_beatTicks / 2, 4000}});
	const auto notes = [&]
	{
		Json all = run(client, "project.get").at("midiParts").at(0).at("notes");
		std::sort(all.begin(), all.end(), [](const Json& a, const Json& b)
			{ return a.at("position").get<int64_t>() < b.at("position").get<int64_t>(); });
		return all;
	};
	Json now = notes();
	REQUIRE(now.size() == 3);
	CHECK(now[0].at("length") == g_beatTicks);
	CHECK(pointsOf(now[0]) == Points{{"pressure", 0, 500}, {"tuning", 0, 0}, {"tuning", g_beatTicks / 2, 2000}});
	CHECK(now[2].at("length") == g_beatTicks);

	// Glued, the note plays what the two played: its own points up to the rest's start, then the rest's.
	const Json glued =
		run(client, "note.glue", {{"part", part}, {"note", note.at("id")}, {"next", rest.at("id")}}).at("note");
	CHECK(glued.at("length") == 2 * g_beatTicks);
	CHECK(pointsOf(glued) ==
		Points{{"brightness", g_beatTicks, 1000}, {"pressure", 0, 500}, {"pressure", g_beatTicks, 500},
			{"tuning", 0, 0}, {"tuning", g_beatTicks / 2, 2000}, {"tuning", g_beatTicks, 2000},
			{"tuning", 3 * g_beatTicks / 2, 4000}});
	now = notes();
	REQUIRE(now.size() == 2);
	CHECK(now[0] == glued);
	CHECK_THAT(reject(client, "note.glue", {{"part", part}, {"note", other.at("id")}, {"next", note.at("id")}}),
		ContainsSubstring("must start after"));
}

TEST_CASE("a note.split that cuts nothing leaves the project unmodified", "[integration]")
{
	// Review of #1480: no revision, so no modified project without an undo step to take it back.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json note = run(client, "note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5}}}}})
						  .at("notes")
						  .at(0);
	const Json before = run(client, "project.revision");
	for (const int64_t position : {int64_t{0}, g_beatTicks, 2 * g_beatTicks})
		CHECK(run(client, "note.split", {{"part", part}, {"notes", {note.at("id")}}, {"position", position}})
				.at("notes")
				.empty());
	CHECK(run(client, "project.revision") == before);
}

TEST_CASE("note.glue leaves out the next note's points from its end on: they didn't play", "[integration]")
{
	// Review of #1480: the next note ends inside the first, and has points at and past its end, e.g. after a trim.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions", {point("tuning", 0, 0.0)}}},
					{{"position", g_beatTicks / 2}, {"length", g_beatTicks / 2}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions",
							{point("tuning", 0, 1.0), point("tuning", g_beatTicks / 4, 2.0),
								point("tuning", g_beatTicks / 2, 3.0), point("pressure", g_beatTicks, 0.5)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	CHECK(glued.at("length") == 2 * g_beatTicks);
	CHECK(pointsOf(glued) ==
		Points{{"tuning", 0, 0}, {"tuning", g_beatTicks / 2, 1000}, {"tuning", 3 * g_beatTicks / 4, 2000}});
}

TEST_CASE("resizing a note's start keeps its expression where it was", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json note = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", g_beatTicks}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions", {point("tuning", 0, 1.0), point("tuning", g_beatTicks / 2, 3.0)}}}}}})
						  .at("notes")
						  .at(0);
	const auto resize = [&](int64_t position)
	{
		return run(client, "note.set",
			{{"part", part},
				{"notes", {{{"id", note.at("id")}, {"position", position}, {"length", (2 * g_beatTicks) - position}}}}})
			.at("notes")
			.at(0);
	};
	// Later: the points before the new start become its initial values.
	CHECK(pointsOf(resize(5 * g_beatTicks / 4)) == Points{{"tuning", 0, 1000}, {"tuning", g_beatTicks / 4, 3000}});
	run(client, "history.undo");
	// Earlier: the initial values start at the new start, the other points stay where they were.
	CHECK(pointsOf(resize(g_beatTicks / 2)) == Points{{"tuning", 0, 1000}, {"tuning", g_beatTicks, 3000}});
}

TEST_CASE("sends before the modifiers get the notes' expression", "[integration]")
{
	for (const bool pre : {false, true})
	{
		Setup setup;
		Client& client = setup.client;
		const ObjectId part = addPart(client, setup.track);
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", 0}, {"length", g_beatTicks / 4}, {"pitch", 60}, {"velocity", 0.5}, {"channel", 2},
						{"expressions", {point("tuning", ticksOf(100), 12.0)}}}}}});
		// A send to another instrument track, which gets every event the send plays.
		const ObjectId other =
			run(client, "track.add", {{"type", "instrument"}}).at("channel").at("id").get<ObjectId>();
		run(client, "channel.set", {{"channel", other}, {"instrument", "noteProbe"}});
		run(client, "channel.set",
			{{"channel", setup.channel}, {"recordEnable", false}, {"modifiers", {{"transpose", 12}}},
				{"midiSends",
					{{"pre", pre},
						{"slots", {{{"effect", "transformer"}, {"parameters", {{"value", 1}}}, {"output", other}}}}}}});
		run(client, "audio.render", {{"frames", 64}});
		const auto start = run(client, "audio.get").at("position").get<int64_t>();
		run(client, "midi.capture", {{"start", true}});
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", 1000}});
		std::vector<std::vector<int64_t>> sent; // what the other track's instrument played
		const Json capture = run(client, "midi.capture");
		for (const Json& captured : capture.at("captured"))
			if (captured.at("channel") == other)
				sent.push_back({captured.at("position").get<int64_t>() - start,
					captured.at("bytes").at(0).get<int64_t>(), captured.at("bytes").at(1).get<int64_t>()});
		INFO("sends " << (pre ? "pre" : "post"));
		// The transformer moves the note, but not yet its expression: its initial point no longer comes before it.
		CHECK(sent == std::vector<std::vector<int64_t>>{{0, 0x92, pre ? 61 : 73}, {0, 0xE2, 0}, {100, 0xE2, 0}});
	}
}

TEST_CASE("retrospective recording records MPE input by midi.mpePitchRange", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "settings.set", {{"key", "midi.mpeInput"}, {"value", true}});
	run(client, "settings.set", {{"key", "midi.mpePitchRange"}, {"value", 24}});
	run(client, "channel.set", {{"channel", setup.channel}, {"recordEnable", false}});
	// In stop mode: a bend before the note-on, its initial tuning, and one while it sounds.
	run(client, "midi.inject",
		{{"channel", setup.channel},
			{"messages",
				{message(900, {0xE1, 0, 96}), message(1000, {0x91, 60, 100}), message(2000, {0xE1, 0, 80}),
					message(3000, {0x81, 60, 0})}}});
	run(client, "audio.render", {{"frames", 4000}});
	run(client, "midi.insertRetrospective", {{"track", setup.track}});
	const Json part = run(client, "project.get").at("midiParts").at(0);
	REQUIRE(part.at("notes").size() == 1);
	CHECK(part.at("controllers").empty());
	CHECK(pointsOf(part.at("notes").at(0)) ==
		Points{{"tuning", 0, semitones(96 << 7, 24.0)}, {"tuning", ticksOf(1000), semitones(80 << 7, 24.0)}});
	CHECK(semitones(96 << 7, 24.0) == 12001);
}

TEST_CASE("recording records MPE input by midi.mpePitchRange", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "settings.set", {{"key", "midi.mpeInput"}, {"value", true}});
	run(client, "settings.set", {{"key", "midi.mpePitchRange"}, {"value", 24}});
	run(client, "transport.record");
	run(client, "midi.inject",
		{{"channel", setup.channel},
			{"messages",
				{message(900, {0xE1, 0, 96}), message(1000, {0x91, 60, 100}), message(2000, {0xE1, 0, 80}),
					message(3000, {0x81, 60, 0})}}});
	run(client, "audio.render", {{"frames", 4000}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());
	const Json part = run(client, "project.get").at("midiParts").at(0);
	REQUIRE(part.at("notes").size() == 1);
	CHECK(part.at("controllers").empty());
	CHECK(pointsOf(part.at("notes").at(0)) ==
		Points{{"tuning", 0, semitones(96 << 7, 24.0)}, {"tuning", ticksOf(1000), semitones(80 << 7, 24.0)}});
}

TEST_CASE(
	"a project loads with expression points out of range clamped and unknown expressions left out", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions", {point("tuning", 10, 0.0)}}}}}});
	Json json = run(client, "project.get");
	json["midiParts"][0]["notes"][0]["expressions"] =
		Json::array({point("tuning", 10, 200.0), point("pressure", -5, -1.0), point("wobble", 0, 0.5)});
	const Project loaded = projectFromJson(json);
	const std::vector<ExpressionPoint>& points = loaded.midiParts.at(part).notes.begin()->second.expressions;
	REQUIRE(points.size() == 2);
	CHECK(points[0].type == gnubase::plugin::NoteExpression::Pressure);
	CHECK(points[0].position == 0);
	CHECK(points[0].value == 0.0);
	CHECK(points[1].type == gnubase::plugin::NoteExpression::Tuning);
	CHECK(points[1].value == 120.0);
}
