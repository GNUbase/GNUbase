// SPDX-License-Identifier: GPL-3.0-or-later
// Time warp, Process Tempo, Process Bars, Set Definition from Tempo and copying warp markers in the app's UI (#159,
// docs/features/time-warp.md): the time warp tool's modes in the project window, the dialogs and the Audio menu.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter; // half a second at the default 120 BPM

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	void step(test::UiApp& app)
	{
		app.shell().step(std::chrono::milliseconds(0));
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		step(app);
	}

	// Types a number into a value field: a double click edits it, Enter takes it.
	void type(test::UiApp& app, const std::string& selector, const std::string& value)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		app.ui().key(std::nullopt, "Ctrl+A");
		app.ui().text(std::nullopt, value);
		app.ui().key(std::nullopt, "Enter");
		step(app);
	}

	void openDialog(test::UiApp& app, const std::string& command, const std::string& dialog)
	{
		app.keys().run(command, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, &dialog] { return !elements(app, dialog).empty(); }));
	}

	// The project window at a dp ratio of 1, with snapping off and a two-second 48 kHz clip on a linear audio track
	// at the project start.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			step(app);
			app.call("snap.set", {{"on", false}});
			test::Audio audio;
			audio.samples.assign(size_t{2} * 48000, 0.0F);
			const std::string file = (dir.path() / "loop.wav").u8string();
			test::writeWav(file, audio);
			track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}})
						.at("track")
						.at("id")
						.get<uint64_t>();
			const Json media =
				app.call("media.add", {{"path", file}, {"sampleRate", 48000}, {"channels", 1}, {"length", 96000}})
					.at("media");
			clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
			event = app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 96000}})
						.at("event")
						.at("id")
						.get<uint64_t>();
			REQUIRE(app.waitUntil(
				[this] { return display.project().is_object() && display.project().at("audioEvents").size() == 1; }));
		}

		// The x of a time in seconds, and a y on the first lane, in the window's pixels.
		int x(double seconds)
		{
			const Json lanes = elements(app, "event-lanes.events").at(0).at("box");
			return lanes.at("x").get<int>() + static_cast<int>(std::lround(seconds / display.view().scale));
		}

		int y() { return elements(app, "event-lanes.events").at(0).at("box").at("y").get<int>() + 10; }

		void drag(double from, double to)
		{
			app.ui().drag(std::nullopt, {"", 0, ui::Point{x(from), y()}}, {"", 0, ui::Point{x(to), y()}}, {});
			step(app);
		}

		Json warp()
		{
			const Json project = app.call("project.get");
			Json found;
			for (const Json& each : project.at("clips"))
				if (each.at("id") == clip)
					found = each.at("warp");
			REQUIRE(found.is_object());
			return found;
		}

		test::UiApp app{{1000, 700}};
		ui::EventDisplay& display;
		test::TempDir dir;
		uint64_t track = 0;
		uint64_t clip = 0;
		uint64_t event = 0;
	};
}

TEST_CASE("the time warp tool warps the grid by its modes, and Free Warp puts warp markers on audio events", "[ui]")
{
	Fixture f;
	f.app.call("project.set", {{"tempoTrackActive", true}});
	CHECK(elements(f.app, "#time-warp-tool").size() == 1);
	CHECK(elements(f.app, "#toolbox menuitem[tool=timeWarp]").size() == 1);
	f.app.keys().run("tool.timeWarp", Json::object(), f.app.main(), nullptr);
	step(f.app);
	REQUIRE(f.display.tool() == ui::Tool::TimeWarp);
	CHECK(f.display.warpMode() == ui::WarpMode::Grid);

	// Warp Grid: what plays at 1.5 seconds, the fourth quarter, plays at 2: the first tempo event becomes 90 BPM.
	f.drag(1.5, 2.0);
	REQUIRE(f.app.waitUntil(
		[&f] { return f.app.call("project.get").at("tempoTrack").at(0).at("bpm").get<double>() < 100.0; }));
	const double bpm = f.app.call("project.get").at("tempoTrack").at(0).at("bpm").get<double>();
	CHECK(std::abs(bpm - 90.0) < 1.0);
	CHECK(f.app.call("history.get").at("steps").back().at("action") == "Time Warp");

	// The second click on the tool's button opens its modes.
	click(f.app, "#time-warp-tool");
	REQUIRE(f.app.waitUntil([&f] { return !elements(f.app, "#time-warp-menu menuitem[warp-mode=freeWarp]").empty(); }));
	click(f.app, "#time-warp-menu menuitem[warp-mode=freeWarp]");
	REQUIRE(f.display.warpMode() == ui::WarpMode::Free);

	// Free Warp: the audio at one second goes to 1.2 seconds, in clip frames.
	f.drag(1.0, 1.2);
	REQUIRE(f.app.waitUntil([&f] { return f.warp().at("markers").size() == 1; }));
	const Json marker = f.warp().at("markers").at(0);
	const double frames = 2.0 * f.display.view().scale * 48000.0;
	CHECK(std::abs(marker.at("position").get<double>() - 48000.0) <= frames);
	CHECK(std::abs(marker.at("target").get<double>() - 57600.0) <= frames);
	CHECK(f.app.call("project.get").at("tempoTrack").at(0).at("bpm") == bpm);
}

TEST_CASE("Project > Tempo Track > Process Tempo and Process Bars process the tempo track", "[ui]")
{
	Fixture f;
	f.app.call("project.set", {{"tempoTrackActive", true}});
	f.app.call("transport.set", {{"left", 0}, {"right", 192000}});
	CHECK(elements(f.app, "#project-menu menuitem[command=\"tempo.openProcessTempo\"]").size() == 1);
	CHECK(elements(f.app, "#project-menu menuitem[command=\"tempo.openProcessBars\"]").size() == 1);

	// Four seconds between the locators to five: 96 BPM.
	openDialog(f.app, "tempo.openProcessTempo", "#process-tempo-dialog");
	CHECK(elements(f.app, "#pt-finish").at(0).value("text", "") == "4.000");
	type(f.app, "#pt-new-length", "5");
	CHECK(elements(f.app, "#pt-scaling").at(0).value("text", "") == "0.800");
	CHECK(elements(f.app, "#pt-new-end").at(0).at("attributes").at("value") == "5.000");
	click(f.app, "#pt-process");
	REQUIRE(f.app.waitUntil([&f] { return elements(f.app, "#process-tempo-dialog").empty(); }));
	const Json tempo = f.app.call("project.get").at("tempoTrack");
	CHECK(std::abs(tempo.at(0).at("bpm").get<double>() - 96.0) < 1e-9);

	// Insert Bars: one bar of 3/4 at bar 2.
	openDialog(f.app, "tempo.openProcessBars", "#process-bars-dialog");
	type(f.app, "#pb-start", "2");
	type(f.app, "#pb-numerator", "3");
	click(f.app, "#pb-process");
	REQUIRE(f.app.waitUntil([&f] { return elements(f.app, "#process-bars-dialog").empty(); }));
	CHECK(f.app.call("project.get").at("signatures") ==
		Json::array({{{"bar", 0}, {"numerator", 4}, {"denominator", 4}},
			{{"bar", 1}, {"numerator", 3}, {"denominator", 4}}, {{"bar", 2}, {"numerator", 4}, {"denominator", 4}}}));
	CHECK(f.app.call("history.get").at("steps").back().at("action") == "Process Bars");
}

TEST_CASE("Audio > Advanced sets the definition from the tempo and copies warp markers", "[ui]")
{
	Fixture f;
	CHECK(elements(f.app, "#audio-menu menuitem[command=\"audio.setDefinitionFromTempo\"]").size() == 1);
	f.display.select({f.event});
	openDialog(f.app, "audio.setDefinitionFromTempo", "#definition-from-tempo-dialog");
	click(f.app, "#dt-ok");
	REQUIRE(f.app.waitUntil([&f] { return f.warp().at("musical") == true; }));
	CHECK(f.warp().at("beats").size() == 3);
	CHECK(elements(f.app, "#definition-from-tempo-dialog").empty());

	// The markers of the event go into a copy of it on another clip at the same time.
	const uint64_t other = f.app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}})
							   .at("track")
							   .at("id")
							   .get<uint64_t>();
	const Json media = f.app
						   .call("media.add",
							   {{"path", (f.dir.path() / "loop.wav").u8string()}, {"sampleRate", 48000},
								   {"channels", 1}, {"length", 96000}})
						   .at("media");
	const auto clip = f.app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
	const auto copy =
		f.app.call("audioEvent.add", {{"track", other}, {"clip", clip}, {"position", 0}, {"length", 96000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	f.app.call("clip.addWarpMarker", {{"clip", f.clip}, {"position", 30000}, {"target", 32000}});
	REQUIRE(f.app.waitUntil(
		[&f] { return f.display.project().is_object() && f.display.project().at("audioEvents").size() == 2; }));
	f.display.select({f.event});
	f.app.keys().run("audio.copyWarpMarkers", Json::object(), f.app.main(), nullptr);
	f.display.select({copy});
	f.app.keys().run("audio.pasteWarpMarkers", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil(
		[&f, clip]
		{
			const Json project = f.app.call("project.get");
			for (const Json& each : project.at("clips"))
				if (each.at("id") == clip)
					return each.at("warp").at("markers") == Json::array({{{"position", 30000}, {"target", 32000}}});
			return false;
		}));
}
