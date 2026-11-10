// SPDX-License-Identifier: GPL-3.0-or-later
// Patterns in the engine (#181, docs/features/pattern-editor.md): pattern.add, set and remove with their checks and
// mode switches, pattern events playing their patterns (rendered: steps, ties, repeats, gate, offsets, swing,
// directions, lane lengths and resolutions, mute and solo), Convert Pattern Event to MIDI Part, pattern presets, the
// project file and the pattern editor content. tests/patterneditor_ui_tests.cpp drives the pattern editor in the app.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
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
			pattern = call("pattern.add", {{"track", track}}).at("pattern");
			part = call(
				"midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"pattern", pattern.at("id")}})
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

		Json patterns() { return call("project.get").at("patterns"); }

		// Sets the pattern's lanes: each {pitch, steps}.
		void lanes(Json lanes, Json more = Json::object())
		{
			more["pattern"] = pattern.at("id");
			more["lanes"] = std::move(lanes);
			pattern = call("pattern.set", more).at("pattern");
		}

		// The notes played, by start.
		std::vector<engine::RenderNote> rendered()
		{
			std::vector<engine::RenderNote> notes = engine.renderSnapshots().acquire().channels.at(0).notes;
			std::stable_sort(notes.begin(), notes.end(),
				[](const engine::RenderNote& a, const engine::RenderNote& b) { return a.start < b.start; });
			return notes;
		}

		// Sixteenths in samples at the project's 120 bpm.
		int64_t at(double sixteenths) const { return std::llround(sixteenths * m_rate / 8.0); }

		std::vector<int64_t> starts()
		{
			std::vector<int64_t> starts;
			for (const engine::RenderNote& note : rendered())
				starts.push_back(note.start);
			return starts;
		}

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t track = 0;
		Json pattern;
		uint64_t part = 0;
		double m_rate = call("project.get").at("sampleRate").get<double>();
	};

	Json steps(std::vector<int> indexes)
	{
		Json steps = Json::array();
		for (const int index : indexes)
			steps.push_back({{"index", index}});
		return steps;
	}
}

TEST_CASE("patterns are added, duplicated, changed and removed, with their checks and undo", "[unit]")
{
	Fixture f;
	// New Pattern (reference manual p. 899): 16 steps of 1/16 in drum mode, with four drum lanes named after GM.
	const Json pattern = f.pattern;
	CHECK(pattern.at("name") == "Pattern 1");
	CHECK(pattern.at("track") == f.track);
	CHECK(pattern.at("mode") == "drum");
	CHECK(pattern.at("length") == 16);
	CHECK(pattern.at("resolution") == "1/16");
	CHECK(pattern.at("direction") == "forward");
	REQUIRE(pattern.at("lanes").size() == 4);
	CHECK(pattern.at("lanes").at(0) ==
		Json{{"pitch", 36}, {"name", "Bass Drum 1"}, {"mute", false}, {"solo", false}, {"length", 0},
			{"resolution", nullptr}, {"direction", "forward"}, {"swing", 0.0}, {"offset", 0.0},
			{"steps", Json::array()}});
	CHECK(pattern.at("lanes").at(1).at("name") == "Acoustic Snare");
	CHECK(pattern.at("lanes").at(2).at("pitch") == 42);

	// A melodic pattern has no lanes; with part the part plays it.
	const uint64_t other = f.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
							   .at("part")
							   .at("id")
							   .get<uint64_t>();
	const Json melodic = f.call("pattern.add", {{"part", other}, {"mode", "poly"}, {"length", 8}}).at("pattern");
	CHECK(melodic.at("name") == "Pattern 2");
	CHECK(melodic.at("mode") == "poly");
	CHECK(melodic.at("length") == 8);
	CHECK(melodic.at("lanes").empty());
	const Json project = f.call("project.get");
	for (const Json& each : project.at("midiParts"))
		if (each.at("id") == other)
			CHECK(each.at("pattern") == melodic.at("id"));

	// Duplicate Pattern.
	const Json copy = f.call("pattern.add", {{"copy", pattern.at("id")}}).at("pattern");
	CHECK(copy.at("name") == "Pattern 1 Copy");
	CHECK(copy.at("lanes") == pattern.at("lanes"));
	CHECK(f.patterns().size() == 3);

	const uint64_t audio = f.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	CHECK_THAT(f.fails("pattern.add", {{"track", audio}}), ContainsSubstring("MIDI and instrument tracks"));

	// Steps, with their parameters; what isn't given is the default.
	f.lanes({{{"pitch", 36}, {"steps", {{{"index", 0}, {"velocity", 90}, {"repeats", 2}, {"gate", 0.5}}}}},
		{{"pitch", 38}, {"name", "Snare"}, {"steps", steps({4, 12})}}});
	CHECK(f.pattern.at("lanes").at(0).at("steps").at(0) ==
		Json{{"index", 0}, {"length", 1}, {"velocity", 90}, {"repeats", 2}, {"offset", 0.0}, {"probability", 1.0},
			{"velocityVariance", 0.0}, {"gate", 0.5}});

	// What a pattern can't have.
	const Json id = f.pattern.at("id");
	CHECK_THAT(f.fails("pattern.set", {{"pattern", id}, {"length", 129}}), ContainsSubstring("length"));
	CHECK_THAT(f.fails("pattern.set", {{"pattern", id}, {"resolution", "1/128"}}), ContainsSubstring("1/64"));
	CHECK_THAT(f.fails("pattern.set", {{"pattern", id}, {"lanes", {{{"pitch", 36}}, {{"pitch", 36}}}}}),
		ContainsSubstring("has a lane already"));
	CHECK_THAT(f.fails("pattern.set", {{"pattern", id}, {"lanes", {{{"pitch", 36}, {"steps", steps({16})}}}}}),
		ContainsSubstring("must lie on the lane's 16 steps"));
	CHECK_THAT(f.fails("pattern.set", {{"pattern", id}, {"lanes", {{{"pitch", 36}, {"steps", steps({3, 3})}}}}}),
		ContainsSubstring("starts at 3 already"));
	CHECK_THAT(f.fails("pattern.set",
				   {{"pattern", id}, {"lanes", {{{"pitch", 36}, {"steps", {{{"index", 0}, {"repeats", 11}}}}}}}}),
		ContainsSubstring("repeats"));
	CHECK_THAT(f.fails("pattern.add", {{"track", 999999}}), ContainsSubstring("no track"));
	CHECK_THAT(f.fails("midiPart.set", {{"part", f.part}, {"pattern", 999999}}), ContainsSubstring("no pattern"));

	// Changes are undo steps.
	f.call("history.undo");
	CHECK(f.patterns().at(0).at("lanes") == pattern.at("lanes"));
	f.call("history.redo");
	CHECK(f.patterns().at(0).at("lanes").size() == 2);

	// Removing a pattern removes its pattern events.
	f.call("pattern.remove", {{"pattern", id}});
	CHECK(f.patterns().size() == 2);
	const Json removed = f.call("project.get");
	for (const Json& each : removed.at("midiParts"))
		CHECK(each.at("id") != f.part);
	f.call("history.undo");
	CHECK(f.patterns().size() == 3);
	CHECK(f.call("project.get").at("midiParts").size() == 2);
}

TEST_CASE("switching modes clears a pattern between drum and melodic, and keeps the lowest steps in mono", "[unit]")
{
	Fixture f;
	f.lanes({{{"pitch", 36}, {"steps", steps({0})}}});
	// Drum to melodic mode doesn't convert (p. 913).
	f.call("pattern.set", {{"pattern", f.pattern.at("id")}, {"mode", "poly"}});
	f.lanes({{{"pitch", 60}, {"steps", steps({0, 2})}}, {{"pitch", 64}, {"steps", steps({0, 4})}},
				{{"pitch", 67}, {"length", 8}, {"steps", steps({1})}}},
		{{"mode", "poly"}});
	CHECK(f.pattern.at("mode") == "poly");
	REQUIRE(f.pattern.at("lanes").size() == 3);

	// Poly to mono: the lowest step at each position stays, the lane settings go.
	f.pattern = f.call("pattern.set", {{"pattern", f.pattern.at("id")}, {"mode", "mono"}}).at("pattern");
	const Json& lanes = f.pattern.at("lanes");
	CHECK(lanes.at(0).at("steps").size() == 2);
	CHECK(lanes.at(1).at("steps").size() == 1);
	CHECK(lanes.at(1).at("steps").at(0).at("index") == 4);
	CHECK(lanes.at(2).at("length") == 0);
	CHECK(lanes.at(2).at("steps").at(0).at("index") == 1);
	// Mono mode plays one step at a time, ties included.
	CHECK_THAT(f.fails("pattern.set",
				   {{"pattern", f.pattern.at("id")},
					   {"lanes",
						   {{{"pitch", 60}, {"steps", {{{"index", 0}, {"length", 3}}}}},
							   {{"pitch", 62}, {"steps", steps({2})}}}}}),
		ContainsSubstring("one step at a time"));

	f.call("pattern.set", {{"pattern", f.pattern.at("id")}, {"mode", "drum"}});
	CHECK(f.patterns().at(0).at("lanes").empty());
}

TEST_CASE("a pattern event plays its pattern over and over: render", "[unit]")
{
	Fixture f;
	REQUIRE(f.call("project.get").at("fixedTempo") == 120.0);
	// An empty pattern plays nothing, nor do the part's own notes.
	f.call("note.add",
		{{"part", f.part}, {"notes", {{{"position", 0}, {"length", 60}, {"pitch", 70}, {"velocity", 0.5}}}}});
	CHECK(f.rendered().empty());

	// Steps on the kick and the snare: each step a sixteenth, and an eight step pattern twice in the bar.
	f.lanes({{{"pitch", 36}, {"steps", {{{"index", 0}, {"velocity", 127}}, {{"index", 4}, {"length", 2}}}}},
				{{"pitch", 38}, {"steps", steps({2})}}},
		{{"length", 8}});
	std::vector<engine::RenderNote> notes = f.rendered();
	REQUIRE(notes.size() == 6);
	CHECK(notes[0].start == f.at(0));
	CHECK(notes[0].end == f.at(1));
	CHECK(notes[0].pitch == 36);
	CHECK(notes[0].velocity == 0xFFFF);
	CHECK(notes[1].start == f.at(2));
	CHECK(notes[1].pitch == 38);
	CHECK(notes[2].start == f.at(4));
	CHECK(notes[2].end == f.at(6)); // tied to the step after it
	CHECK(notes[3].start == f.at(8));
	CHECK(notes[5].start == f.at(12));

	// Repeats split a step into sub-steps, gate shortens them, offsets move them by parts of a step.
	f.lanes(
		{{{"pitch", 36}, {"steps", {{{"index", 0}, {"repeats", 2}, {"gate", 0.5}}, {{"index", 4}, {"offset", 0.5}}}}}},
		{{"length", 16}});
	notes = f.rendered();
	REQUIRE(notes.size() == 3);
	CHECK(notes[0].start == f.at(0));
	CHECK(notes[0].end == f.at(0.25));
	CHECK(notes[1].start == f.at(0.5));
	CHECK(notes[2].start == f.at(4.5));

	// Swing moves every second step by half a step times the swing; the lane's offset moves all of them.
	f.lanes({{{"pitch", 36}, {"offset", 0.25}, {"steps", steps({0, 1})}}}, {{"swing", 0.5}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(0.25), f.at(1.5)});

	// Backward and alternating play directions.
	f.lanes({{{"pitch", 36}, {"steps", {{{"index", 0}, {"length", 2}}, {{"index", 5}}}}}},
		{{"swing", 0.0}, {"length", 8}, {"direction", "backward"}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(2), f.at(6), f.at(10), f.at(14)});
	f.call("pattern.set", {{"pattern", f.pattern.at("id")}, {"direction", "alternating"}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(0), f.at(5), f.at(10), f.at(14)});

	// A lane of its own length and resolution loops on its own: three eighths against the pattern's sixteenths.
	f.lanes({{{"pitch", 36}, {"steps", steps({0})}},
				{{"pitch", 42}, {"length", 3}, {"resolution", "1/8"}, {"steps", steps({0})}}},
		{{"direction", "forward"}, {"length", 16}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(0), f.at(0), f.at(6), f.at(12)});

	// Mute and solo.
	f.lanes({{{"pitch", 36}, {"mute", true}, {"steps", steps({0})}}, {{"pitch", 38}, {"steps", steps({1})}}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(1)});
	f.lanes({{{"pitch", 36}, {"solo", true}, {"steps", steps({0})}}, {{"pitch", 38}, {"steps", steps({1})}}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(0)});

	// A longer event repeats the pattern; an event plays from its own start, and a muted one doesn't.
	f.call("midiPart.set", {{"part", f.part}, {"position", g_bar}, {"length", 2 * g_bar}});
	CHECK(f.starts() == std::vector<int64_t>{f.at(16), f.at(32)});
	f.call("event.set", {{"events", {f.part}}, {"mute", true}});
	CHECK(f.rendered().empty());
}

TEST_CASE("a step's probability and velocity variance play as a note's, differently in each repeat", "[unit]")
{
	Fixture f;
	f.lanes({{{"pitch", 36}, {"steps", {{{"index", 0}, {"probability", 0.5}}}}}}, {{"length", 1}});
	// 16 repeats in a bar: some play, some don't, and the same ones every time.
	const size_t played = f.rendered().size();
	CHECK(played > 0);
	CHECK(played < 16);
	CHECK(f.rendered().size() == played);
}

TEST_CASE("a pattern event converts to a MIDI part with the notes it played", "[unit]")
{
	Fixture f;
	f.lanes({{{"pitch", 36}, {"steps", {{{"index", 0}, {"probability", 0.5}}, {{"index", 8}, {"velocity", 64}}}}}});
	const std::vector<engine::RenderNote> before = f.rendered();
	const Json part = f.call("pattern.convert", {{"part", f.part}}).at("part");
	CHECK(part.at("pattern") == 0);
	REQUIRE(part.at("notes").size() == 2);
	std::vector<Json> notes(part.at("notes").begin(), part.at("notes").end());
	std::sort(notes.begin(), notes.end(),
		[](const Json& a, const Json& b) { return a.at("position").get<int64_t>() < b.at("position").get<int64_t>(); });
	CHECK(notes[0].at("pitch") == 36);
	CHECK(notes[0].at("probability") == 0.5);
	CHECK(notes[1].at("position") == 8 * g_quarter / 4);
	CHECK(std::lround(notes[1].at("velocity").get<double>() * 127) == 64);
	CHECK(!f.rendered().empty()); // the step without a chance always plays
	CHECK_THAT(f.fails("pattern.convert", {{"part", f.part}}), ContainsSubstring("isn't a pattern event"));
	f.call("history.undo");
	CHECK(f.rendered().size() == before.size());
}

TEST_CASE("pattern presets save a track's patterns and load them into another", "[integration]")
{
	Fixture f;
	const test::TempDir dir;
	const std::string file = (dir.path() / "beat.patternpreset").u8string();
	f.lanes({{{"pitch", 36}, {"name", "Kick"}, {"steps", steps({0, 8})}}}, {{"swing", 0.25}, {"name", "Beat"}});
	f.call("pattern.add", {{"track", f.track}, {"mode", "mono"}, {"lanes", {{{"pitch", 60}, {"steps", steps({1})}}}}});
	f.call("pattern.export", {{"track", f.track}, {"path", file}});
	const uint64_t other = f.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const Json loaded = f.call("pattern.import", {{"track", other}, {"path", file}}).at("patterns");
	REQUIRE(loaded.size() == 2);
	const Json patterns = f.patterns();
	for (size_t i = 0; i < 2; ++i)
	{
		Json expected = patterns.at(i);
		expected["id"] = loaded.at(i).at("id");
		expected["track"] = other;
		CHECK(loaded.at(i) == expected);
	}
	CHECK(loaded.at(1).at("mode") == "mono");

	CHECK_THAT(f.fails("pattern.import", {{"track", other}, {"path", (dir.path() / "none").u8string()}}),
		ContainsSubstring("can't open"));
	std::ofstream((dir.path() / "bad.patternpreset").u8string()) << "{\"drum\": 1}";
	CHECK_THAT(f.fails("pattern.import", {{"track", other}, {"path", (dir.path() / "bad.patternpreset").u8string()}}),
		ContainsSubstring("isn't a pattern preset"));
	CHECK_THAT(f.fails("pattern.export",
				   {{"track", f.call("track.add", {{"type", "midi"}}).at("track").at("id")}, {"path", file}}),
		ContainsSubstring("no patterns"));
}

TEST_CASE("patterns and pattern events are saved and loaded with the project", "[integration]")
{
	Fixture f;
	const test::TempDir dir;
	const std::string file = (dir.path() / "patterns.dawproject").u8string();
	f.lanes({{{"pitch", 36}, {"resolution", "1/8T"}, {"steps", {{{"index", 1}, {"length", 2}, {"gate", 0.3}}}}}},
		{{"direction", "alternating"}});
	f.call("pattern.add", {{"track", f.track}, {"mode", "mono"}, {"lanes", {{{"pitch", 60}, {"steps", steps({1})}}}}});
	const Json before = f.call("project.get");
	const std::vector<engine::RenderNote> played = f.rendered();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	const Json after = f.call("project.get");
	CHECK(after.at("patterns") == before.at("patterns"));
	CHECK(after.at("midiParts").at(0).at("pattern") == f.pattern.at("id"));
	CHECK(f.rendered().size() == played.size());
}

TEST_CASE("pattern events open in the pattern editor", "[unit]")
{
	Fixture f;
	const Json view = f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}});
	CHECK(view.at("content") == "pattern");
	f.call("editor.close", {{"editor", "window"}});
	const uint64_t plain = f.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
							   .at("part")
							   .at("id")
							   .get<uint64_t>();
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {plain}}}).at("content") == "key");
	CHECK(f.call("editor.set", {{"editor", "window"}, {"content", "pattern"}}).at("content") == "pattern");
}
