// SPDX-License-Identifier: GPL-3.0-or-later
// Marker tracks, position and cycle markers, and locating with them (#91).
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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

	ObjectId idOf(const Json& object)
	{
		return object.at("id").get<ObjectId>();
	}

	ObjectId addMarkerTrack(Client& client, Json parameters = Json::object())
	{
		parameters["type"] = "marker";
		return idOf(run(client, "track.add", std::move(parameters)).at("track"));
	}

	// Adds a marker and returns it.
	Json addMarker(Client& client, Json parameters)
	{
		return run(client, "marker.add", std::move(parameters)).at("marker");
	}

	Json addPosition(Client& client, int64_t quarters, const std::string& name = "")
	{
		return addMarker(client, {{"position", quarters * g_ticksPerQuarter}, {"name", name}});
	}

	Json addCycle(Client& client, int64_t from, int64_t to, const std::string& name = "")
	{
		return addMarker(client,
			{{"type", "cycle"}, {"position", from * g_ticksPerQuarter}, {"end", to * g_ticksPerQuarter},
				{"name", name}});
	}

	int64_t cursor(Client& client)
	{
		return run(client, "transport.get").at("position").get<int64_t>();
	}

	std::vector<std::string> eventNames(Client& client)
	{
		std::vector<std::string> names;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				names.push_back(event->name);
		return names;
	}

	std::vector<std::string> markerNames(Client& client, Json parameters = Json::object())
	{
		std::vector<std::string> names;
		const Json list = run(client, "marker.list", std::move(parameters));
		for (const Json& marker : list.at("markers"))
			names.push_back(marker.at("name").get<std::string>());
		return names;
	}
}

TEST_CASE("marker tracks are named, limited to ten and the newest one is active", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json first = run(client, "track.add", {{"type", "marker"}});
	CHECK(first.at("track").at("name") == "Markers");
	CHECK(first.at("track").at("channel") == 0);
	CHECK(first.at("channel").is_null()); // marker tracks have no mixer channel
	const ObjectId second = addMarkerTrack(client);
	CHECK(run(client, "project.get").at("tracks").at(1).at("name") == "Marker 01");
	CHECK(run(client, "project.get").at("activeMarkerTrack") == second);
	CHECK(engine.renderSnapshots().acquire().channels.empty());

	for (int i = 2; i < 10; ++i)
		addMarkerTrack(client);
	CHECK_THAT(reject(client, "track.add", {{"type", "marker"}}), ContainsSubstring("10 marker tracks"));

	const ObjectId folder = idOf(run(client, "track.add", {{"type", "folder"}}).at("track"));
	CHECK_THAT(reject(client, "track.move", {{"track", second}, {"parent", folder}, {"index", 0}}),
		ContainsSubstring("folder"));
	run(client, "project.new");
	const ObjectId folder2 = idOf(run(client, "track.add", {{"type", "folder"}}).at("track"));
	CHECK_THAT(reject(client, "track.add", {{"type", "marker"}, {"parent", folder2}}), ContainsSubstring("folder"));
	CHECK_THAT(reject(client, "track.add", {{"type", "marker"}, {"channels", 2}}), ContainsSubstring("channels"));
}

TEST_CASE("activating marker tracks, and removing the active one activates the topmost", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const ObjectId first = addMarkerTrack(client);
	const ObjectId second = addMarkerTrack(client);
	const ObjectId third = addMarkerTrack(client);
	CHECK(run(client, "project.get").at("activeMarkerTrack") == third);

	eventNames(listener);
	run(client, "marker.activateTrack", {{"track", second}});
	CHECK(run(client, "project.get").at("activeMarkerTrack") == second);
	CHECK(eventNames(listener) == std::vector<std::string>{"marker.trackActivated", "history.changed"});
	const ObjectId audio = idOf(run(client, "track.add", {{"type", "audio"}}).at("track"));
	CHECK_THAT(reject(client, "marker.activateTrack", {{"track", audio}}), ContainsSubstring("not a marker track"));

	run(client, "track.move", {{"track", third}, {"parent", 0}, {"index", 0}}); // third is now the topmost
	addPosition(client, 1);
	run(client, "track.remove", {{"track", second}});
	const Json project = run(client, "project.get");
	CHECK(project.at("activeMarkerTrack") == third);
	CHECK(project.at("markers").empty()); // the removed track's markers go with it
	run(client, "track.remove", {{"track", third}});
	CHECK(run(client, "project.get").at("activeMarkerTrack") == first);
	run(client, "track.remove", {{"track", first}});
	CHECK(run(client, "project.get").at("activeMarkerTrack") == 0);
}

TEST_CASE("position and cycle markers get their own id numbers per track", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const ObjectId track = addMarkerTrack(client);
	eventNames(listener);

	const Json intro = addPosition(client, 0, "Intro");
	CHECK(intro.at("track") == track);
	CHECK(intro.at("type") == "position");
	CHECK(intro.at("number") == 1);
	CHECK(intro.at("position") == 0);
	CHECK(intro.at("end").is_null());
	CHECK(intro.at("name") == "Intro");
	CHECK(intro.at("color").is_null());
	CHECK(eventNames(listener) == std::vector<std::string>{"marker.added", "history.changed"});

	const Json verse = addCycle(client, 4, 12, "Verse");
	CHECK(verse.at("type") == "cycle");
	CHECK(verse.at("number") == 1); // cycle markers count separately
	CHECK(verse.at("end") == 12 * g_ticksPerQuarter);
	CHECK(addPosition(client, 8).at("number") == 2);

	// The first free number goes to the next marker; a number can be given if it is free.
	run(client, "marker.remove", {{"markers", {idOf(intro)}}});
	CHECK(addPosition(client, 16).at("number") == 1);
	CHECK(addMarker(client, {{"position", 20 * g_ticksPerQuarter}, {"number", 7}}).at("number") == 7);
	CHECK_THAT(reject(client, "marker.add", {{"position", 0}, {"number", 7}}), ContainsSubstring("7"));
	CHECK_THAT(reject(client, "marker.add", {{"position", 0}, {"number", 0}}), ContainsSubstring("number"));

	// Numbering again follows the order on the track.
	run(client, "marker.renumber", {{"type", "position"}});
	std::vector<int64_t> numbers;
	const Json list = run(client, "marker.list");
	for (const Json& marker : list.at("markers"))
		if (marker.at("type") == "position")
			numbers.push_back(marker.at("number").get<int64_t>());
	CHECK(numbers == std::vector<int64_t>{1, 2, 3});
}

TEST_CASE("adding a marker without a marker track adds one", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	eventNames(listener);
	const Json marker = addPosition(client, 2, "Drop");
	const Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("tracks").at(0).at("type") == "marker");
	CHECK(project.at("activeMarkerTrack") == marker.at("track"));
	const std::vector<std::string> names = eventNames(listener);
	CHECK(names == std::vector<std::string>{"track.added", "marker.trackActivated", "marker.added", "history.changed"});

	run(client, "history.undo"); // one step
	CHECK(run(client, "project.get").at("tracks").empty());
	CHECK(run(client, "project.get").at("activeMarkerTrack") == 0);
}

TEST_CASE("markers default to the cursor and the locators", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "transport.locate", {{"position", 2 * g_quarterSamples}});
	run(client, "transport.set", {{"left", 4 * g_quarterSamples}, {"right", 8 * g_quarterSamples}});

	CHECK(addMarker(client, Json::object()).at("position") == 2 * g_ticksPerQuarter);
	const Json cycle = addMarker(client, {{"type", "cycle"}});
	CHECK(cycle.at("position") == 4 * g_ticksPerQuarter);
	CHECK(cycle.at("end") == 8 * g_ticksPerQuarter);
	CHECK_THAT(reject(client, "marker.add", {{"type", "cycle"}, {"position", 0}}), ContainsSubstring("end"));
	CHECK_THAT(
		reject(client, "marker.add", {{"type", "cycle"}, {"position", 100}, {"end", 100}}), ContainsSubstring("end"));
	CHECK_THAT(reject(client, "marker.add", {{"position", 0}, {"end", 100}}), ContainsSubstring("end"));
	CHECK_THAT(reject(client, "marker.add", {{"position", -1}}), ContainsSubstring("negative"));
	run(client, "transport.set", {{"left", 8 * g_quarterSamples}, {"right", 4 * g_quarterSamples}});
	CHECK_THAT(reject(client, "marker.add", {{"type", "cycle"}}), ContainsSubstring("locators"));

	// On a linear marker track positions are in samples.
	const ObjectId linear = addMarkerTrack(client, {{"timeBase", "linear"}});
	CHECK(addMarker(client, {{"track", linear}}).at("position") == 2 * g_quarterSamples);
}

TEST_CASE("markers move, get renamed and move to other marker tracks", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const ObjectId first = addMarkerTrack(client);
	const Json marker = addPosition(client, 1, "Intro");
	const Json cycle = addCycle(client, 4, 8, "Verse");
	eventNames(listener);

	Json changed =
		run(client, "marker.set", {{"marker", idOf(marker)}, {"name", "Start"}, {"color", "#ff0000"}}).at("marker");
	CHECK(changed.at("name") == "Start");
	CHECK(changed.at("color") == "#ff0000");
	CHECK(eventNames(listener) == std::vector<std::string>{"marker.changed", "history.changed"});

	// Moving a cycle marker's start moves all of it unless the end is given.
	changed = run(client, "marker.set", {{"marker", idOf(cycle)}, {"position", 6 * g_ticksPerQuarter}}).at("marker");
	CHECK(changed.at("position") == 6 * g_ticksPerQuarter);
	CHECK(changed.at("end") == 10 * g_ticksPerQuarter);
	changed = run(client, "marker.set", {{"marker", idOf(cycle)}, {"end", 7 * g_ticksPerQuarter}}).at("marker");
	CHECK(changed.at("end") == 7 * g_ticksPerQuarter);
	CHECK_THAT(reject(client, "marker.set", {{"marker", idOf(cycle)}, {"end", 6 * g_ticksPerQuarter}}),
		ContainsSubstring("end"));
	CHECK_THAT(reject(client, "marker.set", {{"marker", idOf(marker)}, {"end", 0}}), ContainsSubstring("end"));

	// To the cursor.
	run(client, "transport.locate", {{"position", 3 * g_quarterSamples}});
	changed = run(client, "marker.set", {{"marker", idOf(cycle)}, {"position", "cursor"}}).at("marker");
	CHECK(changed.at("position") == 3 * g_ticksPerQuarter);
	CHECK(changed.at("end") == 4 * g_ticksPerQuarter);

	// Numbers stay unique on a track.
	const Json other = addPosition(client, 2);
	CHECK_THAT(reject(client, "marker.set", {{"marker", idOf(other)}, {"number", 1}}), ContainsSubstring("1"));
	CHECK(run(client, "marker.set", {{"marker", idOf(other)}, {"number", 5}}).at("marker").at("number") == 5);

	// On another track a marker takes the first free number there, in that track's time base.
	const ObjectId second = addMarkerTrack(client, {{"timeBase", "linear"}});
	addMarker(client, {{"track", second}, {"position", 0}});
	changed = run(client, "marker.set", {{"marker", idOf(marker)}, {"track", second}}).at("marker");
	CHECK(changed.at("track") == second);
	CHECK(changed.at("number") == 2);
	CHECK(changed.at("position") == g_quarterSamples);
	const ObjectId audio = idOf(run(client, "track.add", {{"type", "audio"}}).at("track"));
	CHECK_THAT(reject(client, "marker.set", {{"marker", idOf(marker)}, {"track", audio}}),
		ContainsSubstring("not a marker track"));
	CHECK(markerNames(client, {{"track", first}}) == std::vector<std::string>{"", "Verse"});

	run(client, "marker.remove", {{"markers", {idOf(marker), idOf(cycle)}}});
	CHECK(run(client, "marker.list", {{"track", second}}).at("markers").size() == 1);
	CHECK_THAT(reject(client, "marker.remove", {{"markers", {idOf(other), 999}}}), ContainsSubstring("999"));
	CHECK(run(client, "marker.list", {{"track", first}}).at("markers").size() == 1); // nothing was removed
}

TEST_CASE("a marker track's time base and the sample rate keep markers at their time", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = addMarkerTrack(client);
	const Json cycle = addCycle(client, 2, 4);
	run(client, "track.set", {{"track", track}, {"timeBase", "linear"}});
	Json marker = run(client, "marker.list").at("markers").at(0);
	CHECK(marker.at("position") == 2 * g_quarterSamples);
	CHECK(marker.at("end") == 4 * g_quarterSamples);
	run(client, "project.set", {{"sampleRate", 96000.0}});
	marker = run(client, "marker.list").at("markers").at(0);
	CHECK(marker.at("position") == 4 * g_quarterSamples);
	CHECK(marker.at("positionSamples") == 4 * g_quarterSamples);
	CHECK(marker.at("endSamples") == 8 * g_quarterSamples);
	CHECK(idOf(marker) == idOf(cycle));
}

TEST_CASE("marker.list lists a track's markers in timeline order with their samples", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK(run(client, "marker.list") == Json{{"track", 0}, {"markers", Json::array()}});
	const ObjectId first = addMarkerTrack(client);
	addPosition(client, 8, "C");
	addCycle(client, 0, 4, "A");
	addPosition(client, 2, "B");
	const ObjectId second = addMarkerTrack(client);
	addPosition(client, 1, "Other");

	CHECK(markerNames(client) == std::vector<std::string>{"Other"}); // the active track
	const Json list = run(client, "marker.list", {{"track", first}});
	CHECK(list.at("track") == first);
	CHECK(markerNames(client, {{"track", first}}) == std::vector<std::string>{"A", "B", "C"});
	CHECK(list.at("markers").at(0).at("positionSamples") == 0);
	CHECK(list.at("markers").at(0).at("endSamples") == 4 * g_quarterSamples);
	CHECK(list.at("markers").at(1).at("endSamples").is_null());
	CHECK_THAT(reject(client, "marker.list", {{"track", second + 100}}), ContainsSubstring("no track"));
}

TEST_CASE("locating to the next and previous marker and to marker N", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	addMarkerTrack(client);
	const Json one = addPosition(client, 2, "One");
	const Json verse = addCycle(client, 4, 8, "Verse");
	const Json two = addPosition(client, 10, "Two");
	eventNames(listener);

	// Position markers and the start and end of cycle markers, from the cursor on.
	Json result = run(client, "marker.locateNext");
	CHECK(result.at("position") == 2 * g_quarterSamples);
	CHECK(result.at("marker").at("id") == idOf(one));
	CHECK(cursor(client) == 2 * g_quarterSamples);
	CHECK(eventNames(listener) == std::vector<std::string>{"transport.changed"});
	CHECK(run(client, "marker.locateNext").at("marker").at("id") == idOf(verse));
	CHECK(run(client, "marker.locateNext").at("position") == 8 * g_quarterSamples);
	CHECK(run(client, "marker.locateNext").at("marker").at("id") == idOf(two));
	result = run(client, "marker.locateNext"); // nothing after the last one: the cursor stays
	CHECK(result.at("marker").is_null());
	CHECK(result.at("position") == 10 * g_quarterSamples);
	CHECK(eventNames(listener).size() == 3); // only the commands that located announced transport.changed

	CHECK(run(client, "marker.locatePrevious").at("position") == 8 * g_quarterSamples);
	CHECK(run(client, "marker.locatePrevious").at("position") == 4 * g_quarterSamples);
	run(client, "transport.locate", {{"position", 2 * g_quarterSamples + 1}});
	CHECK(run(client, "marker.locatePrevious").at("position") == 2 * g_quarterSamples);
	CHECK(run(client, "marker.locatePrevious").at("marker").is_null());

	// By id number: position markers, or cycle markers by their start.
	CHECK(run(client, "marker.locate", {{"number", 2}}).at("position") == 10 * g_quarterSamples);
	CHECK(run(client, "marker.locate", {{"number", 1}, {"type", "cycle"}}).at("position") == 4 * g_quarterSamples);
	CHECK(run(client, "marker.locate", {{"marker", idOf(one)}}).at("position") == 2 * g_quarterSamples);
	CHECK(cursor(client) == 2 * g_quarterSamples);
	CHECK_THAT(reject(client, "marker.locate", {{"number", 9}}), ContainsSubstring("9"));
	CHECK_THAT(reject(client, "marker.locate"), ContainsSubstring("marker or number"));
	CHECK(cursor(client) == 2 * g_quarterSamples);

	// Other marker tracks don't count; locating changes no undo step.
	const ObjectId other = addMarkerTrack(client);
	CHECK(run(client, "marker.locateNext").at("marker").is_null());
	const auto steps = run(client, "history.get").at("steps").size();
	CHECK(run(client, "marker.locateNext", {{"track", other}}).at("marker").is_null());
	CHECK(run(client, "marker.locateNext", {{"track", one.at("track")}}).at("position") == 4 * g_quarterSamples);
	CHECK(run(client, "history.get").at("steps").size() == steps);
}

TEST_CASE("markers on musical and linear marker tracks locate to their samples", "[unit]")
{
	Engine engine;
	Client client(engine);
	addMarkerTrack(client);
	addPosition(client, 4);
	const ObjectId linear = addMarkerTrack(client, {{"timeBase", "linear"}});
	addMarker(client, {{"track", linear}, {"position", 12345}});
	CHECK(run(client, "marker.locate", {{"number", 1}}).at("position") == 12345);
	run(client, "marker.activateTrack", {{"track", run(client, "project.get").at("tracks").at(0).at("id")}});
	CHECK(run(client, "marker.locate", {{"number", 1}}).at("position") == 4 * g_quarterSamples);
}

TEST_CASE("cycle markers set the locators, and markers bound range selections", "[unit]")
{
	Engine engine;
	Client client(engine);
	addMarkerTrack(client);
	const Json verse = addCycle(client, 4, 8, "Verse");
	const Json chorus = addCycle(client, 8, 16, "Chorus");
	const Json drop = addPosition(client, 20, "Drop");

	Json result = run(client, "marker.setLocators", {{"number", 2}});
	CHECK(result.at("left") == 8 * g_quarterSamples);
	CHECK(result.at("right") == 16 * g_quarterSamples);
	CHECK(result.at("marker").at("id") == idOf(chorus));
	const Json transport = run(client, "transport.get");
	CHECK(transport.at("left") == 8 * g_quarterSamples);
	CHECK(transport.at("right") == 16 * g_quarterSamples);
	result = run(client, "marker.setLocators", {{"marker", idOf(verse)}});
	CHECK(result.at("left") == 4 * g_quarterSamples);
	CHECK_THAT(reject(client, "marker.setLocators", {{"marker", idOf(drop)}}), ContainsSubstring("cycle marker"));
	CHECK_THAT(reject(client, "marker.setLocators", {{"number", 3}}), ContainsSubstring("3"));

	// The range between the markers around a position: what a range selection between markers spans.
	result = run(client, "marker.range", {{"position", 17 * g_quarterSamples}});
	CHECK(result.at("start") == 16 * g_quarterSamples);
	CHECK(result.at("end") == 20 * g_quarterSamples);
	result = run(client, "marker.range", {{"position", 4 * g_quarterSamples}});
	CHECK(result.at("start") == 4 * g_quarterSamples);
	CHECK(result.at("end") == 8 * g_quarterSamples);
	CHECK_THAT(reject(client, "marker.range", {{"position", g_quarterSamples}}), ContainsSubstring("before"));
	CHECK_THAT(reject(client, "marker.range", {{"position", 21 * g_quarterSamples}}), ContainsSubstring("after"));
}

TEST_CASE("every marker edit is one undo step that undo announces", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const ObjectId first = addMarkerTrack(client);
	const ObjectId second = addMarkerTrack(client);
	const Json empty = run(client, "project.get");
	const Json marker = addPosition(client, 1, "A");
	addCycle(client, 2, 4, "B");
	run(client, "marker.set",
		{{"marker", idOf(marker)}, {"name", "C"}, {"position", 3 * g_ticksPerQuarter}, {"number", 3}});
	run(client, "marker.renumber", {{"type", "position"}});
	run(client, "marker.activateTrack", {{"track", first}});
	run(client, "marker.set", {{"marker", idOf(marker)}, {"track", first}});
	const Json edited = run(client, "project.get");

	const Json history = run(client, "history.get");
	std::vector<std::string> actions;
	for (const Json& step : history.at("steps"))
		actions.push_back(step.at("action").get<std::string>());
	CHECK(actions ==
		std::vector<std::string>{"Add track", "Add track", "Add marker", "Add marker", "Change marker",
			"Renumber markers", "Activate marker track", "Change marker"});

	eventNames(listener);
	run(client, "history.undo", {{"steps", 6}});
	CHECK(run(client, "project.get") == empty);
	const std::vector<std::string> names = eventNames(listener);
	CHECK(std::count(names.begin(), names.end(), "marker.removed") == 2);
	CHECK(std::count(names.begin(), names.end(), "marker.trackActivated") == 1);
	CHECK(run(client, "project.get").at("activeMarkerTrack") == second);
	run(client, "history.redo", {{"steps", 6}});
	CHECK(run(client, "project.get") == edited);

	// Removing a track with markers is one step too, and undo brings back the markers.
	run(client, "track.remove", {{"track", first}});
	run(client, "history.undo");
	CHECK(run(client, "project.get") == edited);

	// Markers can be part of a transaction.
	run(client, "history.transaction",
		{{"description", "Song parts"},
			{"commands",
				{{{"command", "marker.add"}, {"parameters", {{"type", "cycle"}, {"position", 0}, {"end", 100}}}},
					{{"command", "marker.add"}, {"parameters", {{"position", 50}}}}}}});
	CHECK(run(client, "project.get").at("markers").size() == 4);
	run(client, "history.undo");
	CHECK(run(client, "project.get") == edited);
}
