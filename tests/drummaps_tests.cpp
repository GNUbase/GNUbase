// SPDX-License-Identifier: GPL-3.0-or-later
// Drum maps in the engine (#129, docs/features/drum-editor.md): the project's maps and the GM Map, drumMap.add, set,
// remove, export and import, a track's map, playback through the O-notes, channels and mutes, input through the
// I-notes, and the drum editor's content and Drum-Map Link. tests/drumeditor_ui_tests.cpp drives the drum editor in
// the app.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;

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

		Json maps() { return call("project.get").at("drumMaps"); }

		Json map(uint64_t id)
		{
			for (const Json& each : maps())
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		uint64_t gm() { return maps().at(0).at("id").get<uint64_t>(); }

		Json trackJson()
		{
			const Json project = call("project.get");
			for (const Json& each : project.at("tracks"))
				if (each.at("id") == track)
					return each;
			return nullptr;
		}

		std::vector<Json> notes()
		{
			const Json project = call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
					return {each.at("notes").begin(), each.at("notes").end()};
			return {};
		}

		std::vector<engine::RenderNote> rendered() { return engine.renderSnapshots().acquire().channels.at(0).notes; }

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t track = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("a new project has the GM Map, and drum maps are added, copied, changed and removed", "[unit]")
{
	Fixture f;
	// Initially only the GM Map (reference manual p. 1239): 128 sounds, named after General MIDI's percussion.
	Json maps = f.maps();
	REQUIRE(maps.size() == 1);
	const Json& gm = maps.at(0);
	CHECK(gm.at("name") == "GM Map");
	REQUIRE(gm.at("sounds").size() == 128);
	CHECK(gm.at("sounds").at(36) ==
		Json{{"pitch", 36}, {"name", "Bass Drum 1"}, {"snap", "1/16"}, {"mute", false}, {"iNote", 36}, {"oNote", 36},
			{"channel", 9}, {"output", ""}});
	CHECK(gm.at("sounds").at(38).at("name") == "Acoustic Snare");
	CHECK(gm.at("sounds").at(42).at("name") == "Closed Hi-Hat");
	CHECK(gm.at("order").size() == 128);
	CHECK(gm.at("order").at(0) == 0);

	// New Map: "Empty Map" with sounds named Sound 1, Sound 2 and so on (p. 1236); New Copy copies a map.
	const Json empty = f.call("drumMap.add").at("map");
	CHECK(empty.at("name") == "Empty Map");
	CHECK(empty.at("sounds").at(0).at("name") == "Sound 1");
	CHECK(empty.at("sounds").at(127).at("name") == "Sound 128");
	CHECK(empty.at("sounds").at(5).at("channel").is_null()); // Any: the track's channel
	const Json copy = f.call("drumMap.add", {{"copy", f.gm()}, {"name", "My Kit"}}).at("map");
	CHECK(copy.at("name") == "My Kit");
	CHECK(copy.at("sounds") == gm.at("sounds"));
	CHECK(f.maps().size() == 3);

	// Every setting but the pitch can change (p. 1237).
	const auto id = copy.at("id").get<uint64_t>();
	const Json changed = f.call("drumMap.set",
							  {{"map", id}, {"name", "Kit"},
								  {"sounds",
									  {{{"pitch", 36}, {"name", "Kick"}, {"snap", "1/8"}, {"mute", true}, {"iNote", 40},
										  {"oNote", 24}, {"channel", nullptr}, {"output", "Drum Module"}}}}})
							 .at("map");
	CHECK(changed.at("name") == "Kit");
	CHECK(changed.at("sounds").at(36) ==
		Json{{"pitch", 36}, {"name", "Kick"}, {"snap", "1/8"}, {"mute", true}, {"iNote", 40}, {"oNote", 24},
			{"channel", nullptr}, {"output", "Drum Module"}});
	CHECK(changed.at("sounds").at(38) == gm.at("sounds").at(38));
	Json order = Json::array();
	for (int pitch = 127; pitch >= 0; --pitch)
		order.push_back(pitch);
	CHECK(f.call("drumMap.set", {{"map", id}, {"order", order}}).at("map").at("order") == order);

	CHECK_THAT(f.fails("drumMap.set", {{"map", id}, {"sounds", {{{"pitch", 128}}}}}), ContainsSubstring("pitch"));
	CHECK_THAT(f.fails("drumMap.set", {{"map", id}, {"sounds", {{{"pitch", 1}, {"snap", "1/7"}}}}}),
		ContainsSubstring("snap"));
	CHECK_THAT(f.fails("drumMap.set", {{"map", id}, {"sounds", {{{"pitch", 1}, {"oNote", 200}}}}}),
		ContainsSubstring("oNote"));
	CHECK_THAT(f.fails("drumMap.set", {{"map", id}, {"sounds", {{{"pitch", 1}, {"channel", 16}}}}}),
		ContainsSubstring("channel"));
	CHECK_THAT(f.fails("drumMap.set", {{"map", id}, {"order", {1, 2, 3}}}), ContainsSubstring("order"));
	CHECK_THAT(f.fails("drumMap.set", {{"map", 999}, {"name", "x"}}), ContainsSubstring("no drum map 999"));

	// Each change is one undo step.
	f.call("history.undo");
	CHECK(f.map(id).at("order").at(0) == 0);
	f.call("history.undo");
	CHECK(f.map(id).at("name") == "My Kit");

	// A track uses a map; removing the map takes it from the track (p. 1239).
	CHECK(f.trackJson().at("drumMap") == 0);
	f.call("track.set", {{"track", f.track}, {"drumMap", id}});
	CHECK(f.trackJson().at("drumMap") == id);
	const auto audio = f.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	CHECK_THAT(f.fails("track.set", {{"track", audio}, {"drumMap", id}}), ContainsSubstring("MIDI and instrument"));
	CHECK_THAT(f.fails("track.set", {{"track", f.track}, {"drumMap", 999}}), ContainsSubstring("no drum map 999"));
	f.call("drumMap.remove", {{"map", id}});
	CHECK(f.map(id).is_null());
	CHECK(f.trackJson().at("drumMap") == 0);
	f.call("history.undo");
	CHECK(f.map(id).at("name") == "My Kit");
	CHECK(f.trackJson().at("drumMap") == id);
}

TEST_CASE("drum maps export to a file and import from it unchanged", "[integration]")
{
	Fixture f;
	const test::TempDir dir;
	const std::string file = (dir.path() / "kit.drm").u8string();
	const auto id =
		f.call("drumMap.add", {{"copy", f.gm()}, {"name", "Studio Kit & <Co>"}}).at("map").at("id").get<uint64_t>();
	Json order = Json::array();
	for (int pitch = 0; pitch < 128; ++pitch)
		order.push_back((pitch + 36) % 128);
	f.call("drumMap.set",
		{{"map", id}, {"order", order},
			{"sounds",
				{{{"pitch", 36}, {"name", "Kick \"Deep\""}, {"snap", "1/8T"}, {"mute", true}, {"iNote", 48},
					 {"oNote", 35}, {"channel", 2}, {"output", "Module"}},
					{{"pitch", 38}, {"snap", "1/4."}, {"channel", nullptr}}, {{"pitch", 0}, {"name", "Zero"}}}}});
	const Json before = f.map(id);
	f.call("drumMap.export", {{"map", id}, {"path", file}});
	std::ifstream in(std::filesystem::u8path(file));
	const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	CHECK_THAT(text, ContainsSubstring("<DrumMap>"));

	const Json imported = f.call("drumMap.import", {{"path", file}}).at("map");
	CHECK(imported.at("id") != id);
	Json expected = before;
	expected["id"] = imported.at("id");
	CHECK(imported == expected);
	CHECK(f.maps().size() == 3);
	f.call("history.undo"); // one undo step
	CHECK(f.maps().size() == 2);

	CHECK_THAT(f.fails("drumMap.import", {{"path", (dir.path() / "missing.drm").u8string()}}),
		ContainsSubstring("missing.drm"));
	{
		std::ofstream bad(dir.path() / "bad.drm");
		bad << "<Something/>";
	}
	CHECK_THAT(f.fails("drumMap.import", {{"path", (dir.path() / "bad.drm").u8string()}}),
		ContainsSubstring("isn't a drum map"));
	CHECK_THAT(f.fails("drumMap.export", {{"map", 999}, {"path", file}}), ContainsSubstring("no drum map 999"));
}

TEST_CASE("drum maps and the tracks' maps are saved and loaded with the project", "[integration]")
{
	Fixture f;
	const test::TempDir dir;
	const std::string file = (dir.path() / "drums.dawproject").u8string();
	const auto id = f.call("drumMap.add", {{"name", "Mine"}}).at("map").at("id").get<uint64_t>();
	f.call("drumMap.set", {{"map", id}, {"sounds", {{{"pitch", 40}, {"oNote", 41}, {"mute", true}}}}});
	f.call("track.set", {{"track", f.track}, {"drumMap", id}});
	const Json before = f.call("project.get");
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	const Json after = f.call("project.get");
	CHECK(after.at("drumMaps") == before.at("drumMaps"));
	CHECK(f.trackJson().at("drumMap") == id);
}

TEST_CASE("a track's drum map plays its notes through the O-notes, channels and mutes", "[unit]")
{
	Fixture f;
	const auto id = f.call("drumMap.add", {{"copy", f.gm()}}).at("map").at("id").get<uint64_t>();
	f.call("drumMap.set",
		{{"map", id},
			{"sounds",
				{{{"pitch", 36}, {"oNote", 24}, {"channel", 3}}, {{"pitch", 38}, {"mute", true}},
					{{"pitch", 40}, {"channel", nullptr}}}}});
	f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 36}, {"velocity", 0.5}, {"channel", 5}},
					{{"position", 120}, {"length", 60}, {"pitch", 38}, {"velocity", 0.5}},
					{{"position", 240}, {"length", 60}, {"pitch", 40}, {"velocity", 0.5}, {"channel", 7}}}}});
	// Without a map the notes play as they are.
	std::vector<engine::RenderNote> notes = f.rendered();
	REQUIRE(notes.size() == 3);
	CHECK(notes[0].pitch == 36);
	CHECK(notes[0].channel == 5);

	f.call("track.set", {{"track", f.track}, {"drumMap", id}});
	notes = f.rendered();
	REQUIRE(notes.size() == 2); // the snare is muted
	CHECK(notes[0].pitch == 24);
	CHECK(notes[0].channel == 3);
	CHECK(notes[1].pitch == 40);
	CHECK(notes[1].channel == 7); // Any keeps the note's channel
	// The notes themselves keep their pitch.
	CHECK(f.notes().size() == 3);
}

TEST_CASE("notes played into a track with a drum map go through the I-notes", "[unit]")
{
	Fixture f;
	const auto id = f.call("drumMap.add", {{"copy", f.gm()}}).at("map").at("id").get<uint64_t>();
	// The bass drum's I-note is A1 (45); the low tom's own I-note moves away so that nothing else plays from 45.
	f.call("drumMap.set",
		{{"map", id}, {"sounds", {{{"pitch", 36}, {"iNote", 45}, {"snap", "1/8"}}, {{"pitch", 45}, {"iNote", 100}}}}});
	f.call("track.set", {{"track", f.track}, {"drumMap", id}});

	SECTION("step input, with the drum map's snap as the length")
	{
		f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}});
		const Json view =
			f.call("editor.set", {{"editor", "window"}, {"stepInput", true}, {"lengthQuantize", "drumMap"}});
		CHECK(view.at("lengthQuantize") == "drumMap");
		f.call("editor.input", {{"editor", "window"}, {"notes", {{{"pitch", 45}}, {{"pitch", 50}}}}});
		const std::vector<Json> notes = f.notes();
		REQUIRE(notes.size() == 2);
		CHECK(notes[0].at("pitch") == 36);
		CHECK(notes[0].at("length") == g_quarter / 2); // the bass drum's snap
		CHECK(notes[1].at("pitch") == 50);
		CHECK(notes[1].at("length") == g_quarter / 4); // the hi tom's: 1/16
	}
	SECTION("recording")
	{
		f.call("recording.add",
			{{"tracks",
				{{{"track", f.track}, {"takes", {{{"position", 0}, {"length", 48000}}}},
					{"notes", {{{"position", 0}, {"length", 1000}, {"pitch", 45}, {"velocity", 0.5}}}}}}}});
		const Json parts = f.call("project.get").at("midiParts");
		REQUIRE(parts.size() == 2);
		CHECK(parts.at(1).at("notes").at(0).at("pitch") == 36);
	}
	CHECK_THAT(f.fails("editor.set", {{"editor", "lower"}, {"lengthQuantize", "drum"}}), ContainsSubstring("drumMap"));
}

TEST_CASE("the drum editor is an editor content, and parts of a track with a drum map open in it", "[unit]")
{
	Fixture f;
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}, {"content", "drum"}}).at("content") ==
		"drum");
	f.call("editor.close", {{"editor", "window"}});
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}}).at("content") == "key");
	f.call("editor.close", {{"editor", "window"}});
	// Use Drum Editor when Drum Map is assigned (p. 1221).
	f.call("track.set", {{"track", f.track}, {"drumMap", f.gm()}});
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}}).at("content") == "drum");
	CHECK(f.call("editor.set", {{"editor", "window"}, {"content", "key"}}).at("content") == "key");
}
