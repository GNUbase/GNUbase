// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"
#include "engine/document/projectrules.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// 120 BPM at 48 kHz: project ticks per quarter note, and per tick of 480 to a quarter note.
	constexpr int64_t g_beatTicks = 1720320;
	constexpr int64_t g_tick = g_beatTicks / 480;

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

	Json filter(const char* target, const char* condition, Json p1 = "", double p2 = 0.0)
	{
		return {{"target", target}, {"condition", condition}, {"parameter1", std::move(p1)}, {"parameter2", p2}};
	}

	Json action(const char* target, const char* operation, Json p1 = "", Json p2 = 0)
	{
		return {
			{"target", target}, {"operation", operation}, {"parameter1", std::move(p1)}, {"parameter2", std::move(p2)}};
	}

	Json rules(std::vector<Json> filters, const char* function, std::vector<Json> actions = {})
	{
		return {{"filters", Json(std::move(filters))}, {"function", function}, {"actions", Json(std::move(actions))}};
	}

	Json readJson(const std::filesystem::path& path)
	{
		std::ifstream in(path);
		return Json::parse(in);
	}

	// A folder with an audio track "Vocals" in it, a MIDI track "Keys" with a part of notes at a quarter note and an
	// empty muted one at two, and a marker track with a cycle marker from a quarter note to two.
	struct Fixture
	{
		explicit Fixture(const std::filesystem::path& settings = {}) : engine(gnubase::audio::makeBackends(), settings)
		{
			folder = add({{"type", "folder"}, {"name", "Band"}});
			vocals = add({{"type", "audio"}, {"name", "Vocals"}, {"parent", folder}});
			keys = add({{"type", "midi"}, {"name", "Keys"}});
			full = part(1, "Chords");
			run(client, "note.add",
				{{"part", full},
					{"notes",
						Json::array({{{"position", 0}, {"length", g_beatTicks}, {"pitch", 60}, {"velocity", 0.8}}})}});
			empty = part(2, "Empty");
			run(client, "event.set", {{"events", {empty}}, {"mute", true}});
			markers = add({{"type", "marker"}, {"timeBase", "musical"}});
			marker = run(client, "marker.add",
				{{"type", "cycle"}, {"track", markers}, {"position", g_beatTicks}, {"end", 2 * g_beatTicks},
					{"name", "Verse"}})
						 .at("marker")
						 .at("id")
						 .get<ObjectId>();
		}

		ObjectId add(const Json& parameters)
		{
			return run(client, "track.add", parameters).at("track").at("id").get<ObjectId>();
		}

		ObjectId part(int beat, const char* name)
		{
			return run(client, "midiPart.add",
				{{"track", keys}, {"position", beat * g_beatTicks}, {"length", g_beatTicks}, {"name", name}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		}

		Json find(const Json& parameters) { return run(client, "project.findByRules", parameters); }

		Json object(const char* list, ObjectId id)
		{
			const Json project = run(client, "project.get");
			for (const Json& item : project.at(list))
				if (item.at("id") == id)
					return item;
			return nullptr;
		}

		Engine engine;
		Client client{engine};
		ObjectId folder = 0;
		ObjectId vocals = 0;
		ObjectId keys = 0;
		ObjectId full = 0;
		ObjectId empty = 0;
		ObjectId markers = 0;
		ObjectId marker = 0;
	};
}

TEST_CASE("project rules as JSON: factory presets round-trip, and wrong rules are refused", "[unit]")
{
	for (const ProjectRulePreset& preset : factoryProjectRulePresets())
	{
		INFO(preset.name);
		CHECK(projectRulesFromJson(toJson(preset.rules)) == preset.rules);
	}
	const Json commands = {{"pre", Json::array({{{"command", "edit.selectAll"}}})},
		{"post", Json::array({{{"command", "history.undo"}, {"parameters", {{"steps", 1}}}}})}};
	Json withCommands = rules({}, "select");
	withCommands.update(commands);
	const ProjectRules parsed = projectRulesFromJson(withCommands);
	REQUIRE(parsed.pre.size() == 1);
	CHECK(parsed.post.at(0).parameters == Json{{"steps", 1}});
	CHECK(toJson(parsed).at("post") == commands.at("post"));

	const auto refused = [](const Json& json)
	{
		try
		{
			projectRulesFromJson(json);
		}
		catch (const std::invalid_argument& error)
		{
			return std::string(error.what());
		}
		return std::string("accepted");
	};
	CHECK_THAT(refused(rules({filter("mediaType", "contains", "audio")}, "select")),
		ContainsSubstring("no condition contains"));
	CHECK_THAT(refused(rules({filter("mediaType", "equal", "video")}, "select")), ContainsSubstring("no media type"));
	CHECK_THAT(
		refused(rules({filter("property", "propertyIsSet", "isPurple")}, "select")), ContainsSubstring("no property"));
	CHECK_THAT(refused(rules({filter("position", "less", "x")}, "select")), ContainsSubstring("must be a number"));
	Json open = filter("containerType", "equal", "part");
	open["open"] = 1;
	CHECK_THAT(refused(rules({open}, "select")), ContainsSubstring("not closed"));
	CHECK_THAT(refused(rules({}, "transform", {action("name", "add", "x")})), ContainsSubstring("no operation add"));
	CHECK_THAT(refused(rules({}, "transform", {action("trackOperation", "mute", "maybe")})),
		ContainsSubstring("on, off or toggle"));
	Json many = rules({}, "select");
	many["pre"] = Json::array();
	for (int i = 0; i < 5; ++i)
		many["pre"].push_back({{"command", "edit.selectAll"}});
	CHECK(refused(many) != "accepted");
}

TEST_CASE("project.findByRules finds tracks, parts and events", "[integration]")
{
	Fixture f;
	using Ids = std::vector<ObjectId>;
	const auto found = [&f](const Json& parameters, const char* list)
	{ return f.find(parameters).at(list).get<Ids>(); };

	CHECK(found({{"preset", "Select/MIDI parts"}}, "events") == Ids{f.full, f.empty});
	CHECK(found({{"preset", "Select/MIDI parts"}}, "tracks").empty());
	CHECK(found({{"preset", "Select/Muted parts and events"}}, "events") == Ids{f.empty});
	CHECK(found({{"preset", "Select/Tracks named Vocals"}}, "tracks") == Ids{f.vocals});
	CHECK(found({{"preset", "Select/Parts of the selected tracks"}, {"selected", {f.keys}}}, "events") ==
		Ids{f.full, f.empty});
	CHECK(found({{"preset", "Select/Parts of the selected tracks"}}, "events").empty());
	// Tracks from top to bottom; a folder is a container of its own.
	CHECK(found({{"rules", rules({filter("containerType", "allTypes")}, "select")}}, "tracks") ==
		Ids{f.folder, f.vocals, f.keys, f.markers});
	CHECK(found({{"rules", rules({filter("containerType", "equal", "track")}, "select")}}, "tracks") ==
		Ids{f.vocals, f.keys, f.markers});
	// Positions and lengths in ticks of 480 to a quarter note; tracks have neither.
	const Json later = rules({filter("position", "biggerOrEqual", 480)}, "select");
	CHECK(found({{"rules", later}}, "events") == Ids{f.full, f.empty});
	CHECK(found({{"rules", later}}, "markers") == Ids{f.marker});
	CHECK(found({{"rules", later}}, "tracks").empty());
	CHECK(found({{"rules", rules({filter("position", "insideRange", 400, 500)}, "select")}}, "events") == Ids{f.full});
	CHECK(found({{"rules", rules({filter("length", "equal", 480)}, "select")}}, "markers") == Ids{f.marker});
	// Names, color names and properties.
	CHECK(found({{"rules", rules({filter("name", "contains", "e")}, "select")}}, "markers") == Ids{f.marker});
	CHECK(found({{"rules", rules({filter("name", "contains", "and")}, "select")}}, "tracks") == Ids{f.folder});
	CHECK(found({{"rules", rules({filter("name", "containsNot", "and")}, "select")}}, "tracks").size() == 3);
	run(f.client, "track.set", {{"track", f.keys}, {"color", "#4caf50"}});
	CHECK(found({{"rules", rules({filter("colorName", "equal", "Green")}, "select")}}, "tracks") == Ids{f.keys});
	CHECK(found({{"rules", rules({filter("colorName", "equal", "Green")}, "select")}}, "events") ==
		Ids{f.full, f.empty}); // parts follow their track's color
	CHECK(found({{"preset", "Delete/Empty parts"}}, "events") == Ids{f.empty});
	CHECK(found({{"preset", "Delete/Empty tracks"}}, "tracks") == Ids{f.vocals});
	CHECK(found({{"rules", rules({filter("property", "propertyIsSet", "isSelected")}, "select")},
					{"selected", {f.vocals, f.marker}}},
			  "tracks") == Ids{f.vocals});
	CHECK(found({{"rules", rules({filter("property", "propertyIsSet", "parentIsSelected")}, "select")},
					{"selected", {f.folder}}},
			  "tracks") == Ids{f.vocals});
	// and binds more tightly than or; brackets come first.
	Json first = filter("containerType", "equal", "track");
	first["join"] = "or";
	Json bracketed = first;
	bracketed["open"] = 1;
	const Json second = filter("containerType", "equal", "part");
	Json closed = second;
	closed["close"] = 1;
	const Json muted = filter("property", "propertyIsSet", "isMuted");
	const Json loose = f.find({{"rules", rules({first, second, muted}, "select")}});
	CHECK(loose.at("tracks").size() == 3);
	CHECK(loose.at("events") == Json{f.empty});
	const Json tight = f.find({{"rules", rules({bracketed, closed, muted}, "select")}});
	CHECK(tight.at("tracks").empty());
	CHECK(tight.at("events") == Json{f.empty});
	// Select finds without changing; applying refuses select.
	CHECK_THAT(reject(f.client, "project.applyRules", {{"preset", "Select/MIDI parts"}}),
		ContainsSubstring("project.findByRules"));
	CHECK_THAT(reject(f.client, "project.findByRules", {{"preset", "Nope"}}),
		ContainsSubstring("no project rule preset Nope"));
	CHECK_THAT(reject(f.client, "project.findByRules", Json::object()), ContainsSubstring("rules or preset"));
}

TEST_CASE("project.applyRules transforms and deletes in one undo step", "[integration]")
{
	Fixture f;
	const auto history = [&f] { return run(f.client, "history.get").at("steps").size(); };
	const size_t steps = history();

	SECTION("parts and markers move and resize, tracks and parts are renamed and colored")
	{
		const Json result = run(f.client, "project.applyRules",
			{{"rules",
				rules({filter("position", "bigger", 0)}, "transform",
					{action("position", "add", 240), action("length", "multiplyBy", 2), action("name", "append", "!"),
						action("color", "setFixed", "Red")})}});
		CHECK(result.at("events") == Json{f.full, f.empty});
		CHECK(f.object("midiParts", f.full).at("position") == g_beatTicks + (240 * g_tick));
		CHECK(f.object("midiParts", f.full).at("length") == 2 * g_beatTicks);
		CHECK(f.object("midiParts", f.full).at("name") == "Chords!");
		CHECK(f.object("midiParts", f.full).at("color") == "#d0453e");
		const Json marker = run(f.client, "marker.list", {{"track", f.markers}}).at("markers").at(0);
		CHECK(marker.at("position") == g_beatTicks + (240 * g_tick));
		CHECK(marker.at("end") == (3 * g_beatTicks) + (240 * g_tick));
		CHECK(marker.at("name") == "Verse!");
		CHECK(history() == steps + 1);
		run(f.client, "history.undo");
		CHECK(f.object("midiParts", f.full).at("name") == "Chords");
		CHECK(f.object("midiParts", f.full).at("position") == g_beatTicks);
	}
	SECTION("track names, colors and operations")
	{
		const Json selected = {{"selected", {f.vocals, f.keys}}};
		Json parameters = {{"preset", "Transform/Number the selected tracks"}};
		parameters.update(selected);
		run(f.client, "project.applyRules", parameters);
		CHECK(f.object("tracks", f.vocals).at("name") == "Track 1");
		CHECK(f.object("tracks", f.keys).at("name") == "Track 2");
		parameters["preset"] = "Transform/Mute the selected tracks";
		run(f.client, "project.applyRules", parameters);
		const ObjectId vocalChannel = f.object("tracks", f.vocals).at("channel").get<ObjectId>();
		CHECK(f.object("channels", vocalChannel).at("mute") == true);
		run(f.client, "project.applyRules", {{"preset", "Transform/Unmute all tracks"}});
		CHECK(f.object("channels", vocalChannel).at("mute") == false);
		run(f.client, "track.set", {{"track", f.vocals}, {"color", "#d0453e"}});
		run(f.client, "project.applyRules",
			{{"rules",
				rules({filter("name", "equal", "Track 1")}, "transform",
					{action("trackOperation", "solo", "toggle"), action("trackOperation", "timeDomain", "linear"),
						action("name", "replaceSearchString", "Track", "Lead"), action("color", "decrement")})}});
		CHECK(f.object("channels", vocalChannel).at("solo") == true);
		CHECK(f.object("tracks", f.vocals).at("timeBase") == "linear");
		CHECK(f.object("tracks", f.vocals).at("name") == "Lead 1");
		CHECK(f.object("tracks", f.vocals).at("color") == "#8c8c8c"); // before the first color, the last
		parameters["preset"] = "Transform/Next color for the selected tracks";
		run(f.client, "project.applyRules", parameters);
		CHECK(f.object("tracks", f.vocals).at("color") == "#d0453e");
	}
	SECTION("delete removes what is found; a folder takes its tracks")
	{
		run(f.client, "project.applyRules", {{"preset", "Delete/Muted parts and events"}});
		CHECK(f.object("midiParts", f.empty).is_null());
		CHECK(!f.object("midiParts", f.full).is_null());
		run(f.client, "project.applyRules",
			{{"rules", rules({filter("name", "equal", "Band", 0), filter("name", "equal", "Vocals")}, "delete")}});
		CHECK(!f.object("tracks", f.folder).is_null()); // both lines must hold
		Json band = filter("name", "equal", "Band");
		band["join"] = "or";
		run(f.client, "project.applyRules", {{"rules", rules({band, filter("name", "equal", "Vocals")}, "delete")}});
		CHECK(f.object("tracks", f.folder).is_null());
		CHECK(f.object("tracks", f.vocals).is_null());
		run(f.client, "project.applyRules", {{"rules", rules({filter("mediaType", "equal", "marker")}, "delete")}});
		CHECK(f.object("tracks", f.markers).is_null());
		CHECK(history() == steps + 3);
		run(f.client, "history.undo", {{"steps", 3}});
		CHECK(!f.object("tracks", f.vocals).is_null());
		CHECK(!f.object("midiParts", f.empty).is_null());
	}
	SECTION("locked parts stay; random values repeat with the seed")
	{
		run(f.client, "event.set", {{"events", {f.full}}, {"lock", true}});
		run(f.client, "project.applyRules", {{"preset", "Delete/Empty parts"}});
		const Json move = {{"rules",
							   rules({filter("containerType", "equal", "part")}, "transform",
								   {action("position", "setRandom", 0, 4800)})},
			{"seed", 3}};
		run(f.client, "project.applyRules", move);
		CHECK(f.object("midiParts", f.full).at("position") == g_beatTicks);
		CHECK(f.object("midiParts", f.empty).is_null());
		run(f.client, "event.set", {{"events", {f.full}}, {"lock", false}});
		run(f.client, "project.applyRules", move);
		const Json first = f.object("midiParts", f.full).at("position");
		CHECK(first != g_beatTicks);
		run(f.client, "history.undo");
		run(f.client, "project.applyRules", move);
		CHECK(f.object("midiParts", f.full).at("position") == first);
	}
}

TEST_CASE("event.set renames parts, not audio events", "[integration]")
{
	Fixture f;
	run(f.client, "event.set", {{"events", {f.full, f.empty}}, {"name", "Piano"}});
	CHECK(f.object("midiParts", f.full).at("name") == "Piano");
	CHECK(f.object("midiParts", f.empty).at("name") == "Piano");
}

TEST_CASE("project rule presets: factory presets, and the user's kept in the settings folder", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::filesystem::path file = dir.path() / "project-rule-presets.json";
	Json mine = rules({filter("containerType", "equal", "part")}, "select");
	mine["post"] = Json::array({{{"command", "edit.zoomToSelection"}}});
	{
		Fixture f(dir.path());
		const Json listed = run(f.client, "projectRules.presets");
		CHECK(listed.at("presets").size() == factoryProjectRulePresets().size());
		CHECK(listed.at("presets")[0].at("name") == "Init");
		CHECK(listed.at("presets")[0].at("factory") == true);
		CHECK(!std::filesystem::exists(file));

		run(f.client, "projectRules.savePreset", {{"name", "Mine/Parts"}, {"rules", mine}});
		run(f.client, "projectRules.savePreset", {{"name", "Gone"}, {"rules", rules({}, "delete")}});
		run(f.client, "projectRules.removePreset", {{"name", "Gone"}});
		CHECK_THAT(reject(f.client, "projectRules.savePreset", {{"name", "Init"}, {"rules", mine}}),
			ContainsSubstring("factory preset"));
		CHECK_THAT(reject(f.client, "projectRules.removePreset", {{"name", "Init"}}), ContainsSubstring("factory"));
		CHECK_THAT(reject(f.client, "projectRules.removePreset", {{"name", "Gone"}}),
			ContainsSubstring("no project rule preset"));
		CHECK_THAT(reject(f.client, "projectRules.savePreset",
					   {{"name", "Bad"}, {"rules", rules({filter("mediaType", "equal", "video")}, "delete")}}),
			ContainsSubstring("no media type"));
		CHECK(f.find({{"preset", "Mine/Parts"}}).at("events") == Json{f.full, f.empty});
	}
	const Json stored = readJson(file).at("values").at("projectRules.presets");
	REQUIRE(stored.size() == 1);
	CHECK(stored[0].at("name") == "Mine/Parts");

	Json document = readJson(file);
	document["values"]["projectRules.presets"].push_back({{"name", "Broken"}, {"rules", {{"function", "shuffle"}}}});
	std::ofstream(file) << document.dump();
	{
		Fixture next(dir.path());
		const Json listed = run(next.client, "projectRules.presets");
		const Json& last = listed.at("presets").back();
		CHECK(last.at("name") == "Mine/Parts");
		CHECK(last.at("factory") == false);
		CHECK(last.at("rules") == toJson(projectRulesFromJson(mine))); // with its post command
		REQUIRE(listed.at("problems").size() == 1);
		CHECK_THAT(listed.at("problems")[0].get<std::string>(), ContainsSubstring("skipped preset 2"));
		run(next.client, "projectRules.savePreset", {{"name", "Mine/Again"}, {"rules", mine}});
	}
	CHECK(readJson(file).at("values").at("projectRules.presets").size() == 3); // Broken stays
}
