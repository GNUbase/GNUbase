// SPDX-License-Identifier: GPL-3.0-or-later
// Time warp, Process Tempo, Process Bars, Set Definition from Tempo and copying warp markers (#159,
// docs/features/time-warp.md).
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;
using gnubase::time::TempoMap;
using gnubase::time::TempoPoint;

namespace
{
	constexpr int64_t g_quarterSamples = 24000; // at 120 BPM and 48 kHz
	constexpr int64_t g_q = g_ticksPerQuarter;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId addTrack(Client& client, Json parameters)
	{
		return run(client, "track.add", std::move(parameters)).at("track").at("id").get<ObjectId>();
	}

	ObjectId addPart(Client& client, ObjectId track, int64_t quarters)
	{
		return run(client, "midiPart.add", {{"track", track}, {"position", quarters * g_q}, {"length", g_q}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	}

	ObjectId addClip(Client& client, const std::string& path, int64_t length)
	{
		const Json media =
			run(client, "media.add", {{"path", path}, {"sampleRate", 48000.0}, {"channels", 1}, {"length", length}})
				.at("media");
		return run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<ObjectId>();
	}

	ObjectId addEvent(Client& client, ObjectId track, ObjectId clip, int64_t position, int64_t offset, int64_t length)
	{
		return run(client, "audioEvent.add",
			{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}})
			.at("event")
			.at("id")
			.get<ObjectId>();
	}

	Json byId(Client& client, const char* list, ObjectId id)
	{
		Json found;
		const Json project = run(client, "project.get");
		for (const Json& each : project.at(list))
			if (each.at("id") == id)
				found = each;
		INFO("no " << id << " in " << list);
		REQUIRE(found.is_object());
		return found;
	}

	TempoMap tempoOf(const Json& points)
	{
		std::vector<TempoPoint> tempo;
		for (const Json& point : points)
			tempo.push_back(
				{point.at("position").get<int64_t>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		return TempoMap(tempo);
	}

	TempoMap tempoTrack(Client& client)
	{
		return tempoOf(run(client, "project.get").at("tempoTrack"));
	}

	// Where the first audio event, or else MIDI part, of the track starts and how long it plays, in project samples.
	std::pair<int64_t, int64_t> rendered(Engine& engine, ObjectId track)
	{
		const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		const auto channel = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
			[track](const RenderChannel& each) { return each.track == track; });
		REQUIRE(channel != snapshot.channels.end());
		if (!channel->audioEvents.empty())
			return {channel->audioEvents.front().start, channel->audioEvents.front().length};
		REQUIRE(!channel->midiParts.empty());
		return {channel->midiParts.front().start, 0};
	}

	Json signatures(Client& client)
	{
		return run(client, "project.get").at("signatures");
	}

	Json signature(int64_t bar, int numerator, int denominator)
	{
		return {{"bar", bar}, {"numerator", numerator}, {"denominator", denominator}};
	}
}

TEST_CASE("time warp makes a musical position play at a time by the tempo event before it", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(
		reject(client, "tempo.warp", {{"position", 4 * g_q}, {"to", 120000}}), ContainsSubstring("tempo track mode"));
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId part = addPart(client, midi, 8);
	run(client, "midiPart.add", {{"track", linear}, {"position", 8 * g_quarterSamples}, {"length", 1000}});

	// Warp Grid: the fourth quarter to 2.5 seconds makes the first tempo event 96 BPM; the part keeps its time.
	Json result = run(client, "tempo.warp", {{"position", 4 * g_q}, {"to", 120000}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Time Warp");
	CHECK(result.at("event") == 0);
	CHECK(std::abs(result.at("bpm").get<double>() - 96.0) < 1e-6);
	CHECK(tempoTrack(client).points().size() == 1);
	CHECK(tempoTrack(client).toSamples(4 * g_q, 48000.0) == 120000);
	CHECK(rendered(engine, midi).first == 8 * g_quarterSamples);
	CHECK(rendered(engine, linear).first == 8 * g_quarterSamples);
	CHECK(byId(client, "tracks", midi).at("timeBase") == "musical");
	CHECK(std::abs(byId(client, "midiParts", part).at("position").get<int64_t>() - (32 * g_q / 5)) < g_q / 1000);

	// Warp Grid, musical events follow: the part keeps its position, 2.5 seconds per four quarters.
	run(client, "history.undo");
	run(client, "tempo.warp", {{"position", 4 * g_q}, {"to", 120000}, {"follow", true}});
	CHECK(byId(client, "midiParts", part).at("position") == 8 * g_q);
	CHECK(rendered(engine, midi).first == 240000);
	CHECK(rendered(engine, linear).first == 8 * g_quarterSamples);

	// With a later tempo event, a tempo event at the position keeps the tempo after it.
	run(client, "history.undo");
	run(client, "tempo.add", {{"position", 8 * g_q}, {"bpm", 140.0}, {"ramp", false}});
	run(client, "tempo.warp", {{"position", 4 * g_q}, {"to", 120000}, {"follow", true}});
	const TempoMap warped = tempoTrack(client);
	REQUIRE(warped.points().size() == 3);
	CHECK(warped.points()[1].position == 4 * g_q);
	CHECK(warped.points()[1].bpm == 120.0);
	CHECK(warped.points()[2].bpm == 140.0);
	CHECK(std::abs(warped.toSeconds(8 * g_q) - 4.5) < 1e-6);

	CHECK_THAT(reject(client, "tempo.warp", {{"position", 4 * g_q}, {"to", 1000}}), ContainsSubstring("360 BPM"));
	CHECK_THAT(reject(client, "tempo.warp", {{"position", 0}, {"to", 1000}}), ContainsSubstring("after the project"));
}

TEST_CASE("process tempo fits a range into a new length and keeps the tempo after it", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(reject(client, "tempo.process", {{"start", 0}, {"end", 8 * g_q}, {"newLength", 5.0}}),
		ContainsSubstring("tempo track mode"));
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	addPart(client, midi, 12);
	run(client, "midiPart.add", {{"track", linear}, {"position", 12 * g_quarterSamples}, {"length", 1000}});

	// Eight quarters (4 s) to 5 s: 96 BPM, then 120 again; the musical part follows, the linear one stays.
	Json result = run(client, "tempo.process", {{"start", 0}, {"end", 8 * g_q}, {"newLength", 5.0}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Process Tempo");
	CHECK(std::abs(result.at("factor").get<double>() - 0.8) < 1e-12);
	TempoMap tempo = tempoTrack(client);
	REQUIRE(tempo.points().size() == 2);
	CHECK(tempo.points()[0].bpm == 96.0);
	CHECK(tempo.points()[1].position == 8 * g_q);
	CHECK(tempo.points()[1].bpm == 120.0);
	CHECK(rendered(engine, midi).first == 336000); // 5 s and two quarters
	CHECK(rendered(engine, linear).first == 12 * g_quarterSamples);

	// newEnd, from the locators; the same as newLength.
	run(client, "history.undo");
	run(client, "transport.set", {{"left", 0}, {"right", 8 * g_quarterSamples}});
	result = run(client, "tempo.process", {{"newEnd", 5.0}});
	CHECK(std::abs(result.at("factor").get<double>() - 0.8) < 1e-12);
	CHECK(rendered(engine, midi).first == 336000);

	// A ramp across the range's end: the range gets its length and what follows keeps its time.
	run(client, "history.undo");
	run(client, "tempo.add", {{"position", 16 * g_q}, {"bpm", 60.0}, {"ramp", true}});
	const TempoMap before = tempoTrack(client);
	run(client, "tempo.process", {{"start", 2 * g_q}, {"end", 8 * g_q}, {"newLength", 2.0}});
	tempo = tempoTrack(client);
	CHECK(std::abs(tempo.toSeconds(2 * g_q) - before.toSeconds(2 * g_q)) < 1e-9);
	CHECK(std::abs(tempo.toSeconds(8 * g_q) - tempo.toSeconds(2 * g_q) - 2.0) < 1e-5);
	CHECK(std::abs((tempo.toSeconds(20 * g_q) - tempo.toSeconds(8 * g_q)) -
			  (before.toSeconds(20 * g_q) - before.toSeconds(8 * g_q))) < 1e-5);

	CHECK_THAT(reject(client, "tempo.process", {{"start", 0}, {"end", 8 * g_q}, {"newLength", 5.0}, {"newEnd", 5.0}}),
		ContainsSubstring("not both"));
	CHECK_THAT(reject(client, "tempo.process", {{"start", 0}, {"end", 8 * g_q}, {"newLength", 0.001}}),
		ContainsSubstring("999 BPM"));
	CHECK_THAT(reject(client, "tempo.process", {{"start", 8 * g_q}, {"end", 8 * g_q}, {"newLength", 1.0}}),
		ContainsSubstring("end after its start"));
}

TEST_CASE("process bars inserts, deletes, replaces and reinterprets bars", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", 16 * g_q}, {"bpm", 100.0}, {"ramp", false}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId part = addPart(client, midi, 8);

	SECTION("insert")
	{
		const Json result = run(client, "tempo.processBars",
			{{"bar", 2}, {"bars", 1}, {"type", "insert"}, {"numerator", 3}, {"denominator", 4}});
		CHECK(run(client, "history.get").at("steps").back().at("action") == "Process Bars");
		CHECK(result.at("signatures") == Json::array({signature(0, 4, 4), signature(2, 3, 4), signature(3, 4, 4)}));
		const TempoMap tempo = tempoTrack(client);
		CHECK(tempo.bpmAt(19 * g_q) == 100.0);
		CHECK(tempo.bpmAt(19 * g_q - 1) == 120.0);
		CHECK(byId(client, "midiParts", part).at("position") == 8 * g_q); // positions stay

		// Deleting the bar again brings the tempo event back to where it was.
		run(client, "tempo.processBars", {{"bar", 2}, {"bars", 1}, {"type", "delete"}});
		CHECK(signatures(client) == Json::array({signature(0, 4, 4), signature(2, 4, 4)}));
		CHECK(tempoTrack(client).bpmAt(16 * g_q) == 100.0);
		CHECK(tempoTrack(client).bpmAt(16 * g_q - 1) == 120.0);
		CHECK(rendered(engine, midi).first == 8 * g_quarterSamples);
	}
	SECTION("replace")
	{
		run(client, "tempo.processBars",
			{{"bar", 1}, {"bars", 2}, {"type", "replace"}, {"numerator", 3}, {"denominator", 4}});
		CHECK(signatures(client) == Json::array({signature(0, 4, 4), signature(1, 3, 4), signature(3, 4, 4)}));
		CHECK(tempoTrack(client).bpmAt(14 * g_q) == 100.0); // two quarters fewer before it
		CHECK(tempoTrack(client).bpmAt(14 * g_q - 1) == 120.0);
		CHECK(rendered(engine, midi).first == 8 * g_quarterSamples);
	}
	SECTION("reinterpret")
	{
		// Two bars of 4/4 as two bars of 3/4 at 90 BPM: they last as long, and the part keeps its time.
		run(client, "tempo.processBars",
			{{"bar", 0}, {"bars", 2}, {"type", "reinterpret"}, {"numerator", 3}, {"denominator", 4}});
		CHECK(signatures(client) == Json::array({signature(0, 3, 4), signature(2, 4, 4)}));
		const TempoMap tempo = tempoTrack(client);
		CHECK(tempo.bpmAt(0) == 90.0);
		CHECK(tempo.bpmAt(6 * g_q) == 120.0);
		CHECK(tempo.bpmAt(14 * g_q) == 100.0);
		CHECK(std::abs(rendered(engine, midi).first - (8 * g_quarterSamples)) <= 1);
		CHECK(std::abs(byId(client, "midiParts", part).at("position").get<int64_t>() - (6 * g_q)) < g_q / 1000);
		CHECK(byId(client, "tracks", midi).at("timeBase") == "musical");
	}
	SECTION("errors")
	{
		CHECK_THAT(reject(client, "tempo.processBars", {{"bar", 0}, {"bars", 0}, {"type", "insert"}}),
			ContainsSubstring("at least 1"));
		CHECK_THAT(
			reject(client, "tempo.processBars", {{"bar", 0}, {"bars", 1}, {"type", "insert"}, {"denominator", 3}}),
			ContainsSubstring("1, 2, 4, 8 or 16"));
	}
}

TEST_CASE("set definition from tempo makes audio play as before and follow the tempo from then on", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", 2 * g_q}, {"bpm", 60.0}, {"ramp", false}});
	const ObjectId track = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}});
	const ObjectId other = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}});
	const ObjectId clip = addClip(client, "/audio/free.wav", 4 * g_quarterSamples);
	const ObjectId event = addEvent(client, track, clip, 0, 0, 4 * g_quarterSamples);

	// Two seconds of audio: two quarters at 120 BPM and one at 60.
	const Json result = run(client, "event.setDefinitionFromTempo", {{"events", {event}}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Set Definition from Tempo");
	CHECK(result.at("clips") == Json::array({clip}));
	const Json warp = byId(client, "clips", clip).at("warp");
	CHECK(warp.at("musical") == true);
	CHECK(warp.at("tempo") == 60.0); // the tempo after the last beat
	CHECK(warp.at("gridStart") == 0);
	REQUIRE(warp.at("beats").size() == 2);
	CHECK(warp.at("beats").at(0) == Json{{"beat", 1}, {"frame", 24000}});
	CHECK(warp.at("beats").at(1) == Json{{"beat", 2}, {"frame", 48000}});
	CHECK(byId(client, "tracks", track).at("timeBase") == "musical");
	CHECK(byId(client, "tracks", other).at("timeBase") == "linear");
	CHECK(std::abs(rendered(engine, track).second - (4 * g_quarterSamples)) <= 1);

	// The slow quarter at 120 BPM: the event plays its last second in half a second.
	run(client, "tempo.set", {{"events", {{{"position", 2 * g_q}, {"bpm", 120.0}}}}});
	CHECK(std::abs(rendered(engine, track).second - (3 * g_quarterSamples)) <= 2);

	CHECK_THAT(
		reject(client, "event.setDefinitionFromTempo", {{"events", {event}}}), ContainsSubstring("musical mode"));
	run(client, "history.undo");
	run(client, "history.undo");
	run(client, "event.setDefinitionFromTempo", {{"events", {event}}, {"allTracks", true}});
	CHECK(byId(client, "tracks", other).at("timeBase") == "musical");
}

TEST_CASE("warp markers are pasted into other events at the same times", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}});
	const ObjectId other = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}});
	const ObjectId source = addClip(client, "/audio/a.wav", 4 * g_quarterSamples);
	const ObjectId target = addClip(client, "/audio/b.wav", 5 * g_quarterSamples);
	const ObjectId from = addEvent(client, track, source, 0, 0, 4 * g_quarterSamples);
	const ObjectId to = addEvent(client, other, target, 0, 10000, 4 * g_quarterSamples);

	CHECK_THAT(reject(client, "event.pasteWarpMarkers", {{"from", from}, {"events", {to}}}),
		ContainsSubstring("no warp markers"));
	run(client, "clip.addWarpMarker", {{"clip", source}, {"position", 52000}, {"target", 49000}});
	run(client, "clip.addWarpMarker", {{"clip", target}, {"position", 20000}});

	const Json result = run(client, "event.pasteWarpMarkers", {{"from", from}, {"events", {from, to}}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Paste Warp Markers");
	CHECK(result.at("markers") == 1);
	CHECK(byId(client, "clips", target).at("warp").at("markers") ==
		Json::array({{{"position", 20000}, {"target", 20000}}, {{"position", 62000}, {"target", 59000}}}));
	CHECK(byId(client, "clips", source).at("warp").at("markers").size() == 1);
	// Once is enough: a marker at the same position isn't pasted again.
	CHECK(run(client, "event.pasteWarpMarkers", {{"from", from}, {"events", {to}}}).at("markers") == 0);
	run(client, "history.undo"); // the paste that changed something
	CHECK(byId(client, "clips", target).at("warp").at("markers").size() == 1);
}
