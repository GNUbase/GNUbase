// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo track, its tempo events, the project tempo modes and tap tempo (#155).
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;

namespace
{
	// At the default 120 BPM and 48 kHz, a quarter note lasts 24000 samples.
	constexpr int64_t g_quarterSamples = 24000;

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

	Json point(int64_t quarters, double bpm, bool ramp = false)
	{
		return {{"position", quarters * g_ticksPerQuarter}, {"bpm", bpm}, {"ramp", ramp}};
	}

	std::vector<std::string> eventNames(Client& client)
	{
		std::vector<std::string> names;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				names.push_back(event->name);
		return names;
	}

	const RenderChannel& renderChannel(const RenderSnapshot& snapshot, ObjectId track)
	{
		const auto found = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
			[track](const RenderChannel& channel) { return channel.track == track; });
		REQUIRE(found != snapshot.channels.end());
		return *found;
	}
}

TEST_CASE("a project has one tempo track at most, without a channel, at the top level", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json added = run(client, "track.add", {{"type", "tempo"}});
	CHECK(added.at("track").at("name") == "Tempo");
	CHECK(added.at("channel").is_null());
	const ObjectId tempo = added.at("track").at("id").get<ObjectId>();
	CHECK(engine.renderSnapshots().acquire().channels.empty());
	CHECK_THAT(reject(client, "track.add", {{"type", "tempo"}}), ContainsSubstring("one tempo track"));
	CHECK_THAT(reject(client, "track.duplicate", {{"track", tempo}}), ContainsSubstring("one tempo track"));
	const ObjectId folder = addTrack(client, {{"type", "folder"}});
	CHECK_THAT(reject(client, "track.move", {{"track", tempo}, {"parent", folder}, {"index", 0}}),
		ContainsSubstring("folder"));

	// Removing the tempo track keeps the tempo: the track only shows it.
	run(client, "tempo.add", point(4, 90.0));
	run(client, "track.remove", {{"track", tempo}});
	CHECK(run(client, "project.get").at("tempoTrack").size() == 2);
	CHECK_THAT(reject(client, "track.add", {{"type", "tempo"}, {"parent", folder}}), ContainsSubstring("folder"));
}

TEST_CASE("fixed tempo and tempo track mode each keep their tempo", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	Json project = run(client, "project.get");
	CHECK(project.at("tempoTrackActive") == false); // a new project has a fixed tempo
	CHECK(project.at("fixedTempo") == 120.0);

	// In fixed tempo mode, the tempo track's events wait.
	run(client, "tempo.add", point(4, 60.0));
	project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({point(0, 120.0)}));
	CHECK(project.at("tempoTrack") == Json::array({point(0, 120.0), point(4, 60.0)}));

	eventNames(listener);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const std::vector<std::string> events = eventNames(listener);
	CHECK(std::find(events.begin(), events.end(), "tempo.changed") != events.end());
	project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({point(0, 120.0), point(4, 60.0)}));
	CHECK(engine.renderSnapshots().acquire().tempo.points().size() == 2);

	// The transport's tempo is the tempo track's first event in tempo track mode, the fixed tempo otherwise.
	run(client, "project.set", {{"tempo", 100.0}});
	CHECK(run(client, "project.get").at("tempo") == Json::array({point(0, 100.0), point(4, 60.0)}));
	run(client, "project.set", {{"tempoTrackActive", false}, {"tempo", 140.0}});
	project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({point(0, 140.0)}));
	CHECK(project.at("fixedTempo") == 140.0);
	CHECK(project.at("tempoTrack") == Json::array({point(0, 100.0), point(4, 60.0)}));

	run(client, "history.undo");
	project = run(client, "project.get");
	CHECK(project.at("tempoTrackActive") == true);
	CHECK(project.at("fixedTempo") == 120.0);
	CHECK(project.at("tempo") == Json::array({point(0, 100.0), point(4, 60.0)}));
}

TEST_CASE("editing the tempo moves musical-time events and keeps linear-time events in place", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId media = run(client, "media.add",
		{{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 192000}})
							   .at("media")
							   .at("id")
							   .get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	const ObjectId musical = addTrack(client, {{"type", "audio"}});
	const ObjectId linear = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	run(client, "audioEvent.add", {{"track", musical}, {"clip", clip}, {"position", 8 * g_ticksPerQuarter}});
	run(client, "audioEvent.add", {{"track", linear}, {"clip", clip}, {"position", 8 * g_quarterSamples}});
	const ObjectId part = run(client, "midiPart.add",
		{{"track", midi}, {"position", 8 * g_ticksPerQuarter}, {"length", 4 * g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes", {{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});

	const auto starts = [&engine, musical, linear, midi]
	{
		const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		return std::vector<int64_t>{renderChannel(snapshot, musical).audioEvents.at(0).start,
			renderChannel(snapshot, linear).audioEvents.at(0).start, renderChannel(snapshot, midi).notes.at(0).start};
	};
	CHECK(starts() == std::vector<int64_t>{8 * g_quarterSamples, 8 * g_quarterSamples, 8 * g_quarterSamples});

	// Half the tempo from the fourth quarter: what is musical starts four quarters of 24000 samples later.
	run(client, "tempo.add", point(4, 60.0, false));
	CHECK(starts() == std::vector<int64_t>{12 * g_quarterSamples, 8 * g_quarterSamples, 12 * g_quarterSamples});
	// Moving the tempo event moves them again, the linear event stays.
	run(client, "tempo.set", {{"events", {{{"position", 4 * g_ticksPerQuarter}, {"to", 6 * g_ticksPerQuarter}}}}});
	CHECK(starts() == std::vector<int64_t>{10 * g_quarterSamples, 8 * g_quarterSamples, 10 * g_quarterSamples});
	run(client, "tempo.remove", {{"positions", {6 * g_ticksPerQuarter}}});
	CHECK(starts() == std::vector<int64_t>{8 * g_quarterSamples, 8 * g_quarterSamples, 8 * g_quarterSamples});
}

TEST_CASE("notes and controllers in parts on linear tracks keep their time when the tempo changes", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId musical = addTrack(client, {{"type", "midi"}});
	const auto addPart = [&client](ObjectId track, int64_t position, int64_t length)
	{
		const ObjectId part =
			run(client, "midiPart.add", {{"track", track}, {"position", position}, {"length", length}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 60},
						{"velocity", 1.0}}}}});
		run(client, "controller.add",
			{{"part", part}, {"controllers", {{{"position", 2 * g_ticksPerQuarter}, {"number", 7}, {"value", 1.0}}}}});
		return part;
	};
	// A part from the eighth quarter, four quarters long: in samples on the linear track, in ticks on the other.
	addPart(linear, 8 * g_quarterSamples, 4 * g_quarterSamples);
	addPart(musical, 8 * g_ticksPerQuarter, 4 * g_ticksPerQuarter);

	// Where the note starts and ends and the controller is, in samples.
	const auto times = [&engine](ObjectId track)
	{
		const RenderChannel& channel = renderChannel(engine.renderSnapshots().acquire(), track);
		REQUIRE(channel.notes.size() == 1);
		REQUIRE(channel.controllers.size() == 1);
		return std::vector<int64_t>{
			channel.notes.at(0).start, channel.notes.at(0).end, channel.controllers.at(0).position};
	};
	const std::vector<int64_t> before{9 * g_quarterSamples, 10 * g_quarterSamples, 10 * g_quarterSamples};
	CHECK(times(linear) == before);
	CHECK(times(musical) == before);

	// Half the tempo from the fourth quarter, and a ramp inside the parts: the linear part's notes and controllers stay
	// where they were, the musical one's follow the tempo.
	run(client, "tempo.add", point(4, 60.0));
	run(client, "tempo.add", point(7, 90.0, true));
	const auto near = [](const std::vector<int64_t>& a, const std::vector<int64_t>& b)
	{
		return std::equal(
			a.begin(), a.end(), b.begin(), b.end(), [](int64_t x, int64_t y) { return std::abs(x - y) <= 1; });
	};
	CHECK(near(times(linear), before));
	CHECK_FALSE(near(times(musical), before));
	run(client, "project.set", {{"tempo", 200.0}});
	run(client, "tempo.set", {{"events", {{{"position", 7 * g_ticksPerQuarter}, {"bpm", 45.0}}}}});
	CHECK(near(times(linear), before));
	run(client, "tempo.remove", {{"positions", {4 * g_ticksPerQuarter}}});
	run(client, "project.set", {{"tempoTrackActive", false}});
	CHECK(near(times(linear), before));
	CHECK(times(musical) == before); // the fixed 120 BPM again

	// Undo puts the notes back as they were with each tempo.
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(near(times(linear), before));
}

TEST_CASE("a track's events, parts, markers and automation keep their time when its time base switches, and then "
		  "follow the tempo or not",
	"[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId media = run(client, "media.add",
		{{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 192000}})
							   .at("media")
							   .at("id")
							   .get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId markers = addTrack(client, {{"type", "marker"}});
	run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 8 * g_ticksPerQuarter}});
	const ObjectId part = run(client, "midiPart.add",
		{{"track", midi}, {"position", 8 * g_ticksPerQuarter}, {"length", 4 * g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes", {{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	run(client, "marker.add", {{"track", markers}, {"position", 8 * g_ticksPerQuarter}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add",
		{{"lane", lane}, {"points", {{{"position", 8 * g_ticksPerQuarter}, {"value", -6.0}}}}});

	// Where the audio event, the note, the marker and the automation point are, in samples.
	const auto times = [&]
	{
		const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		const RenderChannel& channel = renderChannel(snapshot, audio);
		return std::vector<int64_t>{channel.audioEvents.at(0).start, renderChannel(snapshot, midi).notes.at(0).start,
			run(client, "marker.list", {{"track", markers}}).at("markers").at(0).at("positionSamples").get<int64_t>(),
			channel.automation.at(0).points.at(0).position};
	};
	const std::vector<int64_t> at8(4, 8 * g_quarterSamples);
	CHECK(times() == at8);

	// Linear: switching keeps them where they are, and a slower tempo before them leaves them there.
	for (const ObjectId track : {audio, midi, markers})
		run(client, "track.set", {{"track", track}, {"timeBase", "linear"}});
	CHECK(times() == at8);
	run(client, "tempo.add", point(4, 60.0));
	CHECK(times() == at8);

	// Musical again: they stay at their time, two quarters of 60 BPM after the fourth quarter, and follow the tempo.
	for (const ObjectId track : {audio, midi, markers})
		run(client, "track.set", {{"track", track}, {"timeBase", "musical"}});
	CHECK(times() == at8);
	run(client, "tempo.remove", {{"positions", {4 * g_ticksPerQuarter}}});
	CHECK(times() == std::vector<int64_t>(4, 6 * g_quarterSamples));
}

TEST_CASE("new tracks get the time base of the user setting editing.defaultTimeBase", "[unit]")
{
	Engine engine;
	Client client(engine);
	const auto timeBase = [&client](const std::string& type, Json parameters = Json::object())
	{
		parameters["type"] = type;
		return run(client, "track.add", parameters).at("track").at("timeBase").get<std::string>();
	};
	CHECK(run(client, "settings.get", {{"key", "editing.defaultTimeBase"}}).at("value") == "musical");
	CHECK(timeBase("audio") == "musical");

	run(client, "settings.set", {{"key", "editing.defaultTimeBase"}, {"value", "linear"}});
	for (const char* type : {"audio", "instrument", "midi", "group", "effect", "marker"})
	{
		INFO(type);
		CHECK(timeBase(type) == "linear");
	}
	// Tracks without events of their own have none.
	for (const char* type : {"folder", "ruler", "tempo", "signature"})
	{
		INFO(type);
		CHECK(timeBase(type) == "musical");
	}
	CHECK(timeBase("audio", {{"timeBase", "musical"}}) == "musical"); // a time base given wins

	// Following the primary time format: bars and beats make musical tracks, any other format linear ones.
	run(client, "settings.set", {{"key", "editing.defaultTimeBase"}, {"value", "followTimeFormat"}});
	CHECK(timeBase("midi") == "musical");
	run(client, "project.set", {{"timeFormat", "seconds"}});
	CHECK(timeBase("midi") == "linear");
	run(client, "project.set", {{"timeFormat", "barsBeats"}});
	CHECK(timeBase("midi") == "musical");
	CHECK_THAT(reject(client, "settings.set", {{"key", "editing.defaultTimeBase"}, {"value", "seconds"}}),
		ContainsSubstring("defaultTimeBase"));
}

TEST_CASE("tempo events ramp or jump, and the first one stays", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	// Ramps from 120 to 60 BPM over four quarters: they take 4 * ln(2) seconds, more than four quarters at 120.
	run(client, "tempo.add", point(4, 60.0, true));
	const gnubase::time::TempoMap& tempo = engine.renderSnapshots().acquire().tempo;
	CHECK(tempo.bpmAt(2 * g_ticksPerQuarter) == 90.0);
	CHECK(std::abs(tempo.toSeconds(4 * g_ticksPerQuarter) - (4.0 * std::log(2.0))) < 1e-9);

	// A tempo event at a position that has one changes it.
	run(client, "tempo.add", point(4, 80.0, false));
	CHECK(run(client, "project.get").at("tempo") == Json::array({point(0, 120.0), point(4, 80.0)}));

	CHECK_THAT(reject(client, "tempo.remove", {{"positions", {0}}}), ContainsSubstring("first tempo event"));
	CHECK_THAT(reject(client, "tempo.set", {{"events", {{{"position", 0}, {"to", g_ticksPerQuarter}}}}}),
		ContainsSubstring("project start"));
	CHECK_THAT(reject(client, "tempo.set", {{"events", {{{"position", 4 * g_ticksPerQuarter}, {"to", 0}}}}}),
		ContainsSubstring("project start"));
	CHECK_THAT(reject(client, "tempo.set", {{"events", {{{"position", g_ticksPerQuarter}, {"bpm", 90.0}}}}}),
		ContainsSubstring("no tempo event"));
	CHECK_THAT(reject(client, "tempo.add", point(8, 1000.0)), ContainsSubstring("1 to 999"));
	CHECK_THAT(
		reject(client, "tempo.set", {{"events", {{{"position", 0}, {"bpm", 0.5}}}}}), ContainsSubstring("1 to 999"));

	// Changes happen at once: two events can swap places.
	run(client, "tempo.add", point(8, 100.0));
	run(client, "tempo.set",
		{{"events",
			{{{"position", 4 * g_ticksPerQuarter}, {"to", 8 * g_ticksPerQuarter}},
				{{"position", 8 * g_ticksPerQuarter}, {"to", 4 * g_ticksPerQuarter}, {"ramp", true}}}}});
	CHECK(run(client, "project.get").at("tempo") ==
		Json::array({point(0, 120.0), point(4, 100.0, true), point(8, 80.0)}));
	CHECK_THAT(reject(client, "tempo.set",
				   {{"events", {{{"position", 4 * g_ticksPerQuarter}, {"to", 8 * g_ticksPerQuarter}}}}}),
		ContainsSubstring("two tempo events"));
	// A tempo event at the cursor (Insert Tempo Event at Cursor), in ticks at the tempo before it.
	run(client, "transport.locate", {{"position", 2 * g_quarterSamples}});
	const int64_t cursor = engine.renderSnapshots().acquire().tempo.toTicks(2 * g_quarterSamples, 48000.0);
	const Json atCursor = run(client, "tempo.add", {{"position", "cursor"}, {"bpm", 70.0}, {"ramp", false}});
	CHECK(atCursor.at("position") == cursor);
	CHECK(atCursor.at("tempo").at("tempoTrack").size() == 4);
}

TEST_CASE("new tempo events ramp, jump or follow the event before them by the user setting", "[unit]")
{
	Engine engine; // with user settings in memory
	Client client(engine);
	const auto add = [&client](int64_t quarters, double bpm)
	{ run(client, "tempo.add", {{"position", quarters * g_ticksPerQuarter}, {"bpm", bpm}}); };
	run(client, "tempo.add", point(4, 90.0, true));
	add(8, 100.0); // auto: like the ramp before it
	run(client, "settings.set", {{"key", "editing.tempoType"}, {"value", "step"}});
	add(12, 110.0);
	run(client, "settings.set", {{"key", "editing.tempoType"}, {"value", "ramp"}});
	add(16, 120.0);
	CHECK(run(client, "project.get").at("tempoTrack") ==
		Json::array(
			{point(0, 120.0), point(4, 90.0, true), point(8, 100.0, true), point(12, 110.0), point(16, 120.0, true)}));
}

TEST_CASE("a tapped tempo sets the project tempo or adds a tempo event, by the tap tempo mode", "[unit]")
{
	Engine engine; // with user settings in memory
	Client client(engine);

	// Automatic in fixed tempo mode: the project tempo, rounded to 0.001 BPM.
	Json tapped = run(client, "tempo.tap", {{"bpm", 93.12345}});
	CHECK(tapped == Json{{"bpm", 93.123}, {"applied", "project"}, {"position", nullptr}});
	CHECK(run(client, "project.get").at("fixedTempo") == 93.123);

	run(client, "project.set", {{"tempoTrackActive", true}, {"tempo", 120.0}});
	run(client, "transport.locate", {{"position", 4 * g_quarterSamples}});
	// Automatic in tempo track mode: at the selection's start, else at the cursor while stopped.
	tapped = run(client, "tempo.tap", {{"bpm", 100.0}, {"selection", 2 * g_ticksPerQuarter}});
	CHECK(tapped.at("position") == 2 * g_ticksPerQuarter);
	tapped = run(client, "tempo.tap", {{"bpm", 110.0}});
	CHECK(tapped.at("applied") == "event");
	// Four quarters at 120 BPM are two seconds: two quarters at 120, then one second at 100 BPM, 5/3 quarters.
	CHECK(tapped.at("position") == (2 * g_ticksPerQuarter) + (5 * g_ticksPerQuarter / 3));
	CHECK(run(client, "tempo.tap", {{"bpm", 110.0}, {"mode", "cursor"}}).at("position") == tapped.at("position"));

	// Last playback start position: where playback started.
	run(client, "transport.locate", {{"position", g_quarterSamples}});
	run(client, "transport.play");
	run(client, "transport.locate", {{"position", 6 * g_quarterSamples}});
	tapped = run(client, "tempo.tap", {{"bpm", 130.0}});
	CHECK(tapped.at("position") == g_ticksPerQuarter);
	run(client, "transport.stop");

	// Selection start without a selection, and display only, change nothing.
	const Json before = run(client, "project.get").at("tempo");
	CHECK(run(client, "tempo.tap", {{"bpm", 140.0}, {"mode", "selection"}}).at("applied") == "none");
	CHECK(run(client, "tempo.tap", {{"bpm", 140.0}, {"mode", "display"}}).at("applied") == "none");
	CHECK(run(client, "project.get").at("tempo") == before);

	// Set project tempo in tempo track mode: the first tempo event. The mode defaults to the user setting.
	run(client, "settings.set", {{"key", "transport.tapTempoMode"}, {"value", "project"}});
	CHECK(run(client, "tempo.tap", {{"bpm", 90.0}}).at("applied") == "project");
	CHECK(run(client, "project.get").at("tempo")[0].at("bpm") == 90.0);
}

TEST_CASE("the tempo modes and the tempo track's events are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "tempo.dawproject").u8string();
	Engine engine;
	Client client(engine);
	addTrack(client, {{"type", "tempo"}});
	run(client, "tempo.add", point(4, 90.0, true));
	run(client, "project.set", {{"tempo", 110.0}}); // the fixed tempo
	run(client, "project.save", {{"path", file}});
	const Json before = run(client, "project.get");
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	const Json after = run(client, "project.get");
	for (const char* key : {"tempo", "tempoTrackActive", "tempoTrack", "fixedTempo"})
		CHECK(after.at(key) == before.at(key));
	CHECK(after.at("tracks").at(0).at("type") == "tempo");
}
