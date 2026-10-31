// SPDX-License-Identifier: GPL-3.0-or-later
// Snap and grid (#87): every snap type and grid type, across tempo and signature changes; the snap.* commands, snap
// points, shuffle and snapping to zero crossings. tests/eventediting_tests.cpp snaps through the event display.
#include "engine/document/snap.h"
#include "engine/engine.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
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
		result.grid = grid;
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
	CHECK(engine::nearestGridLine(GridType::Bar, grid, 6 * g_second + 4800) == ticks(3 * g_bar));
	CHECK(engine::nearestGridLine(GridType::Bar, grid, 6 * g_second - 4800) == ticks(2 * g_bar));
	CHECK(engine::nearestGridLine(GridType::Bar, grid, 1000) == ticks(0));
	// Beats are a second long after the change, half a second before it.
	CHECK(engine::nearestGridLine(GridType::Beat, grid, 5 * g_second - 4800) == ticks((2 * g_bar) + g_quarter));
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
	grid.quantize.grid = *engine::midi::noteValueFromString("1/8T");
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
	CHECK(f.call("snap.get") == Json{{"on", false}, {"type", "grid"}, {"grid", "bar"}, {"zeroCrossing", false}});
	f.track("midi"); // an undo step
	const Json set = {{"on", true}, {"type", "eventsGridCursor"}, {"grid", "beat"}, {"zeroCrossing", true}};
	CHECK(f.call("snap.set", set) == set);
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
	CHECK(f.call("snap.get") ==
		Json{{"on", true}, {"type", "eventsGridCursor"}, {"grid", "10ms"}, {"zeroCrossing", true}});
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
}
