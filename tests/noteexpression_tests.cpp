// SPDX-License-Identifier: GPL-3.0-or-later
// Per-note expression (#176, reference manual pp. 1293-1318): notes carry expression points that move with them, MPE
// input records into them (tests/mpeinput_tests.cpp), and playback sends them to the instrument, to an MPE instrument
// as MPE.
#include "engine/document/document.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
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

	// The data of the events since the last call, by name.
	std::map<std::string, std::vector<Json>> events(Client& client)
	{
		std::map<std::string, std::vector<Json>> found;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				found[event->name].push_back(event->data);
		return found;
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

	// Glued, the note plays what the two played: its own points up to the rest's start, then the rest's, without the
	// points of the rest that only repeat the value that plays there. That is the expression the note had before the
	// cut, as it has no point that changes nothing.
	const Json glued =
		run(client, "note.glue", {{"part", part}, {"note", note.at("id")}, {"next", rest.at("id")}}).at("note");
	CHECK(glued.at("length") == 2 * g_beatTicks);
	CHECK(pointsOf(glued) ==
		Points{{"brightness", g_beatTicks, 1000}, {"pressure", 0, 500}, {"tuning", 0, 0},
			{"tuning", g_beatTicks / 2, 2000}, {"tuning", 3 * g_beatTicks / 2, 4000}});
	now = notes();
	REQUIRE(now.size() == 2);
	CHECK(now[0] == glued);
	CHECK_THAT(reject(client, "note.glue", {{"part", part}, {"note", other.at("id")}, {"next", note.at("id")}}),
		ContainsSubstring("must start after"));
}

TEST_CASE("glued after a split, a note loses only points at the cut that repeated the value before it", "[integration]")
{
	// Review of #1608: glue leaves out only the rest's points at its start that repeat what plays; one elsewhere that
	// repeats the value before it, in the first part or later in the rest, stays a point through the split and glue.
	// Review of #1676: at its start is within its first sample, so the point ten ticks after the cut goes too.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json note = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions",
						{point("tuning", 0, 0.0), point("tuning", g_beatTicks / 4, 0.0),
							point("tuning", g_beatTicks / 2, 2.0), point("tuning", g_beatTicks, 2.0),
							point("tuning", g_beatTicks + 10, 2.0), point("tuning", 3 * g_beatTicks / 2, 2.0)}}}}}})
						  .at("notes")
						  .at(0);
	const Json rest = run(client, "note.split", {{"part", part}, {"notes", {note.at("id")}}, {"position", g_beatTicks}})
						  .at("notes")
						  .at(0);
	const Json glued =
		run(client, "note.glue", {{"part", part}, {"note", note.at("id")}, {"next", rest.at("id")}}).at("note");
	CHECK(pointsOf(glued) ==
		Points{{"tuning", 0, 0}, {"tuning", g_beatTicks / 4, 0}, {"tuning", g_beatTicks / 2, 2000},
			{"tuning", 3 * g_beatTicks / 2, 2000}});
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
	// Review of #1480: the next note ends after the first, and has points at and past its end, e.g. after a trim.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions", {point("tuning", 0, 0.0)}}},
					{{"position", g_beatTicks / 2}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions",
							{point("tuning", 0, 1.0), point("tuning", g_beatTicks / 2, 2.0),
								point("tuning", g_beatTicks, 3.0), point("pressure", 2 * g_beatTicks, 0.5)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	CHECK(glued.at("length") == 3 * g_beatTicks / 2);
	CHECK(
		pointsOf(glued) == Points{{"tuning", 0, 0}, {"tuning", g_beatTicks / 2, 1000}, {"tuning", g_beatTicks, 2000}});
}

TEST_CASE(
	"note.glue gives the join the defaults of what the next note doesn't express, across a gap too", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions",
						 {point("tuning", 0, 0.0), point("vibrato", 0, 0.5), point("tuning", g_beatTicks / 2, 2.0),
							 point("pressure", g_beatTicks / 2, 0.8), point("volume", g_beatTicks / 2, 2.0)}}},
					{{"position", 2 * g_beatTicks}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions", {point("pressure", 0, 0.3), point("brightness", g_beatTicks / 2, 1.0)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	CHECK(glued.at("length") == 3 * g_beatTicks);
	// Bent up and louder before the gap, the next note played unbent at unity gain: the join goes back to that.
	// Vibrato has no default, so the note keeps its own; the next note's pressure at its start is its own.
	CHECK(pointsOf(glued) ==
		Points{{"brightness", 5 * g_beatTicks / 2, 1000}, {"pressure", g_beatTicks / 2, 800},
			{"pressure", 2 * g_beatTicks, 300}, {"tuning", 0, 0}, {"tuning", g_beatTicks / 2, 2000},
			{"tuning", 2 * g_beatTicks, 0}, {"vibrato", 0, 500}, {"volume", g_beatTicks / 2, 2000},
			{"volume", 2 * g_beatTicks, 1000}});
}

TEST_CASE("note.glue of a note inside the first one goes on with the first one's expression after it", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 4 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions",
						 {point("tuning", 0, 1.0), point("vibrato", 0, 0.3), point("tuning", 2 * g_beatTicks, 2.0),
							 point("tuning", 3 * g_beatTicks, 3.0)}}},
					{{"position", g_beatTicks}, {"length", 3 * g_beatTicks / 2}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions", {point("brightness", 0, 1.0), point("tuning", g_beatTicks / 2, -1.0)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	CHECK(glued.at("length") == 4 * g_beatTicks);
	// Up to the next note's end its expression, unbent at its start; then the first one's again, as it was there.
	// The vibrato that the next note doesn't express goes on through it, without a point after it.
	CHECK(pointsOf(glued) ==
		Points{{"brightness", g_beatTicks, 1000}, {"brightness", 5 * g_beatTicks / 2, 500}, {"tuning", 0, 1000},
			{"tuning", g_beatTicks, 0}, {"tuning", 3 * g_beatTicks / 2, -1000}, {"tuning", 5 * g_beatTicks / 2, 2000},
			{"tuning", 3 * g_beatTicks, 3000}, {"vibrato", 0, 300}});
}

TEST_CASE("note.glue gives pan and pressure their defaults at the join, and no point where the value plays already",
	"[integration]")
{
	// Review of #1470.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions",
						 {point("pan", 0, 0.2), point("pressure", 0, 0.6), point("volume", 0, 1.0),
							 point("tuning", 0, 0.0), point("brightness", g_beatTicks / 2, 0.5)}}},
					{{"position", g_beatTicks}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions", {point("tuning", 0, 0.0), point("tuning", g_beatTicks / 2, 1.0)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	CHECK(glued.at("length") == 2 * g_beatTicks);
	// Panned and pressed before the join, the next note played centered without pressure. Volume, brightness and
	// tuning already have the value the next note starts with.
	CHECK(pointsOf(glued) ==
		Points{{"brightness", g_beatTicks / 2, 500}, {"pan", 0, 200}, {"pan", g_beatTicks, 500}, {"pressure", 0, 600},
			{"pressure", g_beatTicks, 0}, {"tuning", 0, 0}, {"tuning", 3 * g_beatTicks / 2, 1000},
			{"volume", 0, 1000}});
}

TEST_CASE("note.glue of a next note that starts inside the first one and ends after it", "[integration]")
{
	// Review of #1470: both notes have expression where they overlap.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions",
						 {point("brightness", 0, 0.8), point("tuning", 0, 0.0),
							 point("tuning", 3 * g_beatTicks / 2, 1.0)}}},
					{{"position", g_beatTicks}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions", {point("tuning", 0, 2.0), point("pressure", g_beatTicks / 2, 0.5)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	CHECK(glued.at("length") == 3 * g_beatTicks);
	// From the next note's start on its expression: the first one's tuning point inside it never played.
	CHECK(pointsOf(glued) ==
		Points{{"brightness", 0, 800}, {"brightness", g_beatTicks, 500}, {"pressure", 3 * g_beatTicks / 2, 500},
			{"tuning", 0, 0}, {"tuning", g_beatTicks, 2000}});
}

TEST_CASE("note.glue takes a point within the next note's first sample as one it starts with", "[integration]")
{
	// Review of #1470: as playback does (RenderNote::startsWith), so the join gets no default before it.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions", {point("tuning", 0, 2.0), point("pressure", 0, 0.4)}}},
					{{"position", g_beatTicks}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions", {point("tuning", 10, -1.0), point("pressure", ticksOf(2), 0.7)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	// The pressure point two samples in is not at its start: the next note started without pressure.
	CHECK(pointsOf(glued) ==
		Points{{"pressure", 0, 400}, {"pressure", g_beatTicks, 0}, {"pressure", g_beatTicks + ticksOf(2), 700},
			{"tuning", 0, 2000}, {"tuning", g_beatTicks + 10, -1000}});
}

TEST_CASE("note.glue leaves out a point within the next note's first sample only where its value already plays",
	"[integration]")
{
	// Review of #1519: what plays changes with each point the join keeps, and a point a few ticks in is at the start.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions", {point("tuning", 0, 2.0), point("pressure", 0, 0.4)}}},
					{{"position", g_beatTicks}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
						{"expressions",
							{point("tuning", 0, 1.0), point("tuning", 10, 2.0), point("pressure", 10, 0.4)}}}}}});
	const Json glued = run(client, "note.glue",
		{{"part", part}, {"note", added.at("notes").at(0).at("id")}, {"next", added.at("notes").at(1).at("id")}})
						   .at("note");
	// Tuning 1 at the join changes what plays, so tuning 2 ten ticks later changes it again. The pressure ten ticks in
	// is within the first sample and repeats what plays: no point, and no default before it.
	CHECK(pointsOf(glued) ==
		Points{{"pressure", 0, 400}, {"tuning", 0, 2000}, {"tuning", g_beatTicks, 1000},
			{"tuning", g_beatTicks + 10, 2000}});
}

TEST_CASE("note.split and note.glue refuse notes the part doesn't have, and undo in one step", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", g_beatTicks}, {"length", 2 * g_beatTicks}, {"pitch", 60}, {"velocity", 0.5}}}}});
	const Json first = added.at("notes").at(0).at("id");
	const Json second = added.at("notes").at(1).at("id");
	const auto notes = [&] { return run(client, "project.get").at("midiParts").at(0).at("notes"); };
	const Json before = notes();
	CHECK_THAT(
		reject(client, "note.split", {{"part", part}, {"notes", {first, 999999}}, {"position", g_beatTicks / 2}}),
		ContainsSubstring("no note 999999"));
	CHECK_THAT(reject(client, "note.glue", {{"part", part}, {"note", first}, {"next", 999999}}),
		ContainsSubstring("no note 999999"));
	CHECK_THAT(reject(client, "note.glue", {{"part", part}, {"note", 999999}, {"next", second}}),
		ContainsSubstring("no note 999999"));
	CHECK(notes() == before);

	// Both notes cut at once, then put back by one undo.
	CHECK(run(client, "note.split", {{"part", part}, {"notes", {first, second}}, {"position", 3 * g_beatTicks / 2}})
			  .at("notes")
			  .size() == 2);
	CHECK(notes().size() == 4);
	run(client, "history.undo");
	CHECK(notes() == before);

	// Overlapping notes glue to the later end, and one undo brings both back.
	const Json glued = run(client, "note.glue", {{"part", part}, {"note", first}, {"next", second}}).at("note");
	CHECK(glued.at("length") == 3 * g_beatTicks);
	CHECK(notes().size() == 1);
	run(client, "history.undo");
	CHECK(notes() == before);
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
	for (const auto& [pre, bypass] : {std::pair{false, false}, std::pair{true, false}, std::pair{true, true}})
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
		run(client, "noteExpression.set", {{"track", setup.track}, {"bypass", bypass}});
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
		INFO("sends " << (pre ? "pre" : "post") << (bypass ? ", expression bypassed" : ""));
		// The transformer moves the note with its expression (#1618), whose initial point comes before it; with the
		// track's expression bypassed, also the sends before the modifiers get the note alone.
		if (bypass)
			CHECK(sent == std::vector<std::vector<int64_t>>{{0, 0x92, 61}});
		else
			CHECK(sent == std::vector<std::vector<int64_t>>{{0, 0xE2, 0}, {0, 0x92, pre ? 61 : 73}, {100, 0xE2, 0}});
	}
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

TEST_CASE("expression points lie at most the longest project's end after their note's start", "[integration]")
{
	// #1831: so that a note's start plus a point's position can't overflow, as for positions and lengths (#1906).
	// Points past the note's end stay possible, as a note shortened by the trim tool keeps them.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	Json project = run(client, "project.get");
	const int64_t longest = longestProjectTicks(projectFromJson(project));
	const auto note = [](Json expressions)
	{
		return Json{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
			{"expressions", std::move(expressions)}};
	};
	const std::string bound = "must be at most " + std::to_string(longest);
	CHECK_THAT(reject(client, "note.add",
				   {{"part", part}, {"notes", Json::array({note(Json::array({point("tuning", longest + 1, 1.0)}))})}}),
		ContainsSubstring("notes[0].expressions[0].position " + bound));
	const Json id = run(client, "note.add",
		{{"part", part},
			{"notes",
				Json::array({note(Json::array({point("tuning", 2 * g_beatTicks, 1.0), point("pan", longest, 0.5)}))})}})
						.at("notes")
						.at(0)
						.at("id");
	CHECK_THAT(reject(client, "note.set",
				   {{"part", part},
					   {"notes",
						   Json::array({{{"id", id},
							   {"expressions", {point("pan", 0, 0.5), point("tuning", longest + 1, 1.0)}}}})}}),
		ContainsSubstring("notes[0].expressions[1].position " + bound));
	CHECK_THAT(reject(client, "note.replaceExpression",
				   {{"part", part}, {"type", "tuning"},
					   {"notes",
						   Json::array({{{"id", id}, {"from", 0}, {"to", longest + 1},
							   {"points", Json::array({{{"position", longest + 1}, {"value", 1.0}}})}}})}}),
		ContainsSubstring("notes[0].points[0].position " + bound));
	const Json changed = run(client, "note.replaceExpression",
		{{"part", part}, {"type", "tuning"},
			{"notes",
				Json::array({{{"id", id}, {"from", 0}, {"to", longest},
					{"points", Json::array({{{"position", longest}, {"value", 2.0}}})}}})}});
	CHECK(pointsOf(changed.at("notes").at(0)) == Points{{"pan", longest, 500}, {"tuning", longest, 2000}});

	// A project file can hold any position: loading cuts its points at the longest project.
	project = run(client, "project.get");
	project["midiParts"][0]["notes"][0]["expressions"] =
		Json::array({point("tuning", 10, 1.0), point("pan", std::numeric_limits<int64_t>::max(), 0.5)});
	const Project loaded = projectFromJson(project);
	const std::vector<ExpressionPoint>& points = loaded.midiParts.at(part).notes.begin()->second.expressions;
	REQUIRE(points.size() == 2);
	CHECK(points[0].position == 10);
	CHECK(points[1].position == longest);
}

TEST_CASE("note.replaceExpression replaces one expression's points in a span of each note and keeps the others",
	"[integration]")
{
	// The key editor's expression editor (#1795): an edit names only what its gesture went over, so that it doesn't
	// depend on the points that the editor's copy of the project has.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json added = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					 {"expressions",
						 {point("tuning", 0, 1.0), point("tuning", 100, 2.0), point("pressure", 150, 0.5),
							 point("tuning", 200, 3.0), point("tuning", 300, 4.0)}}},
					{{"position", g_beatTicks}, {"length", g_beatTicks}, {"pitch", 62}, {"velocity", 0.5},
						{"expressions", {point("tuning", 0, 5.0)}}}}}});
	const Json first = added.at("notes").at(0).at("id");
	const Json second = added.at("notes").at(1).at("id");
	const auto noteOf = [&](const Json& id)
	{
		const Json project = run(client, "project.get"); // kept: a range-for keeps no temporary it reaches into
		for (const Json& note : project.at("midiParts").at(0).at("notes"))
			if (note.at("id") == id)
				return note;
		return Json();
	};
	const auto replace = [&](Json notes)
	{
		return run(client, "note.replaceExpression", {{"part", part}, {"type", "tuning"}, {"notes", std::move(notes)}});
	};

	// The tuning points from 100 to 200, both ends included, give way to the new ones; the pressure point between
	// them and the tuning points outside the span stay.
	const Json changed = replace(Json::array({{{"id", first}, {"from", 100}, {"to", 200},
		{"points", {{{"position", 150}, {"value", -2.0}}, {{"position", 120}, {"value", 7.0}}}}}}));
	REQUIRE(changed.at("notes").size() == 1);
	CHECK(pointsOf(changed.at("notes").at(0)) ==
		Points{{"pressure", 150, 500}, {"tuning", 0, 1000}, {"tuning", 120, 7000}, {"tuning", 150, -2000},
			{"tuning", 300, 4000}});
	// Sorted by position, a new point after the one of another expression at its position.
	std::vector<std::pair<std::string, int64_t>> order;
	const Json sorted = noteOf(first);
	for (const Json& each : sorted.at("expressions"))
		order.emplace_back(each.at("type").get<std::string>(), each.at("position").get<int64_t>());
	CHECK(order ==
		std::vector<std::pair<std::string, int64_t>>{
			{"tuning", 0}, {"tuning", 120}, {"pressure", 150}, {"tuning", 150}, {"tuning", 300}});
	CHECK(pointsOf(noteOf(second)) == Points{{"tuning", 0, 5000}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Change note expression");
	run(client, "history.undo");
	CHECK(pointsOf(noteOf(first)) ==
		Points{{"pressure", 150, 500}, {"tuning", 0, 1000}, {"tuning", 100, 2000}, {"tuning", 200, 3000},
			{"tuning", 300, 4000}});
	run(client, "history.redo");

	// Edits that a client made from the same state both stay (#1795): an erase, a draw elsewhere that knows nothing
	// of the erase, then an erase of the point the draw made.
	replace(Json::array({{{"id", first}, {"from", 290}, {"to", 310}, {"points", Json::array()}}}));
	replace(Json::array(
		{{{"id", first}, {"from", 400}, {"to", 400}, {"points", Json::array({{{"position", 400}, {"value", 9.0}}})}}}));
	CHECK(pointsOf(noteOf(first)) ==
		Points{{"pressure", 150, 500}, {"tuning", 0, 1000}, {"tuning", 120, 7000}, {"tuning", 150, -2000},
			{"tuning", 400, 9000}});
	replace({{{"id", first}, {"from", 390}, {"to", 410}, {"points", Json::array()}},
		{{"id", second}, {"from", 0}, {"to", 0}, {"points", Json::array({{{"position", 0}, {"value", 6.0}}})}}});
	CHECK(pointsOf(noteOf(first)) ==
		Points{{"pressure", 150, 500}, {"tuning", 0, 1000}, {"tuning", 120, 7000}, {"tuning", 150, -2000}});
	CHECK(pointsOf(noteOf(second)) == Points{{"tuning", 0, 6000}});
	// The edit of both notes is one undo step.
	run(client, "history.undo");
	CHECK(pointsOf(noteOf(second)) == Points{{"tuning", 0, 5000}});
	CHECK(pointsOf(noteOf(first)).size() == 5);

	// A note named twice gets both changes in order.
	const Json twice =
		replace({{{"id", second}, {"from", 0}, {"to", 0}, {"points", Json::array({{{"position", 0}, {"value", 8.0}}})}},
			{{"id", second}, {"from", 0}, {"to", 10}, {"points", Json::array({{{"position", 10}, {"value", 9.0}}})}}});
	REQUIRE(twice.at("notes").size() == 1);
	CHECK(pointsOf(twice.at("notes").at(0)) == Points{{"tuning", 10, 9000}});
}

TEST_CASE("a note.replaceExpression that changes nothing leaves the project unmodified", "[integration]")
{
	// An erase over a span without points of the expression, as the expression editor sends it for each note that its
	// gesture went over: no revision, so no modified project and no undo step.
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json id = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions", {point("pressure", 100, 0.5), point("tuning", 200, 1.0)}}}}}})
						.at("notes")
						.at(0)
						.at("id");
	const Json before = run(client, "project.revision");
	const size_t steps = run(client, "history.get").at("steps").size();
	CHECK(run(client, "note.replaceExpression",
		{{"part", part}, {"type", "tuning"},
			{"notes", Json::array({{{"id", id}, {"from", 0}, {"to", 150}, {"points", Json::array()}}})}})
			.at("notes")
			.empty());
	// The point that the note has already changes nothing either.
	CHECK(run(client, "note.replaceExpression",
		{{"part", part}, {"type", "tuning"},
			{"notes",
				Json::array({{{"id", id}, {"from", 150}, {"to", 250},
					{"points", Json::array({{{"position", 200}, {"value", 1.0}}})}}})}})
			.at("notes")
			.empty());
	CHECK(run(client, "project.revision") == before);
	CHECK(run(client, "history.get").at("steps").size() == steps);
}

TEST_CASE("note.replaceExpression refuses what it can't do and changes nothing then", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json id = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions", {point("tuning", 100, 1.0)}}}}}})
						.at("notes")
						.at(0)
						.at("id");
	const auto notes = [&] { return run(client, "project.get").at("midiParts").at(0).at("notes"); };
	const Json before = notes();
	const Json revision = run(client, "project.revision");
	// After a valid change, which the refusal must leave out too.
	const auto replace = [&](Json change, const std::string& type = "tuning")
	{
		Json changes = Json::array({{{"id", id}, {"from", 0}, {"to", 200}, {"points", Json::array()}}});
		changes.push_back(std::move(change));
		return reject(client, "note.replaceExpression", {{"part", part}, {"type", type}, {"notes", changes}});
	};
	CHECK_THAT(replace({{"id", 999999}, {"from", 0}, {"to", 10}, {"points", Json::array()}}),
		ContainsSubstring("no note 999999"));
	CHECK_THAT(replace({{"id", id}, {"from", 10}, {"to", 5}, {"points", Json::array()}}),
		ContainsSubstring("notes[1].to must not be before its from"));
	CHECK_THAT(
		replace({{"id", id}, {"from", 10}, {"to", 20}, {"points", Json::array({{{"position", 30}, {"value", 1.0}}})}}),
		ContainsSubstring("notes[1].points[0].position must be from 10 to 20"));
	CHECK_THAT(replace({{"id", id}, {"from", -20}, {"to", 20},
				   {"points", Json::array({{{"position", -10}, {"value", 1.0}}})}}),
		ContainsSubstring("must not be negative"));
	CHECK_THAT(
		replace({{"id", id}, {"from", 0}, {"to", 20}, {"points", Json::array({{{"position", 10}, {"value", 121.0}}})}}),
		ContainsSubstring("notes[1].points[0].value"));
	CHECK_THAT(
		replace({{"id", id}, {"from", 0}, {"to", 20}, {"points", Json::array({{{"position", 10}, {"value", 2.0}}})}},
			"pressure"),
		ContainsSubstring("notes[1].points[0].value"));
	CHECK_THAT(reject(client, "note.replaceExpression",
				   {{"part", part}, {"type", "wobble"},
					   {"notes", Json::array({{{"id", id}, {"from", 0}, {"to", 10}, {"points", Json::array()}}})}}),
		ContainsSubstring("type"));
	CHECK_THAT(reject(client, "note.replaceExpression",
				   {{"part", 999999}, {"type", "tuning"},
					   {"notes", Json::array({{{"id", id}, {"from", 0}, {"to", 10}, {"points", Json::array()}}})}}),
		ContainsSubstring("no MIDI part 999999"));
	CHECK(notes() == before);
	CHECK(run(client, "project.revision") == revision);
}

TEST_CASE("note.replaceExpression reaches the shared copies of the part", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	const Json id = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5},
					{"expressions", {point("tuning", 100, 1.0)}}}}}})
						.at("notes")
						.at(0)
						.at("id");
	run(client, "event.copy", {{"events", {part}}, {"ticks", 4 * g_beatTicks}, {"shared", true}});
	run(client, "note.replaceExpression",
		{{"part", part}, {"type", "volume"},
			{"notes",
				Json::array({{{"id", id}, {"from", 0}, {"to", 0},
					{"points", Json::array({{{"position", 0}, {"value", 3.5}}})}}})}});
	const Json parts = run(client, "project.get").at("midiParts");
	REQUIRE(parts.size() == 2);
	for (const Json& each : parts)
	{
		INFO("part " << each.at("id"));
		REQUIRE(each.at("notes").size() == 1);
		CHECK(pointsOf(each.at("notes").at(0)) == Points{{"tuning", 100, 1000}, {"volume", 0, 3500}});
	}
}

namespace
{
	// The track's note expression setup as project.get lists it with its channel.
	Json setupOf(Client& client, ObjectId channel)
	{
		const Json project = run(client, "project.get");
		for (const Json& each : project.at("channels"))
			if (each.at("id") == channel)
				return each.at("noteExpression");
		return nullptr;
	}

	Json input(const char* type, const char* message, double min, double max)
	{
		return {{"type", type}, {"input", message}, {"min", min}, {"max", max}};
	}
}

TEST_CASE("noteExpression.set sets a track's bypass and input assignments, which the project keeps", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	CHECK(setupOf(client, setup.channel) ==
		Json{{"bypass", false}, {"inputs", Json::array()}, {"hidden", Json::array()}, {"colors", Json::array()}});

	events(client);
	const Json set = run(client, "noteExpression.set",
		{{"track", setup.track}, {"bypass", true},
			{"inputs", {input("tuning", "pitchBend", -12.0, 12.0), input("brightness", "cc74", 0.25, 0.75)}}});
	const Json expected = {{"bypass", true},
		{"inputs", {input("tuning", "pitchBend", -12.0, 12.0), input("brightness", "cc74", 0.25, 0.75)}},
		{"hidden", Json::array()}, {"colors", Json::array()}};
	CHECK(set == Json{{"track", setup.track}, {"noteExpression", expected}});
	// Clients that follow events, like the inspector, see the new setup.
	const std::vector<Json> changes = events(client)["channel.changed"];
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].at("channel").at("id") == setup.channel);
	CHECK(changes[0].at("channel").at("noteExpression") == expected);
	CHECK(setupOf(client, setup.channel) == expected);
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Change note expression setup");

	// What a call leaves out stays; an expression back at no input and its full range is no longer listed.
	run(client, "noteExpression.set",
		{{"track", setup.track},
			{"inputs",
				{{{"type", "brightness"}, {"max", 1.0}}, {{"type", "tuning"}, {"input", "none"}, {"min", -120.0}},
					{{"type", "tuning"}, {"max", 120.0}}, {{"type", "pressure"}, {"input", "aftertouch"}}}}});
	const Json changed = {{"bypass", true},
		{"inputs", {input("brightness", "cc74", 0.25, 1.0), input("pressure", "aftertouch", 0.0, 1.0)}},
		{"hidden", Json::array()}, {"colors", Json::array()}};
	CHECK(setupOf(client, setup.channel) == changed);

	// Saved and loaded with the project.
	const Project loaded = projectFromJson(run(client, "project.get"));
	CHECK(toJson(loaded.channels.at(setup.channel)).at("noteExpression") == changed);

	run(client, "history.undo");
	CHECK(setupOf(client, setup.channel) == expected);
	run(client, "history.undo");
	CHECK(setupOf(client, setup.channel) ==
		Json{{"bypass", false}, {"inputs", Json::array()}, {"hidden", Json::array()}, {"colors", Json::array()}});
}

TEST_CASE(
	"a project loads a note expression setup out of range clamped and what it doesn't know left out", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	Json json = run(client, "project.get");
	for (Json& each : json.at("channels"))
		if (each.at("id") == setup.channel)
			each["noteExpression"] = {{"bypass", true},
				{"inputs",
					{input("volume", "cc7", -1.0, 9.0), input("wobble", "cc1", 0.0, 1.0),
						input("pan", "cc200", 0.0, 0.5)}},
				{"hidden", {"wobble", "tuning"}},
				{"colors",
					{{{"type", "pan"}, {"color", "green"}}, {{"type", "wobble"}, {"color", "#ff0000"}},
						{{"type", "tuning"}, {"color", "#0000ff"}}}}};
	const Project loaded = projectFromJson(json);
	CHECK(toJson(loaded.channels.at(setup.channel)).at("noteExpression") ==
		Json{{"bypass", true}, {"inputs", {input("volume", "cc7", 0.0, 4.0), input("pan", "none", 0.0, 0.5)}},
			{"hidden", Json::array({"tuning"})}, {"colors", {{{"type", "tuning"}, {"color", "#0000ff"}}}}});
}

TEST_CASE("noteExpression.set hides expressions from the editors, which the project keeps", "[integration]")
{
	// The inspector's visibility (#2120, reference manual p. 1301): hidden lists them in the expressions' order.
	Setup setup;
	Client& client = setup.client;
	events(client);
	run(client, "noteExpression.set", {{"track", setup.track}, {"hidden", {"vibrato", "pan"}}});
	const auto hidden = [&] { return setupOf(client, setup.channel).at("hidden"); };
	CHECK(hidden() == Json{"pan", "vibrato"});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Change note expression setup");
	// The editors read the notes again after it: not only channel.changed.
	CHECK(events(client)["noteExpression.changed"].size() == 1);
	// A call without it keeps it; saved and loaded with the project.
	run(client, "noteExpression.set", {{"track", setup.track}, {"bypass", true}});
	CHECK(hidden() == Json{"pan", "vibrato"});
	CHECK(events(client)["noteExpression.changed"].empty());
	const Project loaded = projectFromJson(run(client, "project.get"));
	CHECK(toJson(loaded.channels.at(setup.channel)).at("noteExpression").at("hidden") == Json{"pan", "vibrato"});
	CHECK_THAT(reject(client, "noteExpression.set", {{"track", setup.track}, {"hidden", {"pan", "wobble"}}}),
		ContainsSubstring("hidden"));
	run(client, "noteExpression.set", {{"track", setup.track}, {"hidden", Json::array()}});
	CHECK(hidden() == Json::array());
	run(client, "history.undo");
	CHECK(hidden() == Json{"pan", "vibrato"});
}

TEST_CASE("noteExpression.set gives expressions their parameter colors, which the project keeps", "[integration]")
{
	// Parameter Color (#2120, reference manual p. 1301): colors lists them in the expressions' order.
	Setup setup;
	Client& client = setup.client;
	events(client);
	run(client, "noteExpression.set",
		{{"track", setup.track},
			{"colors", {{{"type", "pan"}, {"color", "#00ff00"}}, {{"type", "volume"}, {"color", "#ff0000"}}}}});
	const auto colors = [&] { return setupOf(client, setup.channel).at("colors"); };
	const Json both = {{{"type", "volume"}, {"color", "#ff0000"}}, {{"type", "pan"}, {"color", "#00ff00"}}};
	CHECK(colors() == both);
	// The editors draw the expressions in them: they read the notes again.
	CHECK(events(client)["noteExpression.changed"].size() == 1);
	const Project loaded = projectFromJson(run(client, "project.get"));
	CHECK(toJson(loaded.channels.at(setup.channel)).at("noteExpression").at("colors") == both);
	// An empty color is the default again; a call without colors keeps them.
	run(client, "noteExpression.set", {{"track", setup.track}, {"colors", {{{"type", "volume"}, {"color", ""}}}}});
	run(client, "noteExpression.set", {{"track", setup.track}, {"bypass", true}});
	CHECK(colors() == Json{{{"type", "pan"}, {"color", "#00ff00"}}});
	CHECK_THAT(reject(client, "noteExpression.set",
				   {{"track", setup.track}, {"colors", {{{"type", "pan"}, {"color", "green"}}}}}),
		ContainsSubstring("#rrggbb"));
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(colors() == both);
}

TEST_CASE("an input assignment saved as a file loads into another track, replacing its assignments", "[integration]")
{
	// Save and Load Input Assignment (#2120, reference manual pp. 1302, 1304).
	Setup setup;
	Client& client = setup.client;
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "keyboard.neinput").u8string();
	run(client, "noteExpression.set",
		{{"track", setup.track}, {"bypass", true}, {"hidden", {"pan"}},
			{"colors", {{{"type", "pan"}, {"color", "#00ff00"}}}},
			{"inputs", {input("tuning", "pitchBend", -12.0, 12.0), input("brightness", "cc74", 0.25, 0.75)}}});
	const Json revision = run(client, "project.revision");
	run(client, "noteExpression.saveInputs", {{"track", setup.track}, {"path", file}});
	CHECK(run(client, "project.revision") == revision);

	const Json added = run(client, "track.add", {{"type", "instrument"}});
	const auto other = added.at("track").at("id").get<ObjectId>();
	const auto channel = added.at("channel").at("id").get<ObjectId>();
	run(client, "noteExpression.set", {{"track", other}, {"inputs", {input("volume", "cc7", 0.0, 4.0)}}});
	run(client, "noteExpression.loadInputs", {{"track", other}, {"path", file}});
	// Only the inputs: the bypass, visibility and colors stay the track's own.
	CHECK(setupOf(client, channel) ==
		Json{{"bypass", false},
			{"inputs", {input("tuning", "pitchBend", -12.0, 12.0), input("brightness", "cc74", 0.25, 0.75)}},
			{"hidden", Json::array()}, {"colors", Json::array()}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Load input assignment");
	run(client, "history.undo");
	CHECK(setupOf(client, channel).at("inputs") == Json::array({input("volume", "cc7", 0.0, 4.0)}));

	// A file that isn't one changes nothing.
	const std::string broken = (dir.path() / "broken.neinput").u8string();
	std::ofstream(std::filesystem::u8path(broken), std::ios::binary) << "not json";
	CHECK_THAT(reject(client, "noteExpression.loadInputs", {{"track", other}, {"path", broken}}),
		ContainsSubstring("broken.neinput"));
	CHECK_THAT(reject(client, "noteExpression.loadInputs",
				   {{"track", other}, {"path", (dir.path() / "missing.neinput").u8string()}}),
		ContainsSubstring("can't open"));
	CHECK(setupOf(client, channel).at("inputs") == Json::array({input("volume", "cc7", 0.0, 4.0)}));
}

TEST_CASE("noteExpression.set refuses what it can't do and changes nothing then", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	const Json revision = run(client, "project.revision");
	CHECK_THAT(reject(client, "noteExpression.set", {{"track", audio}, {"bypass", true}}),
		ContainsSubstring("no MIDI or instrument track"));
	CHECK_THAT(reject(client, "noteExpression.set", {{"track", 999999}, {"bypass", true}}),
		ContainsSubstring("no MIDI or instrument track"));
	CHECK_THAT(reject(client, "noteExpression.set",
				   {{"track", setup.track}, {"bypass", true}, {"inputs", {{{"type", "tuning"}, {"input", "cc128"}}}}}),
		ContainsSubstring("input"));
	CHECK_THAT(reject(client, "noteExpression.set",
				   {{"track", setup.track}, {"inputs", {{{"type", "volume"}, {"max", 4.5}}}}}),
		ContainsSubstring("max must be 0 to 4"));
	CHECK_THAT(reject(client, "noteExpression.set",
				   {{"track", setup.track}, {"inputs", {{{"type", "tuning"}, {"min", -121.0}}}}}),
		ContainsSubstring("min must be -120 to 120"));
	CHECK(setupOf(client, setup.channel) ==
		Json{{"bypass", false}, {"inputs", Json::array()}, {"hidden", Json::array()}, {"colors", Json::array()}});
	CHECK(run(client, "project.revision") == revision);
}

TEST_CASE("a track's note expression bypass plays its notes without their expression", "[integration]")
{
	for (const bool bypass : {true, false})
	{
		Setup setup;
		Client& client = setup.client;
		const ObjectId part = addPart(client, setup.track);
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", ticksOf(100)}, {"length", ticksOf(1000)}, {"pitch", 62}, {"velocity", 0.5},
						{"channel", 3},
						{"expressions", {point("tuning", 0, -48.0), point("brightness", ticksOf(500), 1.0)}}}}}});
		run(client, "noteExpression.set", {{"track", setup.track}, {"bypass", bypass}});
		run(client, "audio.render", {{"frames", 64}});
		const auto start = run(client, "audio.get").at("position").get<int64_t>();
		run(client, "midi.capture", {{"start", true}});
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", 2000}});
		std::vector<std::vector<int64_t>> played;
		for (const std::vector<int64_t>& each : capturedFrom(client, start))
			played.push_back({each[0], each[1], each.size() > 2 ? each[2] : -1});
		INFO("bypass " << bypass);
		if (bypass)
			CHECK(played == std::vector<std::vector<int64_t>>{{100, 0x93, 62}, {1100, 0x83, 62}});
		else
			CHECK(played ==
				std::vector<std::vector<int64_t>>{
					{100, 0xE3, 0}, {100, 0xB3, 74}, {100, 0x93, 62}, {600, 0xB3, 74}, {1100, 0x83, 62}});
	}
}

TEST_CASE("a track's MIDI input plays the assigned messages as the expression of its held notes, over their ranges",
	"[integration]")
{
	for (const bool bypass : {false, true})
	{
		Setup setup;
		Client& client = setup.client;
		run(client, "noteExpression.set",
			{{"track", setup.track}, {"bypass", bypass},
				{"inputs",
					{input("brightness", "cc1", 0.0, 0.5), input("pan", "cc1", 1.0, 0.0),
						input("tuning", "pitchBend", -12.0, 12.0), input("pressure", "aftertouch", 0.0, 1.0)}}});
		run(client, "audio.render", {{"frames", 64}});
		run(client, "midi.capture", {{"start", true}});
		// CC 1 before the note finds none to play on; CC 7 isn't assigned and passes. Pitch bend at its center plays
		// a tuning of 0 on the held notes, aftertouch their pressure.
		run(client, "midi.inject",
			{{"channel", setup.channel},
				{"messages",
					{message(0, {0xB2, 1, 127}), message(0, {0x92, 60, 100}), message(0, {0x92, 64, 100}),
						message(0, {0xB2, 1, 127}), message(0, {0xB2, 7, 100}), message(0, {0x82, 60, 0}),
						message(0, {0x82, 64, 0}), message(0, {0x92, 67, 100}), message(0, {0xE2, 0, 64}),
						message(0, {0xD2, 100}), message(0, {0x82, 67, 0})}}});
		run(client, "audio.render", {{"frames", 128}});
		std::vector<std::vector<int64_t>> played;
		for (std::vector<int64_t>& each : capturedFrom(client, 0))
			played.emplace_back(each.begin() + 1, each.end());
		INFO("bypass " << bypass);
		// Brightness at the top of its range, half, as MIDI 1.0 CC 74 on each note, which the capture puts before a
		// note-on of the same frame; pan reversed, left, which MIDI 1.0 doesn't have. With the bypass the notes play
		// without it.
		if (bypass)
			CHECK(played ==
				std::vector<std::vector<int64_t>>{{0x92, 60, 100}, {0x92, 64, 100}, {0xB2, 7, 100}, {0x82, 60, 0},
					{0x82, 64, 0}, {0x92, 67, 100}, {0x82, 67, 0}});
		else
			CHECK(played ==
				std::vector<std::vector<int64_t>>{{0xB2, 74, 64}, {0x92, 60, 100}, {0xB2, 74, 64}, {0x92, 64, 100},
					{0xB2, 7, 100}, {0x82, 60, 0}, {0x82, 64, 0}, {0xE2, 0, 64}, {0xD2, 100}, {0x92, 67, 100},
					{0x82, 67, 0}});
	}
}

TEST_CASE("the note-offs of a block that assigned controllers fill still play", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "noteExpression.set", {{"track", setup.track}, {"inputs", {input("brightness", "cc1", 0.0, 1.0)}}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "midi.capture", {{"start", true}});
	// Ten held notes and 110 CC 1 make 1100 expression events, more than a block holds, before the note-offs.
	Json messages = Json::array();
	for (int key = 60; key < 70; ++key)
		messages.push_back(message(0, {0x90, key, 100}));
	for (int i = 0; i < 110; ++i)
		messages.push_back(message(0, {0xB0, 1, i}));
	for (int key = 60; key < 70; ++key)
		messages.push_back(message(0, {0x80, key, 0}));
	run(client, "midi.inject", {{"channel", setup.channel}, {"messages", messages}});
	run(client, "audio.render", {{"frames", 128}});
	std::vector<int64_t> ended;
	for (const std::vector<int64_t>& each : capturedFrom(client, 0))
		if (each.size() > 2 && each[1] == 0x80)
			ended.push_back(each[2]);
	CHECK(ended == std::vector<int64_t>{60, 61, 62, 63, 64, 65, 66, 67, 68, 69});
}

TEST_CASE("a controller assigned to an expression records into the notes that sound as their expression points",
	"[integration]")
{
	// The bypass leaves it out of what plays, not of what records.
	const bool bypass = GENERATE(false, true);
	INFO("bypass " << bypass);
	Setup setup;
	Client& client = setup.client;
	run(client, "noteExpression.set",
		{{"track", setup.track}, {"bypass", bypass}, {"inputs", {input("brightness", "cc1", 0.0, 1.0)}}});
	run(client, "transport.record");
	run(client, "midi.inject",
		{{"channel", setup.channel},
			{"messages",
				{message(1000, {0x91, 60, 100}), message(1000, {0x91, 64, 100}), message(2000, {0xB1, 1, 127}),
					message(3000, {0x81, 60, 0}), message(3500, {0xB1, 1, 0}), message(4000, {0x81, 64, 0})}}});
	run(client, "audio.render", {{"frames", 5000}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());
	const Json part = run(client, "project.get").at("midiParts").at(0);
	// Both notes get the value while they sound, the one still held the next one too; nothing records as a controller.
	CHECK(part.at("controllers").empty());
	REQUIRE(part.at("notes").size() == 2);
	std::map<int, Points> points;
	for (const Json& note : part.at("notes"))
		points[note.at("pitch").get<int>()] = pointsOf(note);
	CHECK(points[60] == Points{{"brightness", ticksOf(1000), 1000}});
	CHECK(points[64] == Points{{"brightness", ticksOf(1000), 1000}, {"brightness", ticksOf(2500), 0}});
}

TEST_CASE("noteExpression.convert turns a part's assigned controllers into the expression of the notes that sound",
	"[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId part = addPart(client, setup.track);
	run(client, "noteExpression.set",
		{{"track", setup.track},
			{"inputs",
				{input("brightness", "cc1", 0.0, 0.5), input("tuning", "pitchBend", -12.0, 12.0),
					input("pressure", "aftertouch", 0.0, 1.0)}}});
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.5}, {"channel", 0}},
					{{"position", 0}, {"length", 2 * g_beatTicks}, {"pitch", 64}, {"velocity", 0.5},
						{"channel", 1}}}}});
	// CC 1 on each note's channel; CC 7 isn't assigned, and the last CC 1 has no note sounding. Pitch bend at its
	// center is a tuning of 0, aftertouch the pressure.
	run(client, "controller.add",
		{{"part", part},
			{"controllers",
				{{{"position", g_beatTicks / 2}, {"number", 1}, {"value", 1.0}, {"channel", 0}},
					{{"position", 3 * g_beatTicks / 2}, {"number", 1}, {"value", 0.5}, {"channel", 1}},
					{{"position", g_beatTicks / 2}, {"number", 7}, {"value", 0.5}, {"channel", 0}},
					{{"position", 3 * g_beatTicks}, {"number", 1}, {"value", 0.5}, {"channel", 0}},
					{{"position", g_beatTicks / 4}, {"type", "pitchBend"}, {"value", 0.5}, {"channel", 0}},
					{{"position", g_beatTicks}, {"type", "channelPressure"}, {"value", 0.25}, {"channel", 1}}}}});
	const Json before = run(client, "project.get").at("midiParts").at(0);

	const Json converted = run(client, "noteExpression.convert", {{"part", part}});
	CHECK(converted.at("notes").size() == 2);
	const Json after = run(client, "project.get").at("midiParts").at(0);
	std::map<int, Points> points;
	for (const Json& note : after.at("notes"))
		points[note.at("pitch").get<int>()] = pointsOf(note);
	CHECK(points[60] == Points{{"brightness", g_beatTicks / 2, 500}, {"tuning", g_beatTicks / 4, 0}});
	CHECK(points[64] == Points{{"brightness", 3 * g_beatTicks / 2, 250}, {"pressure", g_beatTicks, 250}});
	std::vector<std::pair<int64_t, int>> left;
	for (const Json& controller : after.at("controllers"))
		left.emplace_back(controller.at("position").get<int64_t>(), controller.at("number").get<int>());
	std::sort(left.begin(), left.end());
	CHECK(left == std::vector<std::pair<int64_t, int>>{{g_beatTicks / 2, 7}, {3 * g_beatTicks, 1}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Convert to note expression");

	run(client, "history.undo");
	CHECK(run(client, "project.get").at("midiParts").at(0) == before);
	// Nothing to convert changes nothing.
	run(client, "noteExpression.set",
		{{"track", setup.track},
			{"inputs",
				{{{"type", "brightness"}, {"input", "none"}}, {{"type", "tuning"}, {"input", "none"}},
					{{"type", "pressure"}, {"input", "none"}}}}});
	CHECK(run(client, "noteExpression.convert", {{"part", part}}).at("notes").empty());
	CHECK(run(client, "project.get").at("midiParts").at(0) == before);
}
