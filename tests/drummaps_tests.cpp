// SPDX-License-Identifier: GPL-3.0-or-later
// Drum maps in the engine (#129, docs/features/drum-editor.md): the project's maps and the GM Map, drumMap.add, set,
// remove, export and import, a track's map, playback and MIDI thru through the O-notes, channels and mutes, input
// through the I-notes, O-Note Conversion, and the drum editor's content and Drum-Map Link.
// tests/drumeditor_ui_tests.cpp drives the drum editor in the app.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/midi/drummaps.h"
#include "engine/time/tempomap.h"
#include "platform/audio/device.h"
#include "platform/midi/dummyport.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
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
	// The drum editor's Pitch Visibility and its option, and Show Note Length (pp. 1223-1224, 1227, 1234).
	Json view = f.call("editor.get", {{"editor", "window"}});
	CHECK(view.at("pitchVisibility") == false);
	CHECK(view.at("reverseSounds") == false);
	CHECK(view.at("showNoteLength") == false);
	view = f.call("editor.set",
		{{"editor", "window"}, {"pitchVisibility", true}, {"reverseSounds", true}, {"showNoteLength", true}});
	CHECK(view.at("pitchVisibility") == true);
	CHECK(view.at("reverseSounds") == true);
	CHECK(view.at("showNoteLength") == true);
	// The grid type (p. 1227): Use Quantize, or Use Snap from Drum Map.
	CHECK(view.at("gridType") == "quantize");
	CHECK(f.call("editor.set", {{"editor", "window"}, {"gridType", "drumMap"}}).at("gridType") == "drumMap");
	CHECK(f.call("editor.set", {{"editor", "window"}, {"gridType", "zoom"}}).at("gridType") == "zoom"); // Adapt to Zoom
	CHECK_FALSE(f.client.call("editor.set", {{"editor", "window"}, {"gridType", "bar"}}).ok());
	// Not with the preference off; content still opens the drum editor.
	f.call("editor.close", {{"editor", "window"}});
	f.call("settings.set", {{"key", "editing.useDrumEditor"}, {"value", false}});
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}}).at("content") == "key");
	f.call("editor.close", {{"editor", "window"}});
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}, {"content", "drum"}}).at("content") ==
		"drum");
}

TEST_CASE("New Map and New Copy for a track give it the map in one undo step", "[unit]")
{
	Fixture f;
	const Json added = f.call("drumMap.add", {{"copy", f.gm()}, {"track", f.track}}).at("map");
	CHECK(added.at("name") == "GM Map Copy");
	CHECK(f.trackJson().at("drumMap") == added.at("id"));
	f.call("history.undo");
	CHECK(f.maps().size() == 1);
	CHECK(f.trackJson().at("drumMap") == 0);

	const auto audio = f.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	CHECK_THAT(f.fails("drumMap.add", {{"track", audio}}), ContainsSubstring("MIDI and instrument"));
	CHECK_THAT(f.fails("drumMap.add", {{"track", 999}}), ContainsSubstring("no track 999"));
	CHECK(f.maps().size() == 1);
}

TEST_CASE("a GNUbase entry with a note pitch beyond 0-127 or a channel beyond 0-15 doesn't load", "[integration]")
{
	const auto [key, value] = GENERATE(std::make_pair("pitch", 200), std::make_pair("channel", 16));
	INFO(key);
	Fixture f;
	f.call("note.add",
		{{"part", f.part}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 36}, {"velocity", 0.5}}}}});
	const test::TempDir dir;
	const std::string file = (dir.path() / "pitch.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	std::map<std::string, std::string> entries = test::readZip(file);
	Json extension = Json::parse(entries.at("gnubase.json"));
	for (Json& part : extension.at("project").at("midiParts"))
		for (Json& note : part.at("notes"))
			note[key] = value;
	entries["gnubase.json"] = extension.dump();
	test::writeZip(file, entries);
	// The GNUbase entry is damaged, so the project loads from project.xml, which has the note as it was.
	f.call("project.load", {{"path", file}});
	REQUIRE(f.notes().size() == 1);
	CHECK(f.notes()[0].at("pitch") == 36);
	CHECK(f.notes()[0].at("channel") == 0);
}

TEST_CASE("O-Note Conversion gives notes their sounds' O-notes as pitches", "[unit]")
{
	Fixture f;
	const auto id = f.call("drumMap.add", {{"copy", f.gm()}, {"track", f.track}}).at("map").at("id").get<uint64_t>();
	f.call("drumMap.set", {{"map", id}, {"sounds", {{{"pitch", 36}, {"oNote", 24}}, {{"pitch", 38}, {"oNote", 50}}}}});
	const Json added = f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 120}, {"length", 60}, {"pitch", 38}, {"velocity", 0.5}}}}});
	const auto pitches = [&f]
	{
		std::vector<int> result;
		for (const Json& note : f.notes())
			result.push_back(note.at("pitch").get<int>());
		return result;
	};
	// The selected notes, or all of the part's.
	const Json snare = added.at("notes").at(1).at("id");
	f.call("midi.oNoteConversion", {{"parts", {f.part}}, {"notes", {snare}}});
	CHECK(pitches() == std::vector<int>{36, 50});
	f.call("midi.oNoteConversion", {{"parts", {f.part}}});
	CHECK(pitches() == std::vector<int>{24, 50});
	f.call("history.undo");
	CHECK(pitches() == std::vector<int>{36, 50});
	// Notes of a track without a map keep their pitch.
	f.call("track.set", {{"track", f.track}, {"drumMap", 0}});
	f.call("midi.oNoteConversion", {{"parts", {f.part}}});
	CHECK(pitches() == std::vector<int>{36, 50});
}

namespace
{
	// The notes an instrument played: each its status byte and pitch.
	std::vector<std::vector<int>> notesPlayed(const Json& capture)
	{
		std::vector<std::vector<int>> notes;
		for (const Json& captured : capture.at("captured"))
		{
			const Json& bytes = captured.at("bytes");
			const int status = bytes.at(0).get<int>();
			if ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80)
				notes.push_back({status, bytes.at(1).get<int>()});
		}
		return notes;
	}
}

TEST_CASE("notes played live into a track with a drum map go through its I-notes and O-notes", "[integration]")
{
	// MIDI thru (reference manual pp. 1239-1240): an instrument track playing the note probe that monitors.
	engine::Engine engine(audio::makeBackends(), {}, std::vector<std::unique_ptr<midi::Backend>>());
	engine::Client client(engine);
	const auto run = [&client](const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	run("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	const Json added = run("track.add", {{"type", "instrument"}});
	const Json track = added.at("track").at("id");
	const Json channel = added.at("channel").at("id");
	run("channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"monitor", true}});
	const Json gm = run("project.get").at("drumMaps").at(0).at("id");
	const auto id = run("drumMap.add", {{"copy", gm}, {"track", track}}).at("map").at("id");
	// The bass drum (C1, 36) comes in from A1 (45) and plays B0 (35) on channel 4; the low tom gives 45 up.
	run("drumMap.set",
		{{"map", id},
			{"sounds",
				{{{"pitch", 36}, {"iNote", 45}, {"oNote", 35}, {"channel", 3}}, {{"pitch", 45}, {"iNote", 100}},
					{{"pitch", 38}, {"mute", true}}, {{"pitch", 40}, {"channel", nullptr}}}}});
	run("audio.render", {{"frames", 64}}); // the new graph
	run("midi.capture", {{"start", true}});
	const auto play = [&](int status, int pitch, bool sounds = false)
	{
		run("midi.inject",
			{{"channel", channel}, {"drumSounds", sounds},
				{"messages",
					{{{"position", 0}, {"bytes", {status, pitch, 100}}},
						{{"position", 0}, {"bytes", {status - 0x10, pitch, 0}}}}}});
		run("audio.render", {{"frames", 64}});
		return notesPlayed(run("midi.capture"));
	};
	CHECK(play(0x90, 45) == std::vector<std::vector<int>>{{0x93, 35}, {0x83, 35}});
	CHECK(play(0x90, 38).empty());                                                  // muted
	CHECK(play(0x91, 40) == std::vector<std::vector<int>>{{0x91, 40}, {0x81, 40}}); // Any: the note's channel
	CHECK(play(0x90, 42) == std::vector<std::vector<int>>{{0x99, 42}, {0x89, 42}}); // the GM Map's channel 10
	// The drum editor auditions a sound itself (p. 1236): with drumSounds the key is the sound's pitch, not an
	// I-note, so the low tom plays although its I-note is gone, and the low mid tom although the bass drum has its.
	run("drumMap.set", {{"map", id}, {"sounds", Json::array({{{"pitch", 47}, {"iNote", 45}}})}});
	run("audio.render", {{"frames", 64}});
	CHECK(play(0x90, 45) == std::vector<std::vector<int>>{{0x93, 35}, {0x83, 35}}); // the first sound with it
	CHECK(play(0x90, 45, true) == std::vector<std::vector<int>>{{0x99, 45}, {0x89, 45}});
	CHECK(play(0x90, 47, true) == std::vector<std::vector<int>>{{0x99, 47}, {0x89, 47}});
	CHECK(play(0x90, 36, true) == std::vector<std::vector<int>>{{0x93, 35}, {0x83, 35}});
	CHECK(play(0x90, 38, true).empty()); // muted
	// An insert above the divider that plays notes of its own keeps them the sounds themselves: the octave of the
	// low tom, not the bass drum whose I-note it is.
	run("channel.set",
		{{"channel", channel},
			{"midiInserts",
				{{"divider", 1}, {"slots", {{{"effect", "chordGenerator"}, {"parameters", {{"chord", "Octave"}}}}}}}}});
	run("audio.render", {{"frames", 64}});
	CHECK(play(0x90, 45, true) == std::vector<std::vector<int>>{{0x99, 45}, {0x99, 57}, {0x89, 45}, {0x89, 57}});
	// So does the arpeggiator, with the note-off in a later block: a key released in the block it was pressed in
	// plays no step.
	run("channel.set",
		{{"channel", channel}, {"midiInserts", {{"divider", 1}, {"slots", {{{"effect", "arpeggiator"}}}}}}});
	run("audio.render", {{"frames", 64}});
	const auto inject = [&](int status)
	{
		run("midi.inject",
			{{"channel", channel}, {"drumSounds", true},
				{"messages", {{{"position", 0}, {"bytes", {status, 45, status >= 0x90 ? 100 : 0}}}}}});
		run("audio.render", {{"frames", 64}});
	};
	inject(0x90);
	inject(0x80);
	CHECK(notesPlayed(run("midi.capture")) == std::vector<std::vector<int>>{{0x99, 45}, {0x89, 45}});
	run("channel.set", {{"channel", channel}, {"midiInserts", {{"slots", {{{"effect", ""}}}}}}});
	run("audio.render", {{"frames", 64}});
	// Without the map they play as they come in.
	run("track.set", {{"track", track}, {"drumMap", 0}});
	run("audio.render", {{"frames", 64}});
	CHECK(play(0x90, 45) == std::vector<std::vector<int>>{{0x90, 45}, {0x80, 45}});
}

TEST_CASE("notes injected direct reach the instrument as they are, past the drum map, modifiers and MIDI effects",
	"[integration]")
{
	// The drum machine panel plays a pad itself at velocity 127 (reference manual p. 942, #1614): neither the track's
	// drum map nor its MIDI modifiers nor its MIDI inserts change the note.
	engine::Engine engine(audio::makeBackends(), {}, std::vector<std::unique_ptr<midi::Backend>>());
	engine::Client client(engine);
	const auto run = [&client](const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	run("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	const Json added = run("track.add", {{"type", "instrument"}});
	const Json track = added.at("track").at("id");
	const Json channel = added.at("channel").at("id");
	run("channel.set",
		{{"channel", channel}, {"instrument", "noteProbe"}, {"modifiers", {{"transpose", 12}, {"velocityShift", -40}}},
			{"midiInserts", {{"slots", {{{"effect", "chordGenerator"}, {"parameters", {{"chord", "Octave"}}}}}}}}});
	const Json gm = run("project.get").at("drumMaps").at(0).at("id");
	const auto map = run("drumMap.add", {{"copy", gm}, {"track", track}}).at("map").at("id");
	run("drumMap.set", {{"map", map}, {"sounds", {{{"pitch", 38}, {"oNote", 40}, {"channel", 3}}}}});
	run("audio.render", {{"frames", 64}}); // the new graph
	run("midi.capture", {{"start", true}});
	const auto play = [&](bool direct)
	{
		run("midi.inject",
			{{"channel", channel}, {"drumSounds", true}, {"direct", direct},
				{"messages",
					{{{"position", 0}, {"bytes", {0x90, 38, 127}}}, {{"position", 0}, {"bytes", {0x80, 38, 0}}}}}});
		run("audio.render", {{"frames", 64}});
		std::vector<std::vector<int>> notes;
		const Json capture = run("midi.capture");
		for (const Json& captured : capture.at("captured"))
			if (const auto bytes = captured.at("bytes").get<std::vector<int>>(); (bytes.at(0) & 0xE0) == 0x80)
				notes.push_back(bytes);
		return notes;
	};
	// As a drum sound, the snare plays its O-note on its channel, transposed, softer, and with its octave.
	CHECK(play(false) == std::vector<std::vector<int>>{{0x93, 52, 87}, {0x93, 64, 87}, {0x83, 52, 0}, {0x83, 64, 0}});
	CHECK(play(true) == std::vector<std::vector<int>>{{0x90, 38, 127}, {0x80, 38, 0}});
}

TEST_CASE("the notes played in map to the first sound by pitch whose I-note they are", "[unit]")
{
	// The render snapshot maps MIDI thru with inputPitches, recording and step input with inputPitch (p. 1239): two
	// sounds sharing an I-note must give it to the same, the first by pitch, in both.
	engine::Project project;
	engine::DrumMap map = engine::midi::gmDrumMap();
	map.id = 5;
	map.sounds.at(60).iNote = 50;
	map.sounds.at(40).iNote = 50;
	map.sounds.at(50).iNote = 40;
	project.drumMaps[map.id] = map;
	engine::Track track;
	track.id = 7;
	track.drumMap = map.id;
	project.trackById[track.id] = track;
	const auto pitches = engine::midi::inputPitches(map);
	for (size_t played = 0; played < pitches.size(); ++played)
	{
		INFO(played);
		CHECK(pitches.at(played) == engine::midi::inputPitch(project, track.id, static_cast<uint8_t>(played)));
	}
	CHECK(pitches.at(50) == 40);
	CHECK(pitches.at(40) == 50);
}

TEST_CASE("Solo Instrument plays only the sounds of the drum editor's selected notes", "[unit]")
{
	Fixture f;
	const Json added = f.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", 60}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 120}, {"length", 60}, {"pitch", 38}, {"velocity", 0.5}},
					{{"position", 240}, {"length", 60}, {"pitch", 42}, {"velocity", 0.5}},
					{{"position", 360}, {"length", 60}, {"pitch", 38}, {"velocity", 0.5}}}}});
	const auto noteId = [&](size_t index) { return added.at("notes").at(index).at("id").get<uint64_t>(); };
	const auto pitches = [&]
	{
		std::vector<int> played;
		for (const engine::RenderNote& note : f.rendered())
			played.push_back(note.pitch);
		return played;
	};
	const auto revision = [&] { return f.call("project.revision"); };
	f.call("track.set", {{"track", f.track}, {"drumMap", f.gm()}});
	f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}, {"content", "drum"}});
	f.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", noteId(1)}}}}});
	CHECK(pitches() == std::vector<int>{36, 38, 42, 38}); // Solo Instrument is off

	const bool modified = revision().at("modified").get<bool>();
	const Json view = f.call("editor.set", {{"editor", "window"}, {"soloInstrument", true}});
	CHECK(view.at("soloInstrument") == true);
	CHECK(pitches() == std::vector<int>{38, 38}); // every note of the selected note's sound
	CHECK(revision().at("modified") == modified); // what plays changed, not the project

	f.call("editor.set",
		{{"editor", "window"},
			{"selection", {{{"part", f.part}, {"note", noteId(0)}}, {{"part", f.part}, {"note", noteId(2)}}}}});
	CHECK(pitches() == std::vector<int>{36, 42});
	// A selected note moved to another sound solos that one.
	f.call("note.set", {{"part", f.part}, {"notes", {{{"id", noteId(2)}, {"pitch", 38}}}}});
	CHECK(pitches() == std::vector<int>{36, 38, 38, 38});

	SECTION("nothing selected plays every sound")
	{
		f.call("editor.set", {{"editor", "window"}, {"selection", Json::array()}});
		CHECK(pitches().size() == 4);
	}
	SECTION("only the drum editor solos")
	{
		f.call("editor.set", {{"editor", "window"}, {"content", "key"}});
		CHECK(pitches().size() == 4);
	}
	SECTION("a closed editor solos nothing")
	{
		f.call("editor.close", {{"editor", "window"}});
		CHECK(pitches().size() == 4);
	}
	SECTION("it needs a drum map")
	{
		f.call("track.set", {{"track", f.track}, {"drumMap", 0}});
		CHECK(pitches().size() == 4);
	}
	SECTION("turned off")
	{
		f.call("editor.set", {{"editor", "window"}, {"soloInstrument", false}});
		CHECK(pitches().size() == 4);
	}
}

TEST_CASE("Pitch Visibility's Show Drum Sounds with Events and a track's GM name list", "[unit]")
{
	Fixture f;
	f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}, {"content", "drum"}});
	CHECK(f.call("editor.get", {{"editor", "window"}}).at("visibleSounds") == "all");
	CHECK(f.call("editor.set", {{"editor", "window"}, {"visibleSounds", "withEvents"}}).at("visibleSounds") ==
		"withEvents");
	CHECK_FALSE(f.client.call("editor.set", {{"editor", "window"}, {"visibleSounds", "used"}}).ok());

	// GM Default names the pitches without a drum map (p. 1239); saved with the project, undone like the map.
	CHECK(f.trackJson().at("gmNames") == false);
	f.call("track.set", {{"track", f.track}, {"gmNames", true}});
	CHECK(f.trackJson().at("gmNames") == true);
	const test::TempDir dir;
	const std::string file = (dir.path() / "names.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	f.track = f.call("project.get").at("tracks").at(0).at("id").get<uint64_t>();
	CHECK(f.trackJson().at("gmNames") == true);
	f.call("track.set", {{"track", f.track}, {"gmNames", false}});
	f.call("history.undo");
	CHECK(f.trackJson().at("gmNames") == true);
	const auto audio = f.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	CHECK_THAT(f.fails("track.set", {{"track", audio}, {"gmNames", true}}), ContainsSubstring("name list"));
}

TEST_CASE("Show Drum Sounds in Use by Instrument has the sounds whose pads the drum machine plays", "[unit]")
{
	Fixture f;
	const auto open = [&](uint64_t track)
	{
		const auto part = f.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
		return f.call("editor.open", {{"editor", "window"}, {"parts", {part}}, {"content", "drum"}});
	};
	// p. 1234: only an instrument that can tell; the test's instrument track has no drum machine.
	CHECK(open(f.track).at("soundsInUse").is_null());
	CHECK(f.call("editor.set", {{"editor", "window"}, {"visibleSounds", "inUse"}}).at("visibleSounds") == "inUse");

	// A drum track's kit (drum.create).
	const Json drums = f.call("drum.create");
	const std::vector<int> kit = {36, 38, 39, 42, 44, 45, 46, 48, 49, 50, 56};
	CHECK(open(drums.at("track").at("id").get<uint64_t>()).at("soundsInUse").get<std::vector<int>>() == kit);

	// A MIDI track playing to it, through its drum map's O-notes.
	const Json midi = f.call("track.add", {{"type", "midi"}}).at("track");
	f.call("channel.set", {{"channel", midi.at("channel")}, {"output", drums.at("channel").at("id")}});
	const auto map = f.call("drumMap.add", {{"copy", f.gm()}}).at("map").at("id").get<uint64_t>();
	f.call("drumMap.set", {{"map", map}, {"sounds", {{{"pitch", 40}, {"oNote", 36}}, {{"pitch", 36}, {"oNote", 0}}}}});
	f.call("track.set", {{"track", midi.at("id")}, {"drumMap", map}});
	const auto inUse = open(midi.at("id").get<uint64_t>()).at("soundsInUse").get<std::vector<int>>();
	CHECK(std::find(inUse.begin(), inUse.end(), 40) != inUse.end());
	CHECK(std::find(inUse.begin(), inUse.end(), 36) == inUse.end());
	CHECK(std::find(inUse.begin(), inUse.end(), 38) != inUse.end());
}
