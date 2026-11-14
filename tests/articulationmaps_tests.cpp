// SPDX-License-Identifier: GPL-3.0-or-later
// Articulation maps in the engine (#175, docs/features/articulation-maps.md): articulationMap.add, set, remove,
// export and import, a track's map, directions and attributes in MIDI parts, playback through the sound slots'
// output events and MIDI modifiers, and remote triggers in recordings.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/midi/articulationmaps.h"
#include "engine/midi/midivalues.h"
#include "engine/time/tempomap.h"
#include "platform/audio/device.h"
#include "platform/midi/dummyport.h"
#include "testutils/testutils.h"
#include "ui/articulationnames.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;

	// Strings: Arco and Pizzicato are directions of the group Technique, Staccato an attribute of the group Length.
	// Arco, the default slot, switches with the key C0 (24), Pizzicato with C#0 on channel 3, Staccato with
	// controller 32 at 64 (and back to 0 when it ends), at half the length and more velocity, and Pizzicato Staccato
	// with program 5, an octave up.
	Json stringsSettings()
	{
		return {{"name", "Strings"}, {"groups", {"Technique", "Length"}},
			{"articulations",
				{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
					{{"name", "Pizzicato"}, {"type", "direction"}, {"group", 0}},
					{{"name", "Staccato"}, {"type", "attribute"}, {"group", 1}}}},
			{"slots",
				{{{"name", "Arco"}, {"trigger", 36},
					 {"outputs", {{{"status", "note"}, {"data1", 24}, {"data2", 100}}}}},
					{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"trigger", 37}, {"channel", 2},
						{"outputs", {{{"status", "note"}, {"data1", 25}, {"data2", 100}}}}},
					{{"name", "Staccato"}, {"articulations", {"Staccato"}}, {"trigger", 38}, {"length", 50},
						{"velocity", 150},
						{"outputs",
							{{{"status", "controller"}, {"data1", 32}, {"data2", 64}},
								{{"status", "controller"}, {"data1", 32}, {"data2", 0}, {"off", true}}}}},
					{{"name", "Pizzicato Staccato"}, {"articulations", {"Pizzicato", "Staccato"}}, {"transpose", 12},
						{"outputs", {{{"status", "program"}, {"data1", 5}}}}}}}};
	}

	struct Fixture
	{
		Fixture()
		{
			track = call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
			part = call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
		}

		Json call(const std::string& command, Json parameters = Json::object())
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " succeeded: " << result.value.dump());
			REQUIRE(!result.ok());
			return result.error;
		}

		Json maps() { return call("project.get").at("articulationMaps"); }

		uint64_t strings()
		{
			const uint64_t id = call("articulationMap.add", stringsSettings()).at("map").at("id").get<uint64_t>();
			call("track.set", {{"track", track}, {"articulationMap", id}});
			return id;
		}

		Json trackJson()
		{
			const Json project = call("project.get");
			for (const Json& each : project.at("tracks"))
				if (each.at("id") == track)
					return each;
			return nullptr;
		}

		Json partJson(size_t index = 0) { return call("project.get").at("midiParts").at(index); }

		engine::RenderChannel rendered() { return engine.renderSnapshots().acquire().channels.at(0); }

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t track = 0;
		uint64_t part = 0;
	};

	const engine::RenderNote& notePlaying(const engine::RenderChannel& channel, int pitch)
	{
		const auto found = std::find_if(channel.notes.begin(), channel.notes.end(),
			[pitch](const engine::RenderNote& note) { return note.pitch == pitch; });
		REQUIRE(found != channel.notes.end());
		return *found;
	}
}

TEST_CASE("articulation maps are added, copied, changed and removed", "[unit]")
{
	Fixture f;
	CHECK(f.maps().empty());

	// Add Map (p. 1280): a new map has one group and neither articulations nor sound slots.
	const Json added = f.call("articulationMap.add", {{"name", "Brass"}}).at("map");
	CHECK(added.at("name") == "Brass");
	CHECK(added.at("groups") == Json{"Group 1"});
	CHECK(added.at("articulations").empty());
	CHECK(added.at("slots").empty());
	CHECK(added.at("remoteTrigger") == "keySwitch");
	CHECK(added.at("latch") == true);

	const Json strings = f.call("articulationMap.add", stringsSettings()).at("map");
	REQUIRE(strings.at("slots").size() == 4);
	CHECK(strings.at("slots").at(1) ==
		Json{{"name", "Pizzicato"}, {"color", ""}, {"articulations", {"Pizzicato"}}, {"trigger", 37},
			{"outputs", {{{"status", "note"}, {"data1", 25}, {"data2", 100}, {"off", false}}}}, {"channel", 2},
			{"length", 100}, {"velocity", 100}, {"transpose", 0}});
	CHECK(strings.at("slots").at(3).at("trigger").is_null());
	CHECK(strings.at("slots").at(3).at("channel").is_null());

	// Duplicate Map (p. 1267).
	const Json copy = f.call("articulationMap.add", {{"copy", strings.at("id")}}).at("map");
	CHECK(copy.at("name") == "Strings Copy");
	CHECK(copy.at("slots") == strings.at("slots"));
	CHECK(f.maps().size() == 3);

	const auto id = strings.at("id").get<uint64_t>();
	const Json changed =
		f.call("articulationMap.set", {{"map", id}, {"remoteTrigger", "programChange"}, {"latch", false}}).at("map");
	CHECK(changed.at("remoteTrigger") == "programChange");
	CHECK(changed.at("latch") == false);
	CHECK(changed.at("slots") == strings.at("slots"));
	f.call("history.undo");
	CHECK(f.maps().at(1) == strings);

	// What a map can't have.
	CHECK_THAT(f.fails("articulationMap.set", {{"map", id}, {"groups", Json::array()}}),
		ContainsSubstring("at least one group"));
	CHECK_THAT(
		f.fails("articulationMap.set",
			{{"map", id}, {"articulations", {{{"name", "Arco"}}, {{"name", "Arco"}}}}, {"slots", Json::array()}}),
		ContainsSubstring("Arco"));
	CHECK_THAT(f.fails("articulationMap.set",
				   {{"map", id}, {"articulations", {{{"name", "Arco"}, {"group", 2}}}}, {"slots", Json::array()}}),
		ContainsSubstring("group"));
	CHECK_THAT(f.fails("articulationMap.set", {{"map", id}, {"slots", {{{"name", "X"}, {"articulations", {"Col"}}}}}}),
		ContainsSubstring("Col"));
	// Mutually exclusive: Arco and Pizzicato are both of the group Technique.
	CHECK_THAT(f.fails("articulationMap.set",
				   {{"map", id}, {"slots", {{{"name", "X"}, {"articulations", {"Arco", "Pizzicato"}}}}}}),
		ContainsSubstring("one articulation of each group"));
	CHECK_THAT(f.fails("articulationMap.set", {{"map", id}, {"slots", {{{"name", "X"}, {"trigger", 128}}}}}),
		ContainsSubstring("trigger"));
	CHECK_THAT(f.fails("articulationMap.set",
				   {{"map", id}, {"slots", {{{"name", "X"}, {"outputs", {{{"status", "note"}, {"data1", 200}}}}}}}}),
		ContainsSubstring("data1"));
	CHECK_THAT(f.fails("articulationMap.set", {{"map", 999}}), ContainsSubstring("no articulation map 999"));
	// An articulation the slots use stays.
	CHECK_THAT(f.fails("articulationMap.set", {{"map", id}, {"articulations", {{{"name", "Arco"}}}}}),
		ContainsSubstring("Pizzicato"));

	// A track uses a map; removing the map takes it from the track.
	f.call("track.set", {{"track", f.track}, {"articulationMap", id}});
	CHECK(f.trackJson().at("articulationMap") == id);
	f.call("articulationMap.remove", {{"map", id}});
	CHECK(f.maps().size() == 2);
	CHECK(f.trackJson().at("articulationMap") == 0);
	f.call("history.undo");
	CHECK(f.trackJson().at("articulationMap") == id);

	const auto audio = f.call("track.add", {{"type", "audio"}}).at("track").at("id");
	CHECK_THAT(f.fails("track.set", {{"track", audio}, {"articulationMap", id}}), ContainsSubstring("MIDI"));
	CHECK_THAT(f.fails("track.set", {{"track", f.track}, {"articulationMap", 999}}),
		ContainsSubstring("no articulation map 999"));
}

TEST_CASE("directions are articulation controller events and attributes are the notes' articulations", "[unit]")
{
	Fixture f;
	f.strings();
	const Json direction =
		f.call("controller.add",
			 {{"part", f.part},
				 {"controllers", {{{"position", g_quarter}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}})
			.at("controllers")
			.at(0);
	CHECK(direction.at("type") == "articulation");
	CHECK(direction.at("articulation") == "Pizzicato");
	CHECK(direction.at("reset") == false);
	const Json changed =
		f.call("controller.set", {{"part", f.part}, {"controllers", {{{"id", direction.at("id")}, {"reset", true}}}}})
			.at("controllers")
			.at(0);
	CHECK(changed.at("reset") == true);
	CHECK_THAT(
		f.fails("controller.add", {{"part", f.part}, {"controllers", {{{"position", 0}, {"type", "articulation"}}}}}),
		ContainsSubstring("articulation"));

	const Json note = f.call("note.add",
						   {{"part", f.part},
							   {"notes",
								   {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5},
									   {"articulations", {"Staccato"}}}}}})
						  .at("notes")
						  .at(0);
	CHECK(note.at("articulations") == Json{"Staccato"});
	const Json cleared =
		f.call("note.set", {{"part", f.part}, {"notes", {{{"id", note.at("id")}, {"articulations", Json::array()}}}}})
			.at("notes")
			.at(0);
	CHECK(!cleared.contains("articulations"));
	// Directions play nothing themselves.
	CHECK(f.rendered().controllers.empty());
}

TEST_CASE("articulations play the sound slots' output events and MIDI modifiers", "[unit]")
{
	Fixture f;
	f.call("controller.add",
		{{"part", f.part},
			{"controllers", {{{"position", 2 * g_quarter}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	const auto note = [&f](int64_t position, int pitch, Json articulations)
	{
		f.call("note.add",
			{{"part", f.part},
				{"notes",
					{{{"position", position}, {"length", g_quarter / 2}, {"pitch", pitch}, {"velocity", 0.5},
						{"articulations", std::move(articulations)}}}}});
	};
	note(0, 60, Json::array());                                 // Arco, the default slot
	note(g_quarter, 61, {"Staccato"});                          // Staccato
	note(2 * g_quarter, 62, Json::array());                     // Pizzicato from the direction
	note((2 * g_quarter) + (g_quarter / 2), 63, Json::array()); // still Pizzicato: no output events
	note(3 * g_quarter, 64, {"Staccato"});                      // Pizzicato Staccato

	// Without a map the notes play as they are.
	REQUIRE(f.rendered().notes.size() == 5);
	CHECK(f.rendered().controllers.empty());

	f.strings();
	const engine::RenderChannel channel = f.rendered();
	REQUIRE(channel.notes.size() == 7); // and the key switches C0 and C#0
	const engine::RenderNote& arco = notePlaying(channel, 60);
	const engine::RenderNote& c0 = notePlaying(channel, 24);
	CHECK(c0.start == 0);
	CHECK(c0.end == 1);
	CHECK(c0.channel == 0);
	CHECK(c0.keySwitch); // a jump sends it again
	CHECK(arco.start == 0);
	CHECK_FALSE(arco.keySwitch);

	// Staccato: half the length, one and a half times the velocity, controller 32 at 64 where it starts.
	const engine::RenderNote& staccato = notePlaying(channel, 61);
	CHECK((staccato.end - staccato.start) * 2 == (arco.end - arco.start));
	CHECK(staccato.velocity > arco.velocity);
	REQUIRE(channel.controllers.size() == 3);
	CHECK(channel.controllers[0].position == staccato.start);
	CHECK(channel.controllers[0].type == engine::ControllerType::Control);
	CHECK(channel.controllers[0].number == 32);
	CHECK(channel.controllers[0].value == engine::midi::controllerFromMidi1(64));

	// Pizzicato: the key switch C#0 just before the note, both on channel 3; Staccato's off event where it ends.
	const engine::RenderNote& pizzicato = notePlaying(channel, 62);
	const engine::RenderNote& cSharp0 = notePlaying(channel, 25);
	CHECK(cSharp0.start == pizzicato.start - 1);
	CHECK(cSharp0.end == pizzicato.start);
	CHECK(cSharp0.channel == 2);
	CHECK(pizzicato.channel == 2);
	CHECK(notePlaying(channel, 63).channel == 2);
	CHECK(channel.controllers[1].position == pizzicato.start);
	CHECK(channel.controllers[1].number == 32);
	CHECK(channel.controllers[1].value == 0);

	// Pizzicato Staccato: an octave up, after program 5.
	const engine::RenderNote& both = notePlaying(channel, 76);
	CHECK(channel.controllers[2].position == both.start);
	CHECK(channel.controllers[2].type == engine::ControllerType::Program);
	CHECK(channel.controllers[2].number == 5);

	// A reset ends Pizzicato: the last notes play Arco and Staccato again.
	const auto direction = f.partJson().at("controllers").at(0).at("id");
	f.call("controller.set", {{"part", f.part}, {"controllers", {{{"id", direction}, {"reset", true}}}}});
	const engine::RenderChannel reset = f.rendered();
	CHECK(notePlaying(reset, 62).channel == 0);
	CHECK(notePlaying(reset, 64).end - notePlaying(reset, 64).start ==
		notePlaying(reset, 61).end - notePlaying(reset, 61).start);
}

TEST_CASE("the closest sound slot plays notes whose articulations no slot has", "[unit]")
{
	Fixture f;
	Json settings = stringsSettings();
	settings["articulations"].push_back({{"name", "Tremolo"}, {"type", "direction"}, {"group", 0}});
	settings["articulations"].push_back({{"name", "Accent"}, {"type", "attribute"}, {"group", 1}});
	const auto id = f.call("articulationMap.add", settings).at("map").at("id");
	f.call("track.set", {{"track", f.track}, {"articulationMap", id}});
	// Pizzicato with an accent: no slot has both, Pizzicato matches the most important group. Tremolo: no slot
	// matches, the default slot plays.
	f.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
					{{"position", g_quarter}, {"type", "articulation"}, {"articulation", "Tremolo"}}}}});
	f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}, {"articulations", {"Accent"}}},
					{{"position", g_quarter}, {"length", 60}, {"pitch", 61}, {"velocity", 0.5}}}}});
	const engine::RenderChannel channel = f.rendered();
	CHECK(notePlaying(channel, 60).channel == 2);
	CHECK(notePlaying(channel, 25).start == 0); // C#0
	CHECK(notePlaying(channel, 61).channel == 0);
	CHECK(notePlaying(channel, 24).start == notePlaying(channel, 61).start - 1); // C0
}

TEST_CASE(
	"the sound slots lane shows the slot each note plays, from the map and part as project.get lists them", "[unit]")
{
	Fixture f;
	f.strings();
	// Pizzicato from the start, reset at the second beat; a staccato note at the third.
	f.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
					{{"position", g_quarter}, {"type", "articulation"}, {"articulation", "Pizzicato"},
						{"reset", true}}}}});
	const Json added = f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", g_quarter}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", 2 * g_quarter}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5},
						{"articulations", {"Staccato"}}}}}});
	const std::map<uint64_t, size_t> slots = engine::midi::soundSlots(f.maps().at(0), f.partJson());
	REQUIRE(slots.size() == 3);
	CHECK(slots.at(added.at("notes").at(0).at("id").get<uint64_t>()) == 1); // Pizzicato
	CHECK(slots.at(added.at("notes").at(1).at("id").get<uint64_t>()) == 0); // reset: the default slot
	CHECK(slots.at(added.at("notes").at(2).at("id").get<uint64_t>()) == 2); // Staccato

	// articulationMap.soundSlots gives MCP and Lua clients the same, by the part (#1498).
	const Json listed = f.call("articulationMap.soundSlots", {{"part", f.part}});
	CHECK(listed.at("map") == f.maps().at(0).at("id"));
	REQUIRE(listed.at("notes").size() == 3);
	for (const Json& each : listed.at("notes"))
	{
		const auto note = each.at("note").get<uint64_t>();
		CHECK(each.at("slot") == slots.at(note));
		CHECK(each.at("name") == f.maps().at(0).at("slots").at(slots.at(note)).at("name"));
	}
	// A part of a track without a map lists none.
	f.call("track.set", {{"track", f.track}, {"articulationMap", 0}});
	CHECK(f.call("articulationMap.soundSlots", {{"part", f.part}}) == Json{{"map", 0}, {"notes", Json::array()}});
	CHECK_THAT(f.fails("articulationMap.soundSlots", {{"part", 999999}}), ContainsSubstring("999999"));
}

TEST_CASE("articulation names are joined and split at commas, as the info line and the list editor show them", "[unit]")
{
	CHECK(ui::joinNames({}).empty());
	CHECK(ui::joinNames({"Staccato", "Pizzicato"}) == "Staccato, Pizzicato");
	CHECK(ui::splitNames(" Staccato ,Pizzicato,, ") == std::vector<std::string>{"Staccato", "Pizzicato"});
	CHECK(ui::splitNames("").empty());
}

TEST_CASE("an Articulations lane is a lane of the editor views, which controller.line doesn't draw on", "[unit]")
{
	Fixture f;
	f.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part})}});
	const Json view = f.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "articulations"}}}}});
	CHECK(view.at("lanes") == Json::array({{{"type", "articulations"}, {"number", 0}}}));
	CHECK_THAT(f.fails("controller.line",
				   {{"part", f.part}, {"lane", {{"type", "articulations"}}}, {"mode", "line"},
					   {"from", {{"position", 0}, {"value", 0}}}, {"to", {{"position", g_quarter}, {"value", 1}}}}),
		ContainsSubstring("controller lane type"));
}

TEST_CASE("articulation maps export to a file and import from it unchanged", "[integration]")
{
	Fixture f;
	const test::TempDir dir;
	const std::string file = (dir.path() / "strings.articulationmap").u8string();
	const auto id = f.strings();
	Json before;
	for (const Json& map : f.maps())
		if (map.at("id") == id)
			before = map;
	f.call("articulationMap.export", {{"map", id}, {"path", file}});

	const Json imported = f.call("articulationMap.import", {{"path", file}}).at("map");
	CHECK(imported.at("id") != id);
	Json expected = before;
	expected["id"] = imported.at("id");
	CHECK(imported == expected);
	CHECK(f.maps().size() == 2);
	f.call("history.undo"); // one undo step
	CHECK(f.maps().size() == 1);

	CHECK_THAT(f.fails("articulationMap.import", {{"path", (dir.path() / "missing.articulationmap").u8string()}}),
		ContainsSubstring("missing.articulationmap"));
	{
		std::ofstream bad(dir.path() / "bad.articulationmap");
		bad << "{\"something\": 1}";
	}
	CHECK_THAT(f.fails("articulationMap.import", {{"path", (dir.path() / "bad.articulationmap").u8string()}}),
		ContainsSubstring("isn't an articulation map"));
	CHECK_THAT(f.fails("articulationMap.export", {{"map", 999}, {"path", file}}),
		ContainsSubstring("no articulation map 999"));
}

TEST_CASE("articulation maps, the tracks' maps and the articulations are saved and loaded", "[integration]")
{
	Fixture f;
	const test::TempDir dir;
	const std::string file = (dir.path() / "articulations.dawproject").u8string();
	const auto id = f.strings();
	f.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
					{{"position", g_quarter}, {"type", "articulation"}, {"articulation", "Arco"}, {"reset", true}}}}});
	f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5},
					{"articulations", {"Staccato"}}}}}});
	const Json before = f.call("project.get");
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	const Json after = f.call("project.get");
	CHECK(after.at("articulationMaps") == before.at("articulationMaps"));
	CHECK(after.at("midiParts") == before.at("midiParts"));
	CHECK(f.trackJson().at("articulationMap") == id);
}

TEST_CASE("remote triggers in a recording select sound slots", "[unit]")
{
	Fixture f;
	const auto id = f.strings();
	const auto record = [&f](Json notes, Json controllers)
	{
		f.call("recording.add",
			{{"tracks",
				{{{"track", f.track}, {"takes", {{{"position", 0}, {"length", 48000}}}}, {"notes", std::move(notes)},
					{"controllers", std::move(controllers)}}}}});
		const Json parts = f.call("project.get").at("midiParts");
		REQUIRE(parts.size() == 2);
		return parts.at(1);
	};
	const auto key = [](int64_t position, int64_t length, int pitch)
	{ return Json{{"position", position}, {"length", length}, {"pitch", pitch}, {"velocity", 0.5}}; };
	const auto directions = [](const Json& part)
	{
		std::vector<std::pair<std::string, bool>> found;
		for (const Json& controller : part.at("controllers"))
			if (controller.at("type") == "articulation")
				found.emplace_back(
					controller.at("articulation").get<std::string>(), controller.at("reset").get<bool>());
		return found;
	};
	using Directions = std::vector<std::pair<std::string, bool>>;

	SECTION("key switches with latch: the slot stays until the next key switch")
	{
		// 37 selects Pizzicato, 38 Staccato, 36 Arco, the default slot.
		const Json part = record(Json::array({key(0, 100, 37), key(1000, 500, 60), key(20000, 100, 38),
									 key(24000, 500, 62), key(30000, 100, 36), key(40000, 500, 64)}),
			Json::array());
		const Json& notes = part.at("notes");
		REQUIRE(notes.size() == 3); // the key switches aren't recorded as notes
		CHECK(notes.at(0).at("pitch") == 60);
		CHECK(!notes.at(0).contains("articulations"));
		CHECK(notes.at(1).at("articulations") == Json{"Staccato"});
		CHECK(!notes.at(2).contains("articulations"));
		// Pizzicato, then a reset of its group for Staccato and Arco, which don't have one of its directions.
		CHECK(directions(part) == Directions{{"Pizzicato", false}, {"Arco", true}, {"Arco", true}});
	}
	SECTION("key switches without latch: the default slot is back when the key is released")
	{
		f.call("articulationMap.set", {{"map", id}, {"latch", false}});
		const Json part =
			record(Json::array({key(0, 5000, 37), key(1000, 500, 60), key(6000, 500, 62)}), Json::array());
		CHECK(part.at("notes").size() == 2);
		CHECK(directions(part) == Directions{{"Pizzicato", false}, {"Arco", true}});
		CHECK(part.at("controllers").at(1).at("position").get<int64_t>() > 0);
	}
	SECTION("program changes")
	{
		f.call("articulationMap.set",
			{{"map", id}, {"remoteTrigger", "programChange"},
				{"slots",
					{{{"name", "Arco"}, {"trigger", 0}},
						{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"trigger", 1}}}}});
		const Json part = record(Json::array({key(1000, 500, 37)}),
			Json::array({{{"position", 0}, {"type", "program"}, {"number", 1}},
				{{"position", 10}, {"type", "program"}, {"number", 9}}}));
		CHECK(part.at("notes").size() == 1); // without key switches, notes stay notes
		CHECK(directions(part) == Directions{{"Pizzicato", false}});
		CHECK(part.at("controllers").size() == 2); // program 9 triggers nothing
	}
}

TEST_CASE("renaming or removing an articulation does so in the parts of the tracks that use the map", "[unit]")
{
	Fixture f;
	const auto id = f.strings();
	// Another track with a part that has the same names, without the map: it keeps them.
	const auto other = f.call("track.add", {{"type", "midi"}}).at("track").at("id");
	const auto otherPart =
		f.call("midiPart.add", {{"track", other}, {"position", 0}, {"length", g_bar}}).at("part").at("id");
	for (const Json& part : {Json(f.part), otherPart})
	{
		f.call("controller.add",
			{{"part", part},
				{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
		f.call("note.add",
			{{"part", part},
				{"notes",
					{{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5},
						{"articulations", {"Staccato"}}}}}});
	}
	const auto partNamed = [&f](const Json& id)
	{
		Json found;
		const Json project = f.call("project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("id") == id)
				found = part;
		REQUIRE(found.is_object());
		return found;
	};
	const Json before = partNamed(f.part);

	// Renamed where they are in the list, as the setup renames them: Pizzicato to Pizz, Staccato to Stacc.
	Json settings = stringsSettings();
	settings["articulations"][1]["name"] = "Pizz";
	settings["articulations"][2]["name"] = "Stacc";
	settings["slots"][1]["articulations"] = {"Pizz"};
	settings["slots"][2]["articulations"] = {"Stacc"};
	settings["slots"][3]["articulations"] = {"Pizz", "Stacc"};
	settings["map"] = id;
	f.call("articulationMap.set", settings);
	CHECK(partNamed(f.part).at("controllers").at(0).at("articulation") == "Pizz");
	CHECK(partNamed(f.part).at("notes").at(0).at("articulations") == Json{"Stacc"});
	CHECK(partNamed(otherPart).at("controllers").at(0).at("articulation") == "Pizzicato");
	CHECK(partNamed(otherPart).at("notes").at(0).at("articulations") == Json{"Staccato"});
	f.call("history.undo"); // with the map, in one step
	CHECK(partNamed(f.part) == before);

	// Removed: the directions go, and the notes lose the attribute.
	f.call("articulationMap.set",
		{{"map", id}, {"articulations", {stringsSettings().at("articulations").at(0)}},
			{"slots", {stringsSettings().at("slots").at(0)}}});
	CHECK(partNamed(f.part).at("controllers").empty());
	CHECK(!partNamed(f.part).at("notes").at(0).contains("articulations"));
	CHECK(partNamed(otherPart).at("controllers").size() == 1);
	f.call("history.undo");
	CHECK(partNamed(f.part) == before);
}

TEST_CASE("renaming or removing an articulation does so in shared copies and other track versions", "[unit]")
{
	Fixture f;
	const auto id = f.strings();
	f.call("controller.add",
		{{"part", f.part},
			{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5},
					{"articulations", {"Staccato"}}}}}});
	// Pizzicato renamed to Pizz and Staccato to Stacc, as the setup renames them, or both removed.
	Json renamed = stringsSettings();
	renamed["articulations"][1]["name"] = "Pizz";
	renamed["articulations"][2]["name"] = "Stacc";
	renamed["slots"][1]["articulations"] = {"Pizz"};
	renamed["slots"][2]["articulations"] = {"Stacc"};
	renamed["slots"][3]["articulations"] = {"Pizz", "Stacc"};
	renamed["map"] = id;
	const Json removed = {{"map", id}, {"articulations", {stringsSettings().at("articulations").at(0)}},
		{"slots", {stringsSettings().at("slots").at(0)}}};
	// The direction's name, or empty without one, and the note's attributes, of the part on a track.
	const auto onTrack = [&f](const Json& track)
	{
		std::pair<std::string, Json> found;
		const Json project = f.call("project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
			{
				const Json& controllers = part.at("controllers");
				found.first = controllers.empty() ? "" : controllers.at(0).at("articulation").get<std::string>();
				found.second = part.at("notes").at(0).value("articulations", Json::array());
			}
		return found;
	};
	using Found = std::pair<std::string, Json>;

	SECTION("a shared copy on a track without the map")
	{
		const auto other = f.call("track.add", {{"type", "instrument"}}).at("track").at("id");
		f.call("event.copy", {{"events", {f.part}}, {"ticks", g_bar}, {"track", other}, {"shared", true}});
		f.call("articulationMap.set", renamed);
		CHECK(onTrack(other) == Found{"Pizz", {"Stacc"}});
		f.call("history.undo");
		CHECK(onTrack(other) == Found{"Pizzicato", {"Staccato"}});
		f.call("articulationMap.set", removed);
		CHECK(onTrack(other) == Found{"", Json::array()});
	}
	SECTION("the track's other version")
	{
		// The second version, a copy of the first, is active; the first one keeps the part.
		f.call("trackVersion.duplicate", {{"tracks", {f.track}}});
		const auto second = f.trackJson().at("activeVersion");
		f.call("articulationMap.set", renamed);
		f.call("trackVersion.activate", {{"tracks", {f.track}}, {"version", 1}});
		CHECK(onTrack(f.track) == Found{"Pizz", {"Stacc"}});
		f.call("articulationMap.set", removed);
		f.call("trackVersion.activate", {{"tracks", {f.track}}, {"version", second}});
		CHECK(onTrack(f.track) == Found{"", Json::array()});
	}
}

TEST_CASE("a key switch plays before the note it comes with, whatever its pitch", "[unit]")
{
	Fixture f;
	f.strings();
	// At the project's start the key switch C#0 (25) can't come a sample before the note, which is lower.
	f.call("controller.add",
		{{"part", f.part},
			{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.call("note.add",
		{{"part", f.part}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 20}, {"velocity", 0.5}}}}});
	const engine::RenderChannel channel = f.rendered();
	REQUIRE(channel.notes.size() == 2);
	CHECK(channel.notes[0].pitch == 25);
	CHECK(channel.notes[0].keySwitch);
	CHECK(channel.notes[1].pitch == 20);
	CHECK(channel.notes[0].start == channel.notes[1].start);
}

TEST_CASE("notes that don't play send no output events", "[unit]")
{
	Fixture f;
	f.strings();
	// A Staccato note that never plays, then an Arco one.
	Json silent = {{"position", 0}, {"length", 60}, {"pitch", 61}, {"velocity", 0.5}, {"articulations", {"Staccato"}}};
	SECTION("its probability")
	{
		silent["probability"] = 0.0;
	}
	SECTION("a muted drum sound")
	{
		const auto drums = f.call("drumMap.add", {{"name", "Kit"}}).at("map").at("id");
		f.call("drumMap.set", {{"map", drums}, {"sounds", {{{"pitch", 61}, {"mute", true}}}}});
		f.call("track.set", {{"track", f.track}, {"drumMap", drums}});
	}
	f.call("note.add",
		{{"part", f.part},
			{"notes", {silent, {{"position", g_quarter}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	const engine::RenderChannel channel = f.rendered();
	CHECK(channel.controllers.empty()); // neither Staccato's controller 32 nor its off event
	REQUIRE(channel.notes.size() == 2);
	CHECK(notePlaying(channel, 24).end == notePlaying(channel, 60).start); // C0, Arco's key switch, before it
}

TEST_CASE(
	"a track sends a slot's output events where it differs from the last slot the track sent, across parts", "[unit]")
{
	Fixture f;
	f.strings();
	// Bar 1 plays Staccato, bars 2 and 3 Arco, each in a part of its own.
	f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 61}, {"velocity", 0.5},
					{"articulations", {"Staccato"}}}}}});
	for (const int64_t bar : {1, 2})
	{
		const Json part = f.call("midiPart.add", {{"track", f.track}, {"position", bar * g_bar}, {"length", g_bar}})
							  .at("part")
							  .at("id");
		f.call("note.add",
			{{"part", part}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	}
	// At 120 BPM and 48 kHz a bar is 96000 samples. Staccato's off event and C0 where bar 2 starts, not again in
	// bar 3.
	const engine::RenderChannel channel = f.rendered();
	REQUIRE(channel.controllers.size() == 2);
	CHECK(channel.controllers[0].position == 0);
	CHECK(channel.controllers[0].value == engine::midi::controllerFromMidi1(64));
	CHECK(channel.controllers[1].position == 96000);
	CHECK(channel.controllers[1].number == 32);
	CHECK(channel.controllers[1].value == 0);
	std::vector<int64_t> keySwitches;
	for (const engine::RenderNote& note : channel.notes)
		if (note.keySwitch)
			keySwitches.push_back(note.start);
	CHECK(keySwitches == std::vector<int64_t>{96000 - 1});
}

TEST_CASE("Merge MIDI in Loop takes the directions in effect where the range starts", "[unit]")
{
	Fixture f;
	f.strings();
	f.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "articulation"}, {"articulation", "Arco"}},
					{{"position", 10}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.call("note.add",
		{{"part", f.part},
			{"notes", {{{"position", 2 * g_quarter}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	// At 120 BPM and 48 kHz: from an eighth to the bar's end.
	const Json changes = f.call("midi.merge", {{"start", 12000}, {"end", 96000}});
	Json merged;
	const Json project = f.call("project.get");
	for (const Json& part : project.at("midiParts"))
		if (part.at("id") == changes.at("parts").at(0))
			merged = part;
	REQUIRE(merged.at("controllers").size() == 1); // Pizzicato, the last of its group before the range
	CHECK(merged.at("controllers").at(0).at("articulation") == "Pizzicato");
	CHECK(merged.at("controllers").at(0).at("position") == 0);
	CHECK(merged.at("notes").size() == 1);
}

TEST_CASE("Merge MIDI in Loop takes no directions from parts that end before the range", "[unit]")
{
	Fixture f;
	f.strings();
	// Part A, bar 1, with Pizzicato; part B, bar 3, with a note and no directions: B plays Arco.
	f.call("controller.add",
		{{"part", f.part},
			{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	const Json b =
		f.call("midiPart.add", {{"track", f.track}, {"position", 2 * g_bar}, {"length", g_bar}}).at("part").at("id");
	f.call("note.add", {{"part", b}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	// At 120 BPM and 48 kHz a bar is 96000 samples: bar 3.
	const Json changes = f.call("midi.merge", {{"start", 192000}, {"end", 288000}});
	Json merged;
	const Json project = f.call("project.get");
	for (const Json& part : project.at("midiParts"))
		if (part.at("id") == changes.at("parts").at(0))
			merged = part;
	CHECK(merged.at("controllers").empty());
	CHECK(merged.at("notes").size() == 1);
}

TEST_CASE("Merge MIDI in Loop starts each part with the default slot, on a new track with the map", "[unit]")
{
	Fixture f;
	const auto map = f.strings();
	// Part A, bar 1, plays Pizzicato; part B, bar 2, has no directions: it plays Arco.
	f.call("controller.add",
		{{"part", f.part},
			{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	const Json b =
		f.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}}).at("part").at("id");
	for (const Json& part : {Json(f.part), b})
		f.call("note.add",
			{{"part", part}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	// At 120 BPM and 48 kHz a bar is 96000 samples: bars 1 and 2, onto a new track.
	const Json changes = f.call("midi.merge", {{"start", 0}, {"end", 192000}});
	const Json project = f.call("project.get");
	Json merged;
	for (const Json& part : project.at("midiParts"))
		if (part.at("id") == changes.at("parts").at(0))
			merged = part;
	REQUIRE(merged.at("track") == changes.at("tracks").at(0));
	for (const Json& track : project.at("tracks"))
		if (track.at("id") == merged.at("track"))
			CHECK(track.at("articulationMap") == map);
	// Pizzicato for A's note, a reset of its group where B starts.
	std::vector<std::tuple<int64_t, std::string, bool>> directions;
	for (const Json& controller : merged.at("controllers"))
		directions.emplace_back(controller.at("position").get<int64_t>(),
			controller.at("articulation").get<std::string>(), controller.at("reset").get<bool>());
	std::sort(directions.begin(), directions.end());
	CHECK(directions ==
		std::vector<std::tuple<int64_t, std::string, bool>>{{0, "Pizzicato", false}, {g_bar, "Arco", true}});
}

TEST_CASE("Merge MIDI in Loop adds resets only where a direction of their group is in effect", "[unit]")
{
	Fixture f;
	f.strings();
	// Part B, bar 2, starts where no direction of Technique is in effect in the merged part: A has none, or a
	// reset of its own before B.
	SECTION("a part with attributes only")
	{
		f.call("note.add",
			{{"part", f.part},
				{"notes",
					{{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5},
						{"articulations", {"Staccato"}}}}}});
	}
	SECTION("a part that resets its direction")
	{
		f.call("controller.add",
			{{"part", f.part},
				{"controllers",
					{{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
						{{"position", g_quarter}, {"type", "articulation"}, {"articulation", "Arco"},
							{"reset", true}}}}});
	}
	const Json b =
		f.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}}).at("part").at("id");
	f.call("note.add", {{"part", b}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	// At 120 BPM and 48 kHz a bar is 96000 samples: bars 1 and 2.
	const Json changes = f.call("midi.merge", {{"start", 0}, {"end", 192000}});
	Json merged;
	const Json project = f.call("project.get");
	for (const Json& part : project.at("midiParts"))
		if (part.at("id") == changes.at("parts").at(0))
			merged = part;
	REQUIRE(merged.is_object());
	for (const Json& controller : merged.at("controllers"))
		CHECK(controller.at("position").get<int64_t>() < g_bar); // A's own, no reset where B starts
}

TEST_CASE(
	"Merge MIDI in Loop gives every new part the directions, and the track's key switches stay as they were", "[unit]")
{
	Fixture f;
	f.strings();
	// Part A, bar 1, plays Arco; part B, bar 2, plays Pizzicato with a velocity of 20 and a note with velocity
	// variance, which merge keeps on a new part of its own (#1240); part C, bar 3, plays Arco again, in the new part
	// with A's notes, after B's part of its own sent C#0 (#1491).
	f.call("note.add",
		{{"part", f.part}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	const Json b =
		f.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}}).at("part").at("id");
	f.call("midiPart.set", {{"part", b}, {"velocity", 20}});
	f.call("controller.add",
		{{"part", b}, {"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.call("note.add",
		{{"part", b},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 62}, {"velocity", 0.5}, {"velocityVariance", 0.5}}}}});
	const Json c =
		f.call("midiPart.add", {{"track", f.track}, {"position", 2 * g_bar}, {"length", g_bar}}).at("part").at("id");
	f.call("note.add", {{"part", c}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 64}, {"velocity", 0.5}}}}});
	// The key switches a track plays: start, pitch and channel.
	const auto keySwitches = [&](uint64_t track)
	{
		std::vector<std::tuple<int64_t, int, int>> played;
		for (const engine::RenderChannel& channel : f.engine.renderSnapshots().acquire().channels)
			if (channel.track == track)
				for (const engine::RenderNote& note : channel.notes)
					if (note.keySwitch)
						played.emplace_back(note.start, note.pitch, note.channel);
		std::sort(played.begin(), played.end());
		return played;
	};
	const auto before = keySwitches(f.track);
	REQUIRE(before.size() == 3); // C0 for Arco, C#0 for Pizzicato, C0 for Arco
	// At 120 BPM and 48 kHz a bar is 96000 samples: bars 1 to 3, onto a new track.
	const Json changes = f.call("midi.merge", {{"start", 0}, {"end", 288000}});
	REQUIRE(changes.at("parts").size() == 2);
	CHECK(keySwitches(changes.at("tracks").at(0).get<uint64_t>()) == before);
}

TEST_CASE("Merge MIDI in Loop shares one lane between the tracks' directions", "[unit]")
{
	using Directions = std::vector<std::tuple<int64_t, std::string, bool>>;
	Fixture f;
	const auto map = f.strings();
	// Track A's part, bar 1, plays Pizzicato; track B, with the same map, has a part from beat 3.
	Directions expected;
	SECTION("A's direction in effect where B's part starts: a reset there ends it")
	{
		f.call("controller.add",
			{{"part", f.part},
				{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
		expected = {{0, "Pizzicato", false}, {2 * g_quarter, "Arco", true}};
	}
	SECTION("A's direction reset before B's part starts: no reset there")
	{
		f.call("controller.add",
			{{"part", f.part},
				{"controllers",
					{{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
						{{"position", g_quarter}, {"type", "articulation"}, {"articulation", "Arco"},
							{"reset", true}}}}});
		expected = {{0, "Pizzicato", false}, {g_quarter, "Arco", true}};
	}
	const auto other = f.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	f.call("track.set", {{"track", other}, {"articulationMap", map}});
	const Json b =
		f.call("midiPart.add", {{"track", other}, {"position", 2 * g_quarter}, {"length", g_bar}}).at("part").at("id");
	f.call("note.add", {{"part", b}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 60}, {"velocity", 0.5}}}}});
	// At 120 BPM and 48 kHz a bar is 96000 samples: bars 1 and 2 of both tracks, onto a new track.
	const Json changes = f.call("midi.merge", {{"start", 0}, {"end", 192000}, {"tracks", {f.track, other}}});
	Json merged;
	const Json project = f.call("project.get");
	for (const Json& part : project.at("midiParts"))
		if (part.at("id") == changes.at("parts").at(0))
			merged = part;
	REQUIRE(merged.is_object());
	Directions directions;
	for (const Json& controller : merged.at("controllers"))
		directions.emplace_back(controller.at("position").get<int64_t>(),
			controller.at("articulation").get<std::string>(), controller.at("reset").get<bool>());
	std::sort(directions.begin(), directions.end());
	CHECK(directions == expected);
}

TEST_CASE("track presets and track import bring a track's articulation map along", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine(audio::makeBackends(), dir.path() / "settings");
	engine::Client client(engine);
	const auto call = [&client](const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	const auto track = call("track.add", {{"type", "midi"}, {"name", "Strings"}}).at("track").at("id");
	const auto map = call("articulationMap.add", stringsSettings()).at("map");
	call("track.set", {{"track", track}, {"articulationMap", map.at("id")}});
	call("track.savePreset", {{"tracks", {track}}, {"name", "Strings"}});
	const std::string archive = (dir.path() / "Strings.dawproject").u8string();
	call("track.export", {{"tracks", {track}}, {"path", archive}});
	// The map that the track has, but for its id.
	const auto mapOf = [&call](size_t index)
	{
		const Json project = call("project.get");
		const auto id = project.at("tracks").at(index).at("articulationMap");
		for (Json each : project.at("articulationMaps"))
			if (each.at("id") == id)
			{
				each.erase("id");
				return each;
			}
		return Json();
	};
	Json expected = map;
	expected.erase("id");

	SECTION("presets")
	{
		call("project.new");
		call("track.addFromPreset", {{"name", "Strings"}});
		CHECK(mapOf(0) == expected);
		// A second track from the preset uses the same map.
		call("track.addFromPreset", {{"name", "Strings"}});
		CHECK(call("project.get").at("articulationMaps").size() == 1);
		CHECK(mapOf(1) == expected);
	}
	SECTION("track import")
	{
		call("project.new");
		call("track.import", {{"path", archive}});
		CHECK(mapOf(0) == expected);
		call("track.import", {{"path", archive}});
		CHECK(call("project.get").at("articulationMaps").size() == 1);
		CHECK(mapOf(1) == expected);
	}
}

TEST_CASE("remote triggers played into a track select sound slots", "[integration]")
{
	// An instrument track that monitors, playing the note probe with the map Strings, on the dummy device.
	engine::Engine engine(audio::makeBackends(), {}, std::vector<std::unique_ptr<midi::Backend>>());
	engine::Client client(engine);
	const auto call = [&client](const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	const Json added = call("track.add", {{"type", "instrument"}});
	const auto track = added.at("track").at("id");
	const auto channel = added.at("channel").at("id");
	call("channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"monitor", true}});
	const auto map = call("articulationMap.add", stringsSettings()).at("map").at("id");
	call("track.set", {{"track", track}, {"articulationMap", map}});
	call("audio.render", {{"frames", 64}}); // the new graph
	call("midi.capture", {{"start", true}});
	// Plays MIDI messages into the track: what the instrument gets, each message's first two bytes, and a control
	// change's value.
	const auto play = [&](const std::vector<std::vector<int>>& messages)
	{
		Json injected = Json::array();
		for (const std::vector<int>& bytes : messages)
			injected.push_back({{"position", 0}, {"bytes", bytes}});
		call("midi.inject", {{"channel", channel}, {"messages", injected}});
		call("audio.render", {{"frames", 64}});
		std::vector<std::vector<int>> played;
		const Json capture = call("midi.capture");
		for (const Json& captured : capture.at("captured"))
		{
			const Json& bytes = captured.at("bytes");
			std::vector<int> message = {bytes.at(0).get<int>(), bytes.at(1).get<int>()};
			if ((message[0] & 0xF0) == 0xB0)
				message.push_back(bytes.at(2).get<int>());
			played.push_back(message);
		}
		return played;
	};
	using Played = std::vector<std::vector<int>>;

	SECTION("key switches")
	{
		// C#1 selects Pizzicato: its key switch C#0, then the notes on channel 3; the trigger doesn't play.
		CHECK(play({{0x90, 37, 100}, {0x80, 37, 0}, {0x90, 60, 100}, {0x80, 60, 0}}) ==
			Played{{0x92, 25}, {0x82, 25}, {0x92, 60}, {0x82, 60}});
		// D1 selects Staccato: controller 32 at 64, the notes on their own channel again. The same key again
		// sends nothing.
		CHECK(play({{0x90, 38, 100}, {0x80, 38, 0}, {0x90, 38, 100}, {0x80, 38, 0}, {0x91, 61, 100}, {0x81, 61, 0}}) ==
			Played{{0xB0, 32, 64}, {0x91, 61}, {0x81, 61}});
		// Without latch the default slot, Arco, comes back where the key is released: Staccato's off event, then
		// Arco's key switch C0.
		call("articulationMap.set", {{"map", map}, {"latch", false}});
		call("audio.render", {{"frames", 64}});
		CHECK(play({{0x90, 37, 100}, {0x90, 62, 100}, {0x80, 62, 0}, {0x80, 37, 0}, {0x90, 63, 100}, {0x80, 63, 0}}) ==
			Played{{0xB0, 32, 0}, {0x92, 25}, {0x82, 25}, {0x92, 62}, {0x82, 62}, {0x90, 24}, {0x80, 24}, {0x90, 63},
				{0x80, 63}});
	}
	SECTION("program changes")
	{
		call("articulationMap.set",
			{{"map", map}, {"remoteTrigger", "programChange"},
				{"slots",
					{{{"name", "Arco"}, {"trigger", 0}},
						{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"trigger", 1}, {"transpose", 12},
							{"outputs", {{{"status", "program"}, {"data1", 40}}}}}}}});
		call("audio.render", {{"frames", 64}});
		// Program 2 selects Pizzicato, which sends program 41 and plays the notes an octave up; program 10 is no
		// trigger and plays; key switches are notes again.
		CHECK(play({{0xC0, 1}, {0x90, 37, 100}, {0x80, 37, 0}, {0xC0, 9}}) ==
			Played{{0xC0, 40}, {0x90, 49}, {0x80, 49}, {0xC0, 9}});
	}
	SECTION("another map or other slots start from the default slot")
	{
		// Pizzicato selected, then another map with the same slots: the notes play the default slot, Arco, on their
		// own channel, until C#1 selects Pizzicato of the new map, with its key switch.
		CHECK(play({{0x90, 37, 100}, {0x80, 37, 0}}) == Played{{0x92, 25}, {0x82, 25}});
		const auto copy = call("articulationMap.add", {{"copy", map}}).at("map").at("id");
		call("track.set", {{"track", track}, {"articulationMap", copy}});
		call("audio.render", {{"frames", 64}});
		CHECK(play({{0x90, 60, 100}, {0x80, 60, 0}, {0x90, 37, 100}, {0x80, 37, 0}}) ==
			Played{{0x90, 60}, {0x80, 60}, {0x92, 25}, {0x82, 25}});
		// Other slots in the map: the default slot again.
		Json slots = stringsSettings().at("slots");
		slots[1]["transpose"] = 12;
		call("articulationMap.set", {{"map", copy}, {"slots", slots}});
		call("audio.render", {{"frames", 64}});
		CHECK(play({{0x90, 60, 100}, {0x80, 60, 0}}) == Played{{0x90, 60}, {0x80, 60}});
	}
	SECTION("renaming or recoloring a slot keeps the one selected")
	{
		// Pizzicato selected, then the setup renames and recolors it: the notes still play on its channel 3.
		CHECK(play({{0x90, 37, 100}, {0x80, 37, 0}}) == Played{{0x92, 25}, {0x82, 25}});
		Json slots = stringsSettings().at("slots");
		slots[1]["name"] = "Pizz.";
		slots[1]["color"] = "#ff0000";
		slots[1]["length"] = 80;                   // the notes played in don't end yet
		slots[2]["articulations"] = Json::array(); // what notes select it with doesn't play from the input either
		call("articulationMap.set", {{"map", map}, {"slots", slots}});
		call("audio.render", {{"frames", 64}});
		CHECK(play({{0x90, 60, 100}, {0x80, 60, 0}}) == Played{{0x92, 60}, {0x82, 60}});
	}
	SECTION("a trigger key's note-off ends the note its note-on played")
	{
		// C#1 pressed without a map plays; with the map assigned while it is held, its release still ends it.
		call("track.set", {{"track", track}, {"articulationMap", 0}});
		call("audio.render", {{"frames", 64}});
		CHECK(play({{0x90, 37, 100}}) == Played{{0x90, 37}});
		call("track.set", {{"track", track}, {"articulationMap", map}});
		call("audio.render", {{"frames", 64}});
		CHECK(play({{0x80, 37, 0}}) == Played{{0x80, 37}});
	}
}
