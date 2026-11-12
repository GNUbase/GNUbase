// SPDX-License-Identifier: GPL-3.0-or-later
// Snap and grid (#87): every snap type and grid type, across tempo and signature changes; the snap.* commands, snap
// points, shuffle and snapping to zero crossings. tests/eventediting_tests.cpp snaps through the event display.
#include "engine/document/document.h"
#include "engine/document/snap.h"
#include "engine/engine.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::GridType;
using engine::Json;
using engine::SnapType;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;
	constexpr int64_t g_second = 48000; // a quarter is half a second at 120 BPM

	// 120 BPM for two bars, then 60 BPM: bar 2 starts at 4 seconds, bar 3 at 8.
	engine::SnapGrid slowingDown()
	{
		engine::SnapGrid grid;
		grid.tempo = time::TempoMap({{0, 120.0, false}, {2 * g_bar, 60.0, false}});
		return grid;
	}

	time::Position position(time::TimeBase base, int64_t value)
	{
		return {base, value};
	}

	time::Position ticks(int64_t value)
	{
		return position(time::TimeBase::Musical, value);
	}

	time::Position samples(int64_t value)
	{
		return position(time::TimeBase::Linear, value);
	}

	engine::SnapSettings settings(SnapType type, GridType grid = GridType::Bar)
	{
		engine::SnapSettings result;
		result.on = true;
		result.type = type;
		result.setGrid(grid);
		return result;
	}

	struct Fixture
	{
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = client.call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, const Json& parameters)
		{
			const engine::Result result = client.call(command, parameters);
			REQUIRE(!result.ok());
			return result.error;
		}

		int64_t track(const char* type, const char* timeBase = "musical")
		{
			return call("track.add", {{"type", type}, {"timeBase", timeBase}}).at("track").at("id").get<int64_t>();
		}

		int64_t part(int64_t onTrack, int64_t at, int64_t length)
		{
			return call("midiPart.add", {{"track", onTrack}, {"position", at}, {"length", length}})
				.at("part")
				.at("id")
				.get<int64_t>();
		}

		// An audio event at position playing length frames of a file from offset.
		int64_t audioEvent(
			int64_t onTrack, const std::string& path, int64_t length, int64_t position, int64_t offset, int64_t frames)
		{
			const Json media =
				call("media.add", {{"path", path}, {"sampleRate", 48000}, {"channels", 1}, {"length", length}})
					.at("media");
			const Json clip = call("clip.add", {{"media", media.at("id")}}).at("clip");
			return call("audioEvent.add",
				{{"track", onTrack}, {"clip", clip.at("id")}, {"position", position}, {"offset", offset},
					{"length", frames}})
				.at("event")
				.at("id")
				.get<int64_t>();
		}

		Json object(const char* kind, int64_t id)
		{
			const Json project = call("project.get");
			for (const Json& item : project.at(kind))
				if (item.at("id") == id)
					return item;
			FAIL("no " << kind << " " << id);
		}

		engine::Engine engine;
		engine::Client client{engine};
	};
}

TEST_CASE("bar and beat grids follow tempo and signature changes", "[unit]")
{
	const engine::SnapGrid grid = slowingDown();
	// At 6.1 seconds the next bar, at 8 seconds, is nearer than bar 2 at 4 seconds; at a constant 120 BPM it would be
	// bar 3 at 6 seconds.
	CHECK(engine::nearestGridLine(GridType::Bar, grid, (6 * g_second) + 4800) == ticks(3 * g_bar));
	CHECK(engine::nearestGridLine(GridType::Bar, grid, (6 * g_second) - 4800) == ticks(2 * g_bar));
	CHECK(engine::nearestGridLine(GridType::Bar, grid, 1000) == ticks(0));
	// Beats are a second long after the change, half a second before it.
	CHECK(engine::nearestGridLine(GridType::Beat, grid, (5 * g_second) - 4800) == ticks((2 * g_bar) + g_quarter));
	CHECK(engine::nearestGridLine(GridType::Beat, grid, g_second + 4800) == ticks(2 * g_quarter));

	// 3/4 from bar 1, 6/8 from bar 3: bars of three quarters, then beats of an eighth.
	engine::SnapGrid signatures;
	signatures.signatures = time::SignatureMap({{0, {4, 4}}, {1, {3, 4}}, {3, {6, 8}}});
	const auto at = [&signatures](int64_t tick) { return signatures.tempo.toSamples(tick, signatures.sampleRate); };
	CHECK(engine::nearestGridLine(GridType::Bar, signatures, at(5 * g_quarter)) == ticks(4 * g_quarter));
	CHECK(engine::nearestGridLine(GridType::Bar, signatures, at((11 * g_quarter) / 2)) == ticks(7 * g_quarter));
	CHECK(engine::nearestGridLine(GridType::Beat, signatures, at((10 * g_quarter) + (g_quarter / 3))) ==
		ticks((10 * g_quarter) + (g_quarter / 2)));
	CHECK(engine::gridStep(GridType::Bar, signatures, at(5 * g_quarter)) == ticks(3 * g_quarter));
	CHECK(engine::gridStep(GridType::Beat, signatures, at(11 * g_quarter)) == ticks(g_quarter / 2));
	CHECK(engine::gridStep(GridType::Bar, signatures, 0) == ticks(g_bar));
}

TEST_CASE("the quantize grid and adapt to zoom snap to note values", "[unit]")
{
	engine::SnapGrid grid = slowingDown();
	// Eighth triplets: three per quarter, a sixth of a second apart at 120 BPM.
	grid.quantize.grid = engine::midi::NoteValue{8, engine::midi::NoteValueKind::Triplet};
	CHECK(engine::nearestGridLine(GridType::Quantize, grid, 9000) == ticks(g_quarter / 3));
	CHECK(engine::gridStep(GridType::Quantize, grid, 0) == ticks(g_quarter / 3));
	// After the tempo change a triplet eighth is a third of a second.
	CHECK(engine::nearestGridLine(GridType::Quantize, grid, (4 * g_second) + 17000) ==
		ticks((2 * g_bar) + (g_quarter / 3)));

	// The view's note value, here a sixteenth; none, or one longer than the bar, is the bar.
	grid.zoomStep = g_quarter / 4;
	CHECK(engine::nearestGridLine(GridType::AdaptToZoom, grid, 7000) == ticks(g_quarter / 4));
	CHECK(engine::gridStep(GridType::AdaptToZoom, grid, 0) == ticks(g_quarter / 4));
	grid.zoomStep = 0;
	CHECK(engine::nearestGridLine(GridType::AdaptToZoom, grid, 7000) == ticks(0));
	grid.zoomStep = 2 * g_bar;
	CHECK(engine::gridStep(GridType::AdaptToZoom, grid, 0) == ticks(g_bar));
}

TEST_CASE("seconds, frames and samples grids are linear", "[unit]")
{
	engine::SnapGrid grid = slowingDown(); // tempo changes don't matter
	CHECK(engine::nearestGridLine(GridType::Ms1, grid, 70) == samples(48));
	CHECK(engine::nearestGridLine(GridType::Ms10, grid, 12345) == samples(12480));
	CHECK(engine::nearestGridLine(GridType::Ms100, grid, 7000) == samples(4800));
	CHECK(engine::nearestGridLine(GridType::Ms1000, grid, 70000) == samples(48000));
	// 25 fps: 1920 samples a frame, 24 a subframe.
	CHECK(engine::nearestGridLine(GridType::Frame, grid, 2900) == samples(3840));
	CHECK(engine::nearestGridLine(GridType::Subframe, grid, 30) == samples(24));
	CHECK(engine::nearestGridLine(GridType::QuarterFrame, grid, 500) == samples(480));
	CHECK(engine::nearestGridLine(GridType::HalfFrame, grid, 1000) == samples(960));
	CHECK(engine::nearestGridLine(GridType::TwoFrames, grid, 3000) == samples(3840));
	CHECK(engine::nearestGridLine(GridType::Second, grid, 70000) == samples(48000));
	grid.frameRate = time::g_fps60; // the user's frame rate
	CHECK(engine::nearestGridLine(GridType::Frame, grid, 900) == samples(800));
	CHECK(engine::nearestGridLine(GridType::Samples1, grid, 12345) == samples(12345));
	CHECK(engine::nearestGridLine(GridType::Samples10, grid, 12345) == samples(12350));
	CHECK(engine::nearestGridLine(GridType::Samples100, grid, 12345) == samples(12300));
	CHECK(engine::nearestGridLine(GridType::Samples44100, grid, 70000) == samples(88200));
	CHECK(engine::gridStep(GridType::Ms100, grid, 0) == samples(4800));
	CHECK(engine::gridStep(GridType::Samples100, grid, 0) == samples(100));
}

TEST_CASE("the seconds and frame grids count from the start time, as the ruler's marks do", "[unit]")
{
	// A start time of half a frame at 25 fps (20 ms): the frame lines are where the timecode shows whole frames.
	engine::SnapGrid grid = slowingDown();
	grid.startTime = 0.02;
	CHECK(engine::nearestGridLine(GridType::Frame, grid, 2900) == samples(2880)); // 0.06 s shows frame 2
	CHECK(engine::nearestGridLine(GridType::Ms100, grid, 7000) == samples(8640)); // 0.18 s shows 0.2 s
	CHECK(engine::nearestGridLine(GridType::Second, grid, 70000) == samples(47040));
	// Nearer the line before the project start, it is the first line after it.
	CHECK(engine::nearestGridLine(GridType::Ms1000, grid, 0) == samples(47040));
	// Samples count from the project start; steps keep their length.
	CHECK(engine::nearestGridLine(GridType::Samples100, grid, 12345) == samples(12300));
	CHECK(engine::gridStep(GridType::Frame, grid, 0) == samples(1920));
	// The project's grid has its start time; so does snap.position.
	engine::Project project;
	project.setup.startTime = 0.02;
	CHECK(engine::snapGrid(project, engine::GridFormat::Timecode).startTime == 0.02);
	Fixture f;
	f.call("project.set", {{"startTime", 0.02}});
	CHECK(f.call("snap.position",
			   {{"position", 2900}, {"settings", {{"on", true}, {"format", "timecode"}, {"grid", "frame"}}}})
			  .at("position") == 2880);
}

TEST_CASE("every snap type", "[unit]")
{
	const engine::SnapGrid grid = slowingDown();
	engine::SnapTarget target;
	target.position = (6 * g_second) + 4800; // 6.1 seconds: nearer to bar 3 at 8 seconds
	target.magnets = {7 * g_second};
	target.cursor = (5 * g_second) + 2400;
	target.range = 9600; // 0.2 seconds

	// Off and shuffle leave it where it is.
	CHECK(engine::snap(engine::SnapSettings(), grid, target) == samples(target.position));
	CHECK(engine::snap(settings(SnapType::Shuffle), grid, target) == samples(target.position));
	// Grid: the nearest bar, in ticks.
	CHECK(engine::snap(settings(SnapType::Grid), grid, target) == ticks(3 * g_bar));
	// Events and the cursor attract only from within range; nothing near leaves it.
	CHECK(engine::snap(settings(SnapType::Events), grid, target) == samples(target.position));
	CHECK(engine::snap(settings(SnapType::Cursor), grid, target) == samples(target.position));
	CHECK(engine::snap(settings(SnapType::GridCursor), grid, target) == ticks(3 * g_bar));
	CHECK(engine::snap(settings(SnapType::EventsGridCursor), grid, target) == ticks(3 * g_bar));
	target.position = (7 * g_second) - 4800;
	CHECK(engine::snap(settings(SnapType::Events), grid, target) == samples(7 * g_second));
	CHECK(engine::snap(settings(SnapType::EventsCursor), grid, target) == samples(7 * g_second));
	CHECK(engine::snap(settings(SnapType::EventsGridCursor), grid, target) == samples(7 * g_second));
	CHECK(engine::snap(settings(SnapType::GridCursor), grid, target) == ticks(3 * g_bar));
	target.position = 5 * g_second;
	CHECK(engine::snap(settings(SnapType::Cursor), grid, target) == samples(target.cursor));
	CHECK(engine::snap(settings(SnapType::GridCursor), grid, target) == samples(target.cursor));
	// The nearest of several within range wins.
	target.magnets = {(5 * g_second) + 4800, (5 * g_second) - 1000};
	CHECK(engine::snap(settings(SnapType::EventsCursor), grid, target) == samples((5 * g_second) - 1000));

	// Grid relative (reference manual p. 106): from 3.4.1, a bar later is 4.4.1, also across the tempo change.
	target = engine::SnapTarget();
	const int64_t from = (2 * g_bar) + (3 * g_quarter);
	target.from = grid.tempo.toSamples(from, grid.sampleRate);
	target.position = grid.tempo.toSamples(from + g_bar + (g_quarter / 3), grid.sampleRate);
	CHECK(engine::snap(settings(SnapType::GridRelative), grid, target) == ticks(from + g_bar));
	target.position = grid.tempo.toSamples(from - (2 * g_bar) - (g_quarter / 3), grid.sampleRate);
	CHECK(engine::snap(settings(SnapType::GridRelative), grid, target) == ticks(from - (2 * g_bar)));
	// In beats it keeps the distance to the beat; on a linear grid the distance in samples.
	target.from = 1000;
	target.position = 1000 + 24000 + 3000;
	CHECK(engine::snap(settings(SnapType::GridRelative, GridType::Beat), grid, target) ==
		ticks(grid.tempo.toTicks(1000, grid.sampleRate) + g_quarter));
	CHECK(engine::snap(settings(SnapType::GridRelative, GridType::Ms100), grid, target) == samples(1000 + 28800));
	// Without where it was (new events), like the grid.
	target.from.reset();
	CHECK(engine::snap(settings(SnapType::GridRelative, GridType::Ms100), grid, target) == samples(28800));
}

TEST_CASE("snap.get and snap.set keep the project's settings, saved with it but not undo steps", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "snap.changed")
				changes.push_back(event.data);
		});
	const Json grids = {{"barsBeats", "bar"}, {"seconds", "100ms"}, {"timecode", "frame"}, {"samples", "100samples"},
		{"userFrames", "frame"}};
	CHECK(f.call("snap.get") ==
		Json{{"on", false}, {"type", "grid"}, {"format", "barsBeats"}, {"grid", "bar"}, {"grids", grids},
			{"zeroCrossing", false}});
	f.track("midi"); // an undo step
	const Json set = {{"on", true}, {"type", "eventsGridCursor"}, {"grid", "beat"}, {"zeroCrossing", true}};
	Json beat = set;
	beat["format"] = "barsBeats";
	beat["grids"] = grids;
	beat["grids"]["barsBeats"] = "beat";
	CHECK(f.call("snap.set", set) == beat);
	CHECK(f.call("snap.set", {{"grid", "10ms"}}).at("grid") == "10ms"); // what isn't given stays
	CHECK(f.call("snap.get").at("type") == "eventsGridCursor");
	CHECK(f.call("project.get").at("snap").at("grid") == "10ms");
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 2);
		CHECK(changes[1].at("grid") == "10ms");
	}
	CHECK_THAT(f.fails("snap.set", {{"grid", "1/4"}}), ContainsSubstring("grid"));

	// Undo takes back the track, not the settings.
	f.call("history.undo");
	CHECK(f.call("project.get").at("tracks").empty());
	CHECK(f.call("snap.get").at("grid") == "10ms");

	// Saved with the project and loaded again.
	const test::TempDir dir;
	const std::string file = (dir.path() / "snap.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	CHECK(f.call("snap.get").at("on") == false);
	f.call("project.load", {{"path", file}});
	Json saved = beat;
	saved["format"] = "seconds";
	saved["grid"] = "10ms";
	saved["grids"]["seconds"] = "10ms";
	CHECK(f.call("snap.get") == saved);
}

// Reference manual pp. 105, 107-109: the grid types depend on the time format.
TEST_CASE("each time format keeps a grid type of its own, and the snap grid is the format's", "[unit]")
{
	Fixture f;
	const auto at = [&f](int64_t position) { return f.call("snap.position", {{"position", position}}); };
	f.call("snap.set", {{"on", true}});
	CHECK(at(100000) == Json{{"position", 2 * g_second}, {"ticks", g_bar}, {"musical", true}}); // a bar

	// Seconds: 100 ms until another one is chosen; the bars are kept for bars and beats.
	Json changed = f.call("snap.set", {{"format", "seconds"}});
	CHECK(changed.at("format") == "seconds");
	CHECK(changed.at("grid") == "100ms");
	CHECK(at(100000) == Json{{"position", 100800}, {"ticks", (100800 * g_quarter) / 24000}, {"musical", false}});
	CHECK(f.call("snap.set", {{"grid", "10ms"}}).at("grids").at("seconds") == "10ms");
	CHECK(at(100000).at("position") == 99840);
	CHECK(f.call("snap.set", {{"format", "barsBeats"}}).at("grid") == "bar");
	CHECK(at(100000).at("position") == 2 * g_second);
	CHECK(f.call("snap.set", {{"format", "seconds"}}).at("grid") == "10ms");
	f.call("snap.set", {{"format", "samples"}});
	CHECK(at(100049).at("position") == 100000);

	// The two timecode formats keep their own frame grids.
	f.call("snap.set", {{"format", "userFrames"}, {"grid", "halfFrame"}});
	changed = f.call("snap.set", {{"format", "timecode"}});
	CHECK(changed.at("grid") == "frame");
	CHECK(changed.at("grids").at("userFrames") == "halfFrame");
	// The settings given to snap.position may choose another format.
	CHECK(f.call("snap.position", {{"position", 100000}, {"settings", {{"format", "barsBeats"}}}}).at("position") ==
		2 * g_second);

	// A grid type without a format, as before #413, chooses its format: frames stay in the timecode format in use.
	changed = f.call("snap.set", {{"grid", "beat"}});
	CHECK(changed.at("format") == "barsBeats");
	CHECK(f.call("snap.set", {{"grid", "1000ms"}}).at("format") == "seconds");
	CHECK(f.call("snap.set", {{"grid", "twoFrames"}}).at("format") == "timecode");
	f.call("snap.set", {{"format", "userFrames"}});
	changed = f.call("snap.set", {{"grid", "second"}});
	CHECK(changed.at("format") == "userFrames");
	CHECK(changed.at("grids").at("timecode") == "twoFrames");
	changed = f.call("snap.set", {{"grids", {{"samples", "10samples"}, {"barsBeats", "quantize"}}}});
	CHECK(changed.at("grid") == "second");
	CHECK(changed.at("grids").at("samples") == "10samples");
	CHECK(changed.at("grids").at("barsBeats") == "quantize");

	// A grid type that isn't the format's, and unknown formats, change nothing.
	const Json before = f.call("snap.get");
	CHECK_THAT(f.fails("snap.set", {{"format", "seconds"}, {"grid", "bar"}}), ContainsSubstring("seconds"));
	CHECK_THAT(f.fails("snap.set", {{"grids", {{"seconds", "frame"}}}}), ContainsSubstring("seconds"));
	CHECK(!f.client.call("snap.set", {{"format", "feet"}}).ok());
	CHECK(f.call("snap.get") == before);
}

// The ruler follows the project setup's time format (#78, reference manual p. 69); the engine switches the snap
// format with it, so snapping doesn't depend on a UI being open.
TEST_CASE("a new project time format is the snap format, also after undo and redo", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "snap.changed")
				changes.push_back(event.data);
		});
	auto changed = [&]
	{
		const std::scoped_lock lock(mutex);
		return changes.size();
	};
	f.call("project.set", {{"timeFormat", "seconds"}});
	CHECK(f.call("snap.get").at("format") == "seconds");
	CHECK(f.call("snap.get").at("grid") == "100ms");
	CHECK(changed() == 1);
	// A format of its own right after it stays.
	f.call("snap.set", {{"format", "barsBeats"}});
	CHECK(f.call("snap.get").at("format") == "barsBeats");
	// Other project settings, and the same time format again, leave the snap format alone.
	f.call("project.set", {{"length", 120.0}, {"timeFormat", "seconds"}});
	CHECK(f.call("snap.get").at("format") == "barsBeats");
	CHECK(changed() == 2);
	// Undo and redo change the time format back and forth, and the snap format with it.
	f.call("history.undo"); // the length
	CHECK(f.call("snap.get").at("format") == "barsBeats");
	f.call("history.undo"); // the time format
	CHECK(f.call("project.get").at("timeFormat") == "barsBeats");
	CHECK(f.call("snap.get").at("format") == "barsBeats");
	f.call("history.redo");
	CHECK(f.call("snap.get").at("format") == "seconds");
	CHECK(changed() == 3);
	f.call("project.set", {{"timeFormat", "timecode"}});
	f.call("snap.set", {{"format", "samples"}});
	f.call("history.undo");
	CHECK(f.call("snap.get").at("format") == "seconds");
	CHECK(changed() == 6);
}

TEST_CASE("snap settings from before #413 take the format of their grid type", "[unit]")
{
	// The names are the ruler's.
	for (size_t i = 0; i < engine::g_rulerTimeFormats.size(); ++i)
		CHECK(std::string(engine::gridFormatName(static_cast<engine::GridFormat>(i))) ==
			engine::g_rulerTimeFormats.at(i));

	// Settings as #87 saved them: on, type, grid and zeroCrossing.
	const auto old = [](const char* grid)
	{
		return engine::snapSettingsFrom(
			Json{{"on", true}, {"type", "gridRelative"}, {"grid", grid}, {"zeroCrossing", true}},
			engine::SnapSettings());
	};
	const engine::SnapSettings defaults;
	CHECK(engine::snapSettingsFrom(Json::object(), defaults) == defaults);
	CHECK(defaults.format == engine::GridFormat::BarsBeats);
	CHECK(defaults.grid() == GridType::Bar);
	engine::SnapSettings expected = defaults;
	expected.on = true;
	expected.type = SnapType::GridRelative;
	expected.zeroCrossing = true;
	CHECK(old("bar") == expected);
	expected.grids.at(static_cast<size_t>(engine::GridFormat::BarsBeats)) = GridType::AdaptToZoom;
	CHECK(old("adaptToZoom") == expected);
	CHECK(old("1ms").format == engine::GridFormat::Seconds);
	CHECK(old("1ms").grid() == GridType::Ms1);
	CHECK(old("1ms").grids.at(0) == GridType::Bar);
	CHECK(old("subframe").format == engine::GridFormat::Timecode);
	CHECK(old("subframe").grid() == GridType::Subframe);
	CHECK(old("44100samples").format == engine::GridFormat::Samples);
	CHECK(old("44100samples").grid() == GridType::Samples44100);

	// Every grid type is one format's, the frames the two timecode formats'.
	for (int i = 0; i <= static_cast<int>(GridType::Samples44100); ++i)
	{
		const auto grid = static_cast<GridType>(i);
		int formats = 0;
		for (size_t format = 0; format < engine::g_gridFormatCount; ++format)
			formats += engine::hasGrid(static_cast<engine::GridFormat>(format), grid) ? 1 : 0;
		CHECK(formats == (engine::hasGrid(engine::GridFormat::Timecode, grid) ? 2 : 1));
	}

	// Loading, names a newer build may have keep what there was; a grid type that isn't its format's is left out.
	const Json newer = {{"format", "seconds"}, {"grid", "5ms"}, {"grids", {{"seconds", "1000ms"}, {"samples", "bar"}}}};
	CHECK_THROWS(engine::snapSettingsFrom(newer, defaults));
	const engine::SnapSettings loaded = engine::snapSettingsFrom(newer, defaults, true);
	CHECK(loaded.format == engine::GridFormat::Seconds);
	CHECK(loaded.grid() == GridType::Ms1000);
	CHECK(loaded.grids.at(static_cast<size_t>(engine::GridFormat::Samples)) == GridType::Samples100);
	CHECK(engine::snapSettingsFrom({{"format", "seconds"}, {"grid", "bar"}}, defaults, true).grid() == GridType::Ms100);

	// And what is saved now reads back the same.
	engine::SnapSettings settings = old("halfFrame");
	settings.format = engine::GridFormat::UserFrames;
	settings.setGrid(GridType::TwoFrames);
	CHECK(engine::snapSettingsFrom(engine::toJson(settings), engine::SnapSettings()) == settings);
}

// #565: the grid type menu is built from the engine's table, so the two can't disagree.
TEST_CASE("gridTypes lists each time format's grid types in the order of the grid type menu", "[unit]")
{
	using engine::GridFormat;
	const auto names = [](GridFormat format)
	{
		std::vector<std::string> result;
		for (const GridType grid : engine::gridTypes(format))
			result.emplace_back(engine::gridTypeName(grid));
		return result;
	};
	CHECK(names(GridFormat::BarsBeats) == std::vector<std::string>{"bar", "beat", "quantize", "adaptToZoom"});
	CHECK(names(GridFormat::Seconds) == std::vector<std::string>{"1ms", "10ms", "100ms", "1000ms"});
	const std::vector<std::string> frames = {"subframe", "quarterFrame", "halfFrame", "frame", "twoFrames", "second"};
	CHECK(names(GridFormat::Timecode) == frames);
	CHECK(names(GridFormat::UserFrames) == frames);
	CHECK(names(GridFormat::Samples) == std::vector<std::string>{"1sample", "10samples", "100samples", "44100samples"});

	// Exactly the grid types hasGrid accepts, and each format's default among them.
	const engine::SnapSettings defaults;
	for (size_t i = 0; i < engine::g_gridFormatCount; ++i)
	{
		const auto format = static_cast<GridFormat>(i);
		const std::vector<GridType> grids = engine::gridTypes(format);
		for (int g = 0; g <= static_cast<int>(GridType::Samples44100); ++g)
		{
			const auto grid = static_cast<GridType>(g);
			CHECK(engine::hasGrid(format, grid) == (std::find(grids.begin(), grids.end(), grid) != grids.end()));
		}
		CHECK(engine::hasGrid(format, defaults.grids.at(i)));
	}
}

// #565: a newer build's grid types (and formats) survive being loaded and saved by this one.
TEST_CASE("grid types of a newer build are kept and written back until this build chooses one", "[unit]")
{
	const Json newer = {{"on", true}, {"format", "seconds"}, {"grid", "5ms"},
		{"grids", {{"barsBeats", "beat"}, {"seconds", "5ms"}, {"samples", "bar"}, {"feet", "1foot"}}}};
	const engine::SnapSettings loaded = engine::snapSettingsFrom(newer, engine::SnapSettings(), true);
	CHECK(loaded.grid() == GridType::Ms100); // what this build knows
	CHECK(loaded.grids.at(0) == GridType::Beat);

	// The API shows only what this build knows; saving writes the newer build's entries back, but a grid type that
	// isn't its format's (samples: bar) is a mistake, not a newer build's, and is left out.
	const Json shown = engine::toJson(loaded);
	CHECK(shown.at("grids") ==
		Json{{"barsBeats", "beat"}, {"seconds", "100ms"}, {"timecode", "frame"}, {"samples", "100samples"},
			{"userFrames", "frame"}});
	const Json saved = engine::toJson(loaded, true);
	CHECK(saved.at("grids") ==
		Json{{"barsBeats", "beat"}, {"seconds", "5ms"}, {"timecode", "frame"}, {"samples", "100samples"},
			{"userFrames", "frame"}, {"feet", "1foot"}});
	CHECK(saved.at("format") == "seconds");
	CHECK(saved.at("grid") == "100ms"); // older builds read the grid in use

	// Loaded again (by the newer build or this one), the kept entries are kept again: grids, not grid, has the
	// format's grid type.
	const engine::SnapSettings again = engine::snapSettingsFrom(saved, engine::SnapSettings(), true);
	CHECK(again == loaded);
	CHECK(engine::toJson(again, true) == saved);

	// Choosing a grid type for a format replaces the kept one, with snap.set's grid or grids alike.
	engine::SnapSettings chosen = loaded;
	chosen.setGrid(GridType::Ms10);
	CHECK(engine::toJson(chosen, true).at("grids").at("seconds") == "10ms");
	CHECK(engine::toJson(chosen, true).at("grids").at("feet") == "1foot");
	chosen = engine::snapSettingsFrom({{"grids", {{"seconds", "1000ms"}}}}, loaded);
	CHECK(engine::toJson(chosen, true).at("grids").at("seconds") == "1000ms");
	chosen = engine::snapSettingsFrom({{"grid", "1ms"}}, loaded);
	CHECK(engine::toJson(chosen, true).at("grids").at("seconds") == "1ms");
	// Switching formats or anything else keeps them, as does snap.set with what snap.get returned.
	chosen = engine::snapSettingsFrom({{"format", "samples"}, {"on", false}}, loaded);
	CHECK(engine::toJson(chosen, true).at("grids").at("seconds") == "5ms");
	CHECK(engine::snapSettingsFrom(engine::toJson(loaded), loaded) == loaded);
	// A grid entry this build can't use leaves grid to choose, as before #413.
	for (const Json& unusable : {Json("bar"), Json(3)})
	{
		const Json damaged = {{"format", "timecode"}, {"grid", "quarterFrame"}, {"grids", {{"timecode", unusable}}}};
		CHECK(engine::snapSettingsFrom(damaged, engine::SnapSettings(), true).grid() == GridType::QuarterFrame);
	}

	// Entries that aren't names are left out, as are the newer build's when the settings are changed, not loaded.
	CHECK_THROWS(engine::snapSettingsFrom({{"grids", {{"seconds", "5ms"}}}}, engine::SnapSettings()));
	const engine::SnapSettings odd =
		engine::snapSettingsFrom({{"grids", {{"seconds", 5}, {"feet", Json::array()}}}}, engine::SnapSettings(), true);
	CHECK(engine::toJson(odd, true) == engine::toJson(engine::SnapSettings()));
	CHECK(engine::snapSettingsFrom({{"grids", "frame"}, {"type", 3}}, engine::SnapSettings(), true) ==
		engine::SnapSettings());
	const Json numbers = {{"on", 1}, {"zeroCrossing", "yes"}, {"format", "seconds"}};
	CHECK(engine::snapSettingsFrom(numbers, engine::SnapSettings(), true).format == engine::GridFormat::Seconds);
	CHECK(!engine::snapSettingsFrom(numbers, engine::SnapSettings(), true).on);
	CHECK_THROWS_AS(engine::snapSettingsFrom(numbers, engine::SnapSettings()), std::invalid_argument);
	CHECK(engine::snapSettingsFrom(Json(5), engine::SnapSettings(), true) == engine::SnapSettings());
}

// #565: the frame grids follow the project's frame rate (project setup, #78); user frames have the user's.
TEST_CASE("snap.position's frame grids have the project's frame rate", "[unit]")
{
	Fixture f;
	const auto at = [&f](int64_t position, const Json& settings, const Json& more = Json::object())
	{
		Json parameters = more;
		parameters["position"] = position;
		parameters["settings"] = settings;
		return f.call("snap.position", parameters).at("position").get<int64_t>();
	};
	const Json frame = {{"on", true}, {"format", "timecode"}, {"grid", "frame"}};
	CHECK(at(2900, frame) == 3840); // 25 fps: 1920 samples
	f.call("project.set", {{"frameRate", "24"}});
	CHECK(at(2900, frame) == 2000);
	f.call("project.set", {{"frameRate", "29.97"}});
	CHECK(at(3300, frame) == 3203); // 1.001 / 30 seconds
	CHECK(at(3300, {{"on", true}, {"format", "timecode"}, {"grid", "halfFrame"}}) == 3203);
	// User frames: the user's rate, 30 fps as the event display has it until it is a setting.
	f.call("project.set", {{"frameRate", "24"}});
	CHECK(at(2500, {{"on", true}, {"format", "userFrames"}, {"grid", "frame"}}) == 3200);
	// framesPerSecond chooses a rate for both.
	CHECK(at(2900, frame, {{"framesPerSecond", 60}}) == 3200);
	CHECK(at(2500, {{"on", true}, {"format", "userFrames"}, {"grid", "frame"}}, {{"framesPerSecond", 24}}) == 2000);
	// The other grids don't depend on it.
	CHECK(at(2900, {{"on", true}, {"format", "seconds"}, {"grid", "10ms"}}) == 2880);
}

TEST_CASE("snap.position snaps as the project window does", "[unit]")
{
	Fixture f;
	const int64_t midi = f.track("midi");
	const int64_t audio = f.track("audio", "linear");
	const int64_t part = f.part(midi, g_bar, g_bar);      // 2 to 4 seconds
	const int64_t other = f.part(midi, 3 * g_bar, g_bar); // 6 to 8 seconds
	const int64_t event = f.audioEvent(audio, "take.wav", 10 * g_second, 9 * g_second, 0, g_second);

	// Off, it stays; the settings given override the project's.
	CHECK(f.call("snap.position", {{"position", 100000}}) ==
		Json{{"position", 100000}, {"ticks", (100000 * g_quarter) / 24000}, {"musical", false}});
	const Json bar = f.call("snap.position", {{"position", 100000}, {"settings", {{"on", true}}}});
	CHECK(bar == Json{{"position", 2 * g_second}, {"ticks", g_bar}, {"musical", true}});

	// Events: other events' edges and markers within a tenth of a second, or range; not the events being edited.
	f.call("snap.set", {{"on", true}, {"type", "events"}});
	CHECK(f.call("snap.position", {{"position", (4 * g_second) + 3000}}).at("position") == 4 * g_second);
	CHECK(f.call("snap.position", {{"position", (4 * g_second) + 3000}, {"events", {part}}}).at("position") ==
		(4 * g_second) + 3000);
	CHECK(f.call("snap.position", {{"position", (4 * g_second) + 6000}}).at("position") == (4 * g_second) + 6000);
	CHECK(
		f.call("snap.position", {{"position", (4 * g_second) + 6000}, {"range", 6000}}).at("position") == 4 * g_second);
	f.call("marker.add", {{"position", 10 * g_quarter}}); // 5 seconds
	CHECK(f.call("snap.position", {{"position", (5 * g_second) - 100}}).at("position") == 5 * g_second);
	// Every marker track's markers attract (#415): with another marker track active, the marker still does.
	f.call("track.add", {{"type", "marker"}});
	REQUIRE(f.call("marker.list").at("markers").empty());
	CHECK(f.call("snap.position", {{"position", (5 * g_second) - 100}}).at("position") == 5 * g_second);
	// An audio event's snap point attracts, and is a point in its clip.
	f.call("event.setSnapPoint", {{"events", {event}}, {"position", (9 * g_second) + 12000}});
	CHECK(f.object("audioEvents", event).at("snapPoint") == 12000);
	CHECK(f.call("snap.position", {{"position", (9 * g_second) + 12500}}).at("position") == (9 * g_second) + 12000);
	CHECK_THAT(
		f.fails("event.setSnapPoint", {{"events", {event}}, {"position", g_second}}), ContainsSubstring("inside"));
	CHECK_THAT(
		f.fails("event.setSnapPoint", {{"events", {part}}, {"position", 3 * g_second}}), ContainsSubstring("audio"));
	// By default at the project cursor; it moves with the event.
	f.call("transport.locate", {{"position", (9 * g_second) + 24000}});
	f.call("event.setSnapPoint", {{"events", {event}}});
	CHECK(f.object("audioEvents", event).at("snapPoint") == 24000);
	f.call("event.move", {{"events", {event}}, {"delta", g_second}});
	CHECK(f.call("snap.position", {{"position", (10 * g_second) + 24100}}).at("position") == (10 * g_second) + 24000);
	(void)other;

	// The cursor; the zoom grid and the frame rate.
	f.call("snap.set", {{"type", "cursor"}});
	CHECK(f.call("snap.position", {{"position", (9 * g_second) + 24300}}).at("position") == (9 * g_second) + 24000);
	f.call("snap.set", {{"type", "grid"}, {"grid", "adaptToZoom"}});
	CHECK(f.call("snap.position", {{"position", 7000}, {"zoomStep", g_quarter / 4}}).at("ticks") == g_quarter / 4);
	f.call("snap.set", {{"grid", "frame"}});
	CHECK(f.call("snap.position", {{"position", 900}, {"framesPerSecond", 60}}).at("position") == 800);
	// Grid relative from where it was.
	f.call("snap.set", {{"type", "gridRelative"}, {"grid", "bar"}});
	CHECK(f.call("snap.position", {{"position", (5 * g_second) + 1000}, {"from", 3 * g_second}}).at("ticks") ==
		(2 * g_bar) + (2 * g_quarter));
}

TEST_CASE("event.shuffle changes the order of adjoining events", "[unit]")
{
	Fixture f;
	const int64_t midi = f.track("midi");
	// A, B and C one after the other from bar 1, a bar, two bars and a bar long; D apart at bar 10.
	const int64_t a = f.part(midi, g_bar, g_bar);
	const int64_t b = f.part(midi, 2 * g_bar, 2 * g_bar);
	const int64_t c = f.part(midi, 4 * g_bar, g_bar);
	const int64_t d = f.part(midi, 10 * g_bar, g_bar);
	const auto start = [&f](int64_t id) { return f.object("midiParts", id).at("position").get<int64_t>(); };
	const auto at = [](int64_t tick) { return tick * 24000 / g_quarter; }; // in samples, at 120 BPM

	// A dragged past the middle of B goes after it: B, A, C, with no gap and the row's start kept.
	f.call("event.shuffle", {{"events", {a}}, {"position", at((3 * g_bar) + 1)}});
	CHECK(start(b) == g_bar);
	CHECK(start(a) == 3 * g_bar);
	CHECK(start(c) == 4 * g_bar);
	CHECK(start(d) == 10 * g_bar);
	// Not past it, A stays after B; past C, after C; dragged far left, to the front. One undo step each.
	f.call("event.shuffle", {{"events", {a}}, {"position", at((2 * g_bar) + 1)}});
	CHECK(start(a) == 3 * g_bar);
	f.call("event.shuffle", {{"events", {a}}, {"position", at(9 * g_bar)}});
	CHECK(start(b) == g_bar);
	CHECK(start(c) == 3 * g_bar);
	CHECK(start(a) == 4 * g_bar);
	f.call("event.shuffle", {{"events", {c, a}}, {"position", 0}});
	CHECK(start(c) == g_bar);
	CHECK(start(a) == 2 * g_bar);
	CHECK(start(b) == 3 * g_bar);
	f.call("history.undo");
	CHECK(start(c) == 3 * g_bar);

	// Only adjoining events on one track, unlocked.
	CHECK_THAT(f.fails("event.shuffle", {{"events", {b, d}}, {"position", 0}}), ContainsSubstring("adjoin"));
	const int64_t other = f.part(f.track("midi"), 0, g_bar);
	CHECK_THAT(f.fails("event.shuffle", {{"events", {b, other}}, {"position", 0}}), ContainsSubstring("one track"));
	f.call("event.set", {{"events", {b}}, {"lock", true}});
	CHECK_THAT(f.fails("event.shuffle", {{"events", {b}}, {"position", 0}}), ContainsSubstring("locked"));
	// A locked neighbour ends the row: C and A swap behind it.
	f.call("event.shuffle", {{"events", {a}}, {"position", 0}});
	CHECK(start(b) == g_bar);
	CHECK(start(a) == 3 * g_bar);
	CHECK(start(c) == 4 * g_bar);
}

TEST_CASE("event.shuffle lays out audio events on musical tracks by their length where they go", "[unit]")
{
	// Audio events keep their length in samples, so across a tempo change their length in ticks depends on where
	// they start (#414). 120 BPM for a quarter (24000 samples), then 60 BPM.
	Fixture f;
	f.call("project.set", {{"tempoTrackActive", true}});
	f.call("tempo.add", {{"position", g_quarter}, {"bpm", 60.0}, {"ramp", false}});
	const int64_t audio = f.track("audio");
	// A, half a second from the start; B and C, a second each, one after the other (positions in ticks).
	const int64_t a = f.audioEvent(audio, "a.wav", g_second, 0, 0, g_second / 2);
	const int64_t b = f.audioEvent(audio, "b.wav", g_second, g_quarter, 0, g_second);
	const int64_t c = f.audioEvent(audio, "c.wav", g_second, 2 * g_quarter, 0, g_second);
	const auto span = [&f](int64_t id)
	{
		const Json info = f.call("event.get", {{"events", {id}}}).at("events").at(0);
		return std::pair<int64_t, int64_t>{info.at("start").get<int64_t>(), info.at("end").get<int64_t>()};
	};
	const auto near = [](int64_t actual, int64_t expected) { return std::abs(actual - expected) <= 1; };

	// A to the end: B and C close up from the start, A after them, with no gap or overlap.
	f.call("event.shuffle", {{"events", {a}}, {"position", 3 * g_second}});
	CHECK(near(span(b).first, 0));
	CHECK(near(span(b).second, g_second));
	CHECK(near(span(c).first, g_second));
	CHECK(near(span(c).second, 2 * g_second));
	CHECK(near(span(a).first, 2 * g_second));
	CHECK(near(span(a).second, 5 * g_second / 2));
	// Still one row: A back to the front.
	f.call("event.shuffle", {{"events", {a}}, {"position", 0}});
	CHECK(near(span(a).first, 0));
	CHECK(near(span(b).first, g_second / 2));
	CHECK(near(span(c).first, 3 * g_second / 2));
	CHECK(near(span(c).second, 5 * g_second / 2));

	// A position between the two middles decides at the length where B goes (#873): closed up at the start,
	// B's middle is 0.75 quarter at that length, but 0.5 quarter at the length at its old position, so 0.6
	// quarter (0.3 s) keeps A in front of B.
	f.call("event.shuffle", {{"events", {a}}, {"position", 3 * g_second / 10}});
	CHECK(near(span(a).first, 0));
	CHECK(near(span(b).first, g_second / 2));
	CHECK(near(span(c).first, 3 * g_second / 2));
	CHECK(near(span(c).second, 5 * g_second / 2));
}

TEST_CASE("event.shuffle moves a row's end when an audio part and audio events cross a tempo change", "[unit]")
{
	// Audio parts keep their length in ticks and audio events in samples, so a row with both ends somewhere
	// else after a shuffle across a tempo change, and may overlap the locked event that ends the row; that is
	// the row's new layout, not an error (#873). 120 BPM for a quarter (24000 samples), then 60 BPM.
	Fixture f;
	f.call("project.set", {{"tempoTrackActive", true}});
	f.call("tempo.add", {{"position", g_quarter}, {"bpm", 60.0}, {"ramp", false}});
	const int64_t audio = f.track("audio");
	// Part P over the first quarter, audio event E (half a second) right after it, locked L from sample
	// 48000 on (positions in ticks).
	const int64_t p = f.call("audioPart.add", {{"track", audio}, {"position", 0}, {"length", g_quarter}})
						  .at("part")
						  .at("id")
						  .get<int64_t>();
	const int64_t e = f.audioEvent(audio, "e.wav", g_second, g_quarter, 0, g_second / 2);
	const int64_t l = f.audioEvent(audio, "l.wav", g_second, 3 * g_quarter / 2, 0, g_second / 2);
	f.call("event.set", {{"events", {l}}, {"lock", true}});
	const auto span = [&f](int64_t id)
	{
		const Json info = f.call("event.get", {{"events", {id}}}).at("events").at(0);
		return std::pair<int64_t, int64_t>{info.at("start").get<int64_t>(), info.at("end").get<int64_t>()};
	};
	const auto near = [](int64_t actual, int64_t expected) { return std::abs(actual - expected) <= 1; };
	// The row is P and E; the locked L ends it, and the row ends at sample 48000.
	CHECK(near(span(p).second, g_second / 2));
	CHECK(near(span(e).second, g_second));

	// E to the front: P follows at its length in ticks where it goes, from sample 24000 to 72000, so the
	// row's end moves to 72000, half a second into L, which doesn't move.
	f.call("event.shuffle", {{"events", {e}}, {"position", 0}});
	CHECK(near(span(e).first, 0));
	CHECK(near(span(e).second, g_second / 2));
	CHECK(f.object("audioParts", p).at("position").get<int64_t>() == g_quarter);
	CHECK(near(span(p).first, g_second / 2));
	CHECK(near(span(p).second, 3 * g_second / 2));
	CHECK(near(span(l).first, g_second));
	CHECK(near(span(l).second, 3 * g_second / 2));
}

TEST_CASE("splitting and resizing audio events snap to zero crossings", "[unit]")
{
	// A sine that crosses zero every 100 frames.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 9600; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);

	Fixture f;
	const int64_t audio = f.track("audio", "linear");
	const int64_t event = f.audioEvent(audio, file.u8string(), 9600, g_second, 50, 4800);
	f.call("event.split", {{"events", {event}}, {"positions", {g_second + 1030}}, {"zeroCrossing", true}});
	CHECK(f.object("audioEvents", event).at("length") == 1050); // cut at frame 1100 of the file
	// The end moves to the crossing nearest to where it was dragged, the start too; sizing that moves the contents
	// doesn't.
	f.call("event.resize", {{"events", {event}}, {"edge", "end"}, {"delta", -170}, {"zeroCrossing", true}});
	CHECK(f.object("audioEvents", event).at("length") == 850);
	f.call("event.resize", {{"events", {event}}, {"edge", "start"}, {"delta", 260}, {"zeroCrossing", true}});
	CHECK(f.object("audioEvents", event).at("offset") == 300);
	CHECK(f.object("audioEvents", event).at("position") == g_second + 250);
	f.call("event.resize",
		{{"events", {event}}, {"edge", "start"}, {"delta", 30}, {"mode", "moveContents"}, {"zeroCrossing", true}});
	CHECK(f.object("audioEvents", event).at("position") == g_second + 280);
	// Without the file, or MIDI parts, where they are.
	const int64_t missing = f.audioEvent(audio, "missing.wav", 9600, 3 * g_second, 0, 4800);
	f.call("event.split", {{"events", {missing}}, {"positions", {(3 * g_second) + 1030}}, {"zeroCrossing", true}});
	CHECK(f.object("audioEvents", missing).at("length") == 1030);
	// Near the start of an event playing from frame 10, the crossing at frame 0 is nearer but outside the event: it
	// splits at the one at frame 100.
	const int64_t early = f.audioEvent(audio, file.u8string(), 9600, 5 * g_second, 10, 4800);
	f.call("event.split", {{"events", {early}}, {"positions", {(5 * g_second) + 5}}, {"zeroCrossing", true}});
	CHECK(f.object("audioEvents", early).at("length") == 90);
	// Without events, a position just before an event playing from frame 90 leaves it alone, although the crossing
	// nearest to it, at frame 100, is inside.
	const int64_t after = f.audioEvent(audio, file.u8string(), 9600, 7 * g_second, 90, 4800);
	CHECK(f.call("event.split", {{"positions", {(7 * g_second) - 5}}, {"zeroCrossing", true}}).at("events").empty());
	CHECK(f.object("audioEvents", after).at("length") == 4800);
}

TEST_CASE(
	"snap.zeroCrossings moves positions to zero crossings of the audio on the first track that plays there", "[unit]")
{
	// A sine that crosses zero every 100 frames.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 9600; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);

	Fixture f;
	const int64_t midi = f.track("midi");
	const int64_t empty = f.track("audio", "linear");
	const int64_t audio = f.track("audio", "linear");
	f.audioEvent(audio, file.u8string(), 9600, g_second, 0, 4800);
	// Inside the event, to the crossing nearest; outside it, and on tracks without audio, where they are.
	const Json moved = f.call("snap.zeroCrossings",
		{{"positions", {g_second + 1030, g_second + 2960, 3 * g_second}}, {"tracks", {midi, empty, audio}}});
	CHECK(moved.at("positions") == Json{g_second + 1000, g_second + 3000, 3 * g_second});
	CHECK(f.call("snap.zeroCrossings", {{"positions", {g_second + 1030}}, {"tracks", {midi, empty}}}).at("positions") ==
		Json::array({g_second + 1030}));
}

TEST_CASE("snap.zeroCrossings follows the audio event that plays: in front, unmuted and within its part", "[unit]")
{
	// A sine that crosses zero every 100 frames, in two events over each other from a second on: A from frame 0, B
	// (added later, so in front) from frame 50, whose crossings are 50 samples off A's.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 9600; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);

	Fixture f;
	const int64_t audio = f.track("audio", "linear");
	const int64_t a = f.audioEvent(audio, file.u8string(), 9600, g_second, 0, 4800);
	f.audioEvent(audio, file.u8string(), 9600, g_second, 50, 4800);
	const auto moved = [&f, audio]
	{
		return f.call("snap.zeroCrossings", {{"positions", {g_second + 1030}}, {"tracks", {audio}}})
			.at("positions")
			.at(0)
			.get<int64_t>();
	};
	const int64_t onA = g_second + 1000;
	const int64_t onB = g_second + 1050;
	CHECK(moved() == onB);
	f.call("event.toFront", {{"events", {a}}});
	CHECK(moved() == onA);
	// A muted event plays nothing and hides nothing.
	f.call("event.set", {{"events", {a}}, {"mute", true}});
	CHECK(moved() == onB);
	f.call("event.set", {{"events", {a}}, {"mute", false}});
	CHECK(moved() == onA);
	// Nor does one outside its audio part, or in a muted part.
	const Json part = f.call("event.toPart", {{"events", {a}}}).at("events").at(0);
	f.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", -4000}});
	CHECK(moved() == onB);
	f.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", 4000}});
	CHECK(moved() == onA);
	f.call("event.set", {{"events", {part}}, {"mute", true}});
	CHECK(moved() == onB);
}

TEST_CASE("snap.zeroCrossings moves a position only to where the event playing there plays", "[unit]")
{
	// A sine that crosses zero every 100 frames, in an event A from a second on. 1080 samples into A, the crossing at
	// 1100 is the nearest, the one at 1000 the next.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 9600; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);

	Fixture f;
	const int64_t audio = f.track("audio", "linear");
	const int64_t a = f.audioEvent(audio, file.u8string(), 9600, g_second, 0, 4800);
	const auto moved = [&f, audio]
	{
		return f.call("snap.zeroCrossings", {{"positions", {g_second + 1080}}, {"tracks", {audio}}})
			.at("positions")
			.at(0)
			.get<int64_t>();
	};
	CHECK(moved() == g_second + 1100);
	// A's part ends at 1090: A doesn't play at 1100.
	const Json part = f.call("event.toPart", {{"events", {a}}}).at("events").at(0);
	f.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", 1090 - 4800}});
	CHECK(moved() == g_second + 1000);
	f.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", 4800 - 1090}});
	CHECK(moved() == g_second + 1100);

	// Nor under an event in front of it, B from 1090 on.
	const int64_t b = f.audioEvent(audio, file.u8string(), 9600, g_second + 1090, 50, 1000);
	CHECK(moved() == g_second + 1000);
	// Behind A, B hides nothing.
	f.call("event.toFront", {{"events", {a}}});
	CHECK(moved() == g_second + 1100);
	f.call("event.toFront", {{"events", {b}}});
	// With C in front of A from 900 to 1060 too, A plays from 1060 to 1090, where the audio doesn't cross zero: the
	// position stays.
	f.audioEvent(audio, file.u8string(), 9600, g_second + 900, 0, 160);
	CHECK(moved() == g_second + 1080);
}

TEST_CASE("snap.zeroCrossings takes the piece of a split event that plays in front, as playback does", "[unit]")
{
	// The sine crosses zero every 100 frames. The piece after the split has its event's place in the stack; of the
	// two, playback plays the one with the higher id, the right piece.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 9600; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);

	Fixture f;
	const int64_t audio = f.track("audio", "linear");
	const int64_t left = f.audioEvent(audio, file.u8string(), 9600, g_second, 0, 4800);
	const Json right = f.call("event.split", {{"events", {left}}, {"positions", {g_second + 2000}}}).at("events").at(0);
	REQUIRE(right.get<int64_t>() > left);
	// The right piece, from frame 2000 of the file on, moves left over the left one to 990: its crossings are at 990
	// plus a multiple of 100, the left piece's at multiples of 100.
	f.call("event.move", {{"events", {right}}, {"delta", -1010}});
	REQUIRE(f.object("audioEvents", right.get<int64_t>()).at("position") == g_second + 990);
	const auto moved = [&f, audio](int64_t position)
	{
		return f.call("snap.zeroCrossings", {{"positions", {g_second + position}}, {"tracks", {audio}}})
				   .at("positions")
				   .at(0)
				   .get<int64_t>() -
			g_second;
	};
	// In the overlap the right piece plays: its crossing at 1090, not the left piece's at 1100.
	CHECK(moved(1080) == 1090);
	// Just before it the left piece plays, up to 990: its crossing at 900, not the one at 1000 under the right piece.
	CHECK(moved(960) == 900);
}
