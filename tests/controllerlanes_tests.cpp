// SPDX-License-Identifier: GPL-3.0-or-later
// The controller lanes in the engine (#128, docs/features/controller-lanes.md): play probability and velocity variance
// of notes, how they play and note.apply, controller.line's shapes on note and controller lanes, and the lanes,
// controller selection and Auto Select Controllers of the editor views. tests/controllerlanes_ui_tests.cpp drives the
// lanes in the app.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_sixteenth = g_quarter / 4;
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

		// Adds notes a sixteenth long at the positions, pitch 60 + their index, velocity 0.5; returns their ids.
		std::vector<uint64_t> notes(const std::vector<int64_t>& positions, const Json& extra = Json::object())
		{
			Json items = Json::array();
			for (size_t i = 0; i < positions.size(); ++i)
			{
				Json item = {{"position", positions[i]}, {"length", g_sixteenth}, {"pitch", 60 + static_cast<int>(i)},
					{"velocity", 0.5}};
				item.update(extra);
				items.push_back(item);
			}
			std::vector<uint64_t> ids;
			const Json added = call("note.add", {{"part", part}, {"notes", items}});
			for (const Json& note : added.at("notes"))
				ids.push_back(note.at("id").get<uint64_t>());
			return ids;
		}

		Json midiPart()
		{
			const Json project = call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
					return each;
			return nullptr;
		}

		Json note(uint64_t id)
		{
			const Json found = midiPart();
			for (const Json& each : found.at("notes"))
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		// The part's controller events by position.
		std::vector<Json> controllers()
		{
			const Json found = midiPart().at("controllers");
			std::vector<Json> sorted(found.begin(), found.end());
			std::sort(sorted.begin(), sorted.end(),
				[](const Json& a, const Json& b) { return a.at("position") < b.at("position"); });
			return sorted;
		}

		std::vector<engine::RenderNote> rendered() { return engine.renderSnapshots().acquire().channels.at(0).notes; }

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t track = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("notes have a play probability and a velocity variance", "[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({0, g_quarter});
	// By default every note plays at its velocity: neither field is listed.
	CHECK_FALSE(f.note(ids[0]).contains("probability"));
	CHECK_FALSE(f.note(ids[0]).contains("velocityVariance"));
	f.call("note.set",
		{{"part", f.part}, {"notes", {{{"id", ids[0]}, {"probability", 0.25}, {"velocityVariance", -0.5}}}}});
	CHECK(f.note(ids[0]).at("probability").get<double>() == Approx(0.25));
	CHECK(f.note(ids[0]).at("velocityVariance").get<double>() == Approx(-0.5));
	const std::vector<uint64_t> added = f.notes({g_bar / 2}, {{"probability", 0.5}, {"velocityVariance", 1.0}});
	CHECK(f.note(added[0]).at("probability").get<double>() == Approx(0.5));
	CHECK(f.note(added[0]).at("velocityVariance").get<double>() == Approx(1.0));

	CHECK_THAT(f.fails("note.set", {{"part", f.part}, {"notes", {{{"id", ids[1]}, {"probability", 1.5}}}}}),
		ContainsSubstring("probability must be 0 to 1"));
	CHECK_THAT(f.fails("note.set", {{"part", f.part}, {"notes", {{{"id", ids[1]}, {"velocityVariance", -2}}}}}),
		ContainsSubstring("velocityVariance must be -1 to 1"));
	CHECK_THAT(
		f.fails("note.add",
			{{"part", f.part},
				{"notes", {{{"position", 0}, {"length", 1}, {"pitch", 1}, {"velocity", 1}, {"probability", -1}}}}}),
		ContainsSubstring("probability must be 0 to 1"));
	// Each is one undo step.
	f.call("history.undo");
	CHECK(f.note(added[0]).is_null());
	f.call("history.undo");
	CHECK_FALSE(f.note(ids[0]).contains("probability"));
}

TEST_CASE("play probability and velocity variance are saved with the project", "[integration]")
{
	const test::TempDir dir;
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({0}, {{"probability", 0.3}, {"velocityVariance", 0.4}});
	const std::string file = (dir.path() / "song.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.load", {{"path", file}});
	const Json note = f.midiPart().at("notes").at(0);
	CHECK(note.at("probability").get<double>() == Approx(0.3));
	CHECK(note.at("velocityVariance").get<double>() == Approx(0.4));
	(void)ids;
}

TEST_CASE("playback leaves out notes by their probability and varies velocities, the same on every playback", "[unit]")
{
	Fixture f;
	std::vector<int64_t> positions;
	positions.reserve(64);
	for (int i = 0; i < 64; ++i)
		positions.push_back(i * (g_bar / 64));
	const std::vector<uint64_t> never = f.notes({0}, {{"probability", 0.0}});
	const std::vector<uint64_t> always = f.notes({g_sixteenth}, {{"probability", 1.0}});
	REQUIRE(f.rendered().size() == 1); // the note of probability 0 never plays
	CHECK(f.rendered()[0].pitch == 60);

	// Of many notes of probability 0.5, about half play; the same ones every time the snapshot is made.
	const std::vector<uint64_t> half = f.notes(positions, {{"probability", 0.5}});
	const size_t played = f.rendered().size() - 1;
	CHECK(played > 16);
	CHECK(played < 48);
	const std::vector<engine::RenderNote> first = f.rendered();
	f.call("track.set", {{"track", f.track}, {"name", "again"}}); // a new snapshot
	const std::vector<engine::RenderNote> second = f.rendered();
	REQUIRE(second.size() == first.size());
	for (size_t i = 0; i < first.size(); ++i)
		CHECK(second[i].start == first[i].start);

	// A positive variance raises velocities by up to that share, a negative one lowers them.
	Json raise = Json::array();
	for (const uint64_t id : half)
		raise.push_back({{"id", id}, {"probability", 1.0}, {"velocityVariance", 0.5}});
	f.call("note.set", {{"part", f.part}, {"notes", raise}});
	const auto base = static_cast<double>(std::lround(0.5 * 65535.0));
	bool varied = false;
	for (const engine::RenderNote& note : f.rendered())
	{
		CHECK(note.velocity >= base - 1.0);
		CHECK(note.velocity <= (base * 1.5) + 1.0);
		varied = varied || std::abs(note.velocity - base) > 2.0;
	}
	CHECK(varied);
	Json lower = Json::array();
	for (const uint64_t id : half)
		lower.push_back({{"id", id}, {"velocityVariance", -1.0}});
	f.call("note.set", {{"part", f.part}, {"notes", lower}});
	for (const engine::RenderNote& note : f.rendered())
		if (note.pitch != 60) // the note without variance
			CHECK(note.velocity <= base + 1.0);
	(void)never;
	(void)always;
}

TEST_CASE("note.apply makes play probability and velocity variance permanent as they play", "[unit]")
{
	Fixture f;
	std::vector<int64_t> positions;
	positions.reserve(32);
	for (int i = 0; i < 32; ++i)
		positions.push_back(i * (g_bar / 32));
	const std::vector<uint64_t> ids = f.notes(positions, {{"probability", 0.5}, {"velocityVariance", 0.5}});
	std::vector<int> playedPitches;
	std::vector<uint16_t> playedVelocities;
	for (const engine::RenderNote& note : f.rendered())
	{
		playedPitches.push_back(note.pitch);
		playedVelocities.push_back(note.velocity);
	}

	// Applying the probability keeps the notes that play and resets their probability.
	Json result = f.call("note.apply", {{"part", f.part}, {"what", "probability"}});
	std::vector<int> kept;
	const Json applied = f.midiPart();
	for (const Json& note : applied.at("notes"))
	{
		kept.push_back(note.at("pitch").get<int>());
		CHECK_FALSE(note.contains("probability"));
	}
	std::sort(kept.begin(), kept.end());
	CHECK(kept == playedPitches);
	CHECK(result.at("removed").size() == ids.size() - kept.size());

	// Applying the variance writes the velocities they play with and resets it; only for the notes named.
	const uint64_t first = f.midiPart().at("notes").at(0).at("id").get<uint64_t>();
	f.call("note.apply", {{"part", f.part}, {"what", "velocityVariance"}, {"notes", {first}}});
	CHECK_FALSE(f.note(first).contains("velocityVariance"));
	const int pitch = f.note(first).at("pitch").get<int>();
	const auto index =
		static_cast<size_t>(std::find(playedPitches.begin(), playedPitches.end(), pitch) - playedPitches.begin());
	CHECK(std::lround(f.note(first).at("velocity").get<double>() * 65535.0) == playedVelocities.at(index));
	const Json after = f.midiPart();
	const auto others = std::count_if(after.at("notes").begin(), after.at("notes").end(),
		[](const Json& note) { return note.contains("velocityVariance"); });
	CHECK(static_cast<size_t>(others) == kept.size() - 1);

	// Each is one undo step.
	f.call("history.undo");
	f.call("history.undo");
	CHECK(f.midiPart().at("notes").size() == ids.size());
	CHECK_THAT(f.fails("note.apply", {{"part", f.part}, {"what", "pitch"}}), ContainsSubstring("what"));
}

TEST_CASE("controller.line draws controller events along a line, a parabola or a periodic curve", "[unit]")
{
	Fixture f;
	// An event of the lane in the range goes, those of other lanes and outside stay.
	f.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", g_quarter}, {"number", 7}, {"value", 0.1}},
					{{"position", g_quarter}, {"number", 1}, {"value", 0.1}},
					{{"position", 3 * g_quarter}, {"number", 7}, {"value", 0.9}}}}});
	const Json lane = {{"type", "controller"}, {"number", 7}};
	Json result = f.call("controller.line",
		{{"part", f.part}, {"lane", lane}, {"mode", "line"}, {"from", {{"position", 0}, {"value", 0.0}}},
			{"to", {{"position", 2 * g_quarter}, {"value", 1.0}}}, {"density", g_quarter / 2}});
	CHECK(result.at("removed").size() == 1);
	const std::vector<Json> events = f.controllers();
	REQUIRE(events.size() == 7); // 0, 1/8, ... 2/4 and the two that stay
	std::vector<std::pair<int64_t, double>> seven;
	for (const Json& event : events)
		if (event.at("number") == 7)
			seven.emplace_back(event.at("position").get<int64_t>(), event.at("value").get<double>());
	REQUIRE(seven.size() == 6);
	for (size_t i = 0; i < 5; ++i)
	{
		CHECK(seven[i].first == static_cast<int64_t>(i) * g_quarter / 2);
		CHECK(seven[i].second == Approx(static_cast<double>(i) / 4.0).margin(1e-6));
	}
	CHECK(seven[5].first == 3 * g_quarter);

	// A parabola rises slowly first, reversed fast first.
	const auto valueAt = [&](int64_t position)
	{
		for (const Json& event : f.controllers())
			if (event.at("number") == 7 && event.at("position") == position)
				return event.at("value").get<double>();
		return -1.0;
	};
	f.call("controller.line",
		{{"part", f.part}, {"lane", lane}, {"mode", "parabola"}, {"from", {{"position", 0}, {"value", 0.0}}},
			{"to", {{"position", 2 * g_quarter}, {"value", 1.0}}}, {"density", g_quarter / 2}});
	CHECK(valueAt(g_quarter) == Approx(0.25).margin(1e-6));
	f.call("controller.line",
		{{"part", f.part}, {"lane", lane}, {"mode", "parabola"}, {"reverse", true},
			{"from", {{"position", 0}, {"value", 0.0}}}, {"to", {{"position", 2 * g_quarter}, {"value", 1.0}}},
			{"density", g_quarter / 2}});
	CHECK(valueAt(g_quarter) == Approx(0.75).margin(1e-6));

	// Periodic curves go from the start value to the end value and back once a period.
	const auto periodic = [&](const char* mode)
	{
		f.call("controller.line",
			{{"part", f.part}, {"lane", lane}, {"mode", mode}, {"from", {{"position", 0}, {"value", 0.2}}},
				{"to", {{"position", 2 * g_quarter}, {"value", 0.8}}}, {"density", g_quarter / 4},
				{"period", g_quarter}});
	};
	periodic("sine");
	CHECK(valueAt(0) == Approx(0.2).margin(1e-6));
	CHECK(valueAt(g_quarter / 2) == Approx(0.8).margin(1e-6));
	CHECK(valueAt(g_quarter / 4) == Approx(0.5).margin(1e-6));
	CHECK(valueAt(g_quarter) == Approx(0.2).margin(1e-6));
	periodic("triangle");
	CHECK(valueAt(g_quarter / 4) == Approx(0.5).margin(1e-6));
	CHECK(valueAt(g_quarter / 2) == Approx(0.8).margin(1e-6));
	CHECK(valueAt(3 * g_quarter / 4) == Approx(0.5).margin(1e-6));
	periodic("square");
	CHECK(valueAt(g_quarter / 4) == Approx(0.2).margin(1e-6));
	CHECK(valueAt(g_quarter / 2) == Approx(0.8).margin(1e-6));

	// Pitch bend and program change lanes: values 0-1, a program 0-127.
	f.call("controller.line",
		{{"part", f.part}, {"lane", {{"type", "program"}, {"channel", 2}}}, {"mode", "line"},
			{"from", {{"position", 0}, {"value", 0.0}}}, {"to", {{"position", g_quarter}, {"value", 1.0}}},
			{"density", g_quarter}});
	std::vector<int> programs;
	for (const Json& event : f.controllers())
		if (event.at("type") == "program")
		{
			programs.push_back(event.at("number").get<int>());
			CHECK(event.at("channel") == 2);
		}
	CHECK(programs == std::vector<int>{0, 127});

	CHECK_THAT(f.fails("controller.line",
				   {{"part", f.part}, {"lane", lane}, {"mode", "line"}, {"from", {{"position", 10}, {"value", 0.0}}},
					   {"to", {{"position", 0}, {"value", 1.0}}}}),
		ContainsSubstring("from.position"));
	CHECK_THAT(f.fails("controller.line",
				   {{"part", f.part}, {"lane", lane}, {"mode", "line"}, {"from", {{"position", 0}, {"value", 2.0}}},
					   {"to", {{"position", 10}, {"value", 1.0}}}}),
		ContainsSubstring("value must be 0 to 1"));
	CHECK_THAT(f.fails("controller.line",
				   {{"part", f.part}, {"lane", {{"type", "controller"}, {"number", 200}}}, {"mode", "line"},
					   {"from", {{"position", 0}, {"value", 0.0}}}, {"to", {{"position", 10}, {"value", 1.0}}}}),
		ContainsSubstring("number"));
}

TEST_CASE("controller.line on a note lane changes the notes in its range", "[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({0, g_quarter, 2 * g_quarter, 3 * g_quarter});
	const auto line = [&](const Json& lane, double from, double to, const Json& extra = Json::object())
	{
		Json parameters = {{"part", f.part}, {"lane", lane}, {"mode", "line"},
			{"from", {{"position", 0}, {"value", from}}}, {"to", {{"position", 2 * g_quarter}, {"value", to}}}};
		parameters.update(extra);
		return f.call("controller.line", parameters);
	};
	Json result = line({{"type", "velocity"}}, 1.0, 0.0);
	CHECK(result.at("notes").size() == 3);
	CHECK(f.note(ids[0]).at("velocity").get<double>() == Approx(1.0));
	CHECK(f.note(ids[1]).at("velocity").get<double>() == Approx(0.5).margin(1e-4));
	CHECK(f.note(ids[2]).at("velocity").get<double>() == Approx(0.0));
	CHECK(f.note(ids[3]).at("velocity").get<double>() == Approx(0.5).margin(1e-4)); // outside the range

	// Only the notes named, such as the selected ones.
	line({{"type", "offVelocity"}}, 0.25, 0.25, {{"notes", {ids[1]}}});
	CHECK(f.note(ids[1]).at("releaseVelocity").get<double>() == Approx(0.25).margin(1e-4));
	CHECK(f.note(ids[0]).at("releaseVelocity").get<double>() == Approx(0.5).margin(1e-4));
	line({{"type", "probability"}}, 0.5, 0.5);
	CHECK(f.note(ids[2]).at("probability").get<double>() == Approx(0.5));
	line({{"type", "velocityVariance"}}, -1.0, 1.0);
	CHECK(f.note(ids[0]).at("velocityVariance").get<double>() == Approx(-1.0));
	CHECK(f.note(ids[1]).value("velocityVariance", 0.0) == Approx(0.0).margin(1e-6));
	// One undo step each.
	f.call("history.undo");
	CHECK_FALSE(f.note(ids[0]).contains("velocityVariance"));
}

TEST_CASE("controller events drawn with controller.line play back", "[integration]")
{
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 128}, {"inputChannels", 0}, {"captureSeconds", 1}});
	f.call("channel.set",
		{{"channel", f.call("project.get").at("tracks").at(0).at("channel")}, {"instrument", "testSynth"}});
	f.notes({0}, {{"velocity", 0.25}});
	f.call("controller.line",
		{{"part", f.part}, {"lane", {{"type", "controller"}, {"number", 7}}}, {"mode", "line"},
			{"from", {{"position", 0}, {"value", 0.0}}}, {"to", {{"position", g_quarter}, {"value", 1.0}}},
			{"density", g_quarter / 2}});
	f.call("controller.line",
		{{"part", f.part}, {"lane", {{"type", "velocity"}}}, {"mode", "line"},
			{"from", {{"position", 0}, {"value", 1.0}}}, {"to", {{"position", 0}, {"value", 1.0}}}});
	f.call("midi.capture", {{"start", true}});
	f.call("transport.play");
	f.call("audio.render", {{"frames", 24100}}); // a quarter at 120 BPM is half a second
	std::vector<int> volumes;
	int velocity = -1;
	const Json captured = f.call("midi.capture");
	for (const Json& message : captured.at("captured"))
	{
		const auto bytes = message.at("bytes").get<std::vector<int>>();
		if ((bytes[0] & 0xF0) == 0xB0 && bytes[1] == 7)
			volumes.push_back(bytes[2]);
		if ((bytes[0] & 0xF0) == 0x90)
			velocity = bytes[2];
	}
	CHECK(volumes == std::vector<int>{0, 64, 127});
	CHECK(velocity == 127);
}

TEST_CASE("the editor views have controller lanes, a controller selection and Auto Select Controllers", "[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({0, g_quarter});
	f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}});
	Json view = f.call("editor.get", {{"editor", "window"}});
	CHECK(view.at("lanes") == Json::array({{{"type", "velocity"}, {"number", 0}}}));
	CHECK(view.at("showLanes") == true);
	CHECK(view.at("autoSelectControllers") == false);
	CHECK(view.at("controllerSelection") == Json::array());

	view = f.call("editor.set",
		{{"editor", "window"},
			{"lanes", {{{"type", "controller"}, {"number", 1}}, {{"type", "pitchBend"}}, {{"type", "probability"}}}},
			{"showLanes", false}});
	CHECK(view.at("lanes").size() == 3);
	CHECK(view.at("lanes").at(1) == Json{{"type", "pitchBend"}, {"number", 0}});
	CHECK(view.at("showLanes") == false);
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "sysEx"}}}}}),
		ContainsSubstring("lanes[0].type"));
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "controller"}, {"number", 128}}}}}),
		ContainsSubstring("lanes[0].number"));

	// Controllers can be selected; with Auto Select Controllers, selecting notes selects the controllers in their
	// range (p. 1188).
	const Json added = f.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"number", 1}, {"value", 0.5}},
					{{"position", g_sixteenth / 2}, {"type", "pitchBend"}, {"value", 0.5}},
					{{"position", g_quarter / 2}, {"number", 1}, {"value", 0.5}}}}});
	const uint64_t c0 = added.at("controllers").at(0).at("id").get<uint64_t>();
	const uint64_t c1 = added.at("controllers").at(1).at("id").get<uint64_t>();
	const uint64_t c2 = added.at("controllers").at(2).at("id").get<uint64_t>();
	view =
		f.call("editor.set", {{"editor", "window"}, {"controllerSelection", {{{"part", f.part}, {"controller", c2}}}}});
	CHECK(view.at("controllerSelection") == Json::array({{{"part", f.part}, {"controller", c2}}}));
	CHECK_THAT(f.fails("editor.set",
				   {{"editor", "window"}, {"controllerSelection", {{{"part", f.part}, {"controller", 999999}}}}}),
		ContainsSubstring("no controller"));
	view = f.call("editor.set",
		{{"editor", "window"}, {"autoSelectControllers", true}, {"selection", {{{"part", f.part}, {"note", ids[0]}}}}});
	CHECK(view.at("controllerSelection") ==
		Json::array({{{"part", f.part}, {"controller", c0}}, {{"part", f.part}, {"controller", c1}}}));
	// Removed controllers leave the selection.
	f.call("controller.remove", {{"part", f.part}, {"controllers", {c0}}});
	view = f.call("editor.get", {{"editor", "window"}});
	CHECK(view.at("controllerSelection") == Json::array({{{"part", f.part}, {"controller", c1}}}));
	// Closing the editor clears it.
	view = f.call("editor.close", {{"editor", "window"}});
	CHECK(view.at("controllerSelection") == Json::array());
}
