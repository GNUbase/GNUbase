// SPDX-License-Identifier: GPL-3.0-or-later
// Audio quantizing in the app (#145, docs/features/audio-quantize.md), driven with ui::Automation as MCP's UI tools
// drive it: Edit > Quantize and Reset Quantize on the project window's selected audio events, the sample editor
// toolbar's quantize functions, Warp Quantize On/Off in the quantize panel and the toolbar, the panel's section for
// several audio tracks: the Hitpoint Tracks' priorities, Slice, Create, Crossfade, Nudge Crossfade and Open Crossfade
// Editor, and Create Groove Quantize Preset from an audio event.
// tests/audioquantize_tests.cpp checks the engine's side.
#include "platform/log.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/sampleeditor.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = 24000; // samples at 120 BPM and 48 kHz

	// Two linear audio tracks with an event each at the project start playing a bar of clicks a little off the
	// quarters, with hitpoints at the clicks: the second track's a little later.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			test::Audio audio;
			audio.samples.assign(4 * g_quarter, 0.0F);
			for (const int64_t click : {1000, 25000, 47000, 73000})
				for (int64_t i = 0; i < 240; ++i)
					audio.samples[static_cast<size_t>(click + i)] = 0.5F;
			test::writeWav(file, audio);
			const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media");
			for (int i = 0; i < 2; ++i)
			{
				const auto track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}})
									   .at("track")
									   .at("id")
									   .get<uint64_t>();
				const auto clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
				Json hitpoints = Json::array();
				for (const int64_t position : {1000, 25000, 47000, 73000})
					hitpoints.push_back(
						{{"position", position + (static_cast<int64_t>(i) * 100)}, {"peak", -6.0}, {"intensity", 1.0}});
				app.call("clip.setHitpoints", {{"clip", clip}, {"hitpoints", hitpoints}});
				tracks.push_back(track);
				clips.push_back(clip);
				events.push_back(
					app.call("audioEvent.add",
						   {{"track", track}, {"clip", clip}, {"position", 2000}, {"offset", 0},
							   {"length", 4 * g_quarter}})
						.at("event")
						.at("id")
						.get<uint64_t>());
			}
			app.call("quantize.set", {{"preset", "1/4"}});
			REQUIRE(app.waitUntil([&] { return display.project().at("audioEvents").size() == 2; }));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].at("visible").get<bool>();
		}

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector, int count = 1)
		{
			INFO(selector);
			REQUIRE(app.waitUntil([&] { return shown(selector); }));
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void menu(const std::string& command)
		{
			click("#menu-bar button[menu=edit-menu]");
			click("menuitem[command=\"" + command + "\"]");
		}

		void openPanel()
		{
			app.keys().run("window.quantizePanel", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([this] { return shown("#quantize-panel"); }));
		}

		Json project() { return app.call("project.get"); }

		// The positions of a track's events, sorted.
		std::vector<int64_t> positions(uint64_t track)
		{
			std::vector<int64_t> found;
			const Json all = project();
			for (const Json& event : all.at("audioEvents"))
				if (event.at("track") == track)
					found.push_back(event.at("position").get<int64_t>());
			std::sort(found.begin(), found.end());
			return found;
		}

		bool positionsAre(uint64_t track, const std::vector<int64_t>& expected)
		{
			return app.waitUntil([&] { return positions(track) == expected; });
		}

		Json settings() { return app.call("quantize.get").at("settings"); }

		test::TempDir dir;
		std::filesystem::path file = dir.path() / "clicks.wav";
		test::UiApp app{{1024, 720}};
		ui::EventDisplay& display;
		std::vector<uint64_t> tracks;
		std::vector<uint64_t> clips;
		std::vector<uint64_t> events;
	};
}

TEST_CASE("Edit > Quantize and Reset Quantize quantize the selected audio events", "[ui]")
{
	Fixture f;
	CHECK(!f.app.keys().canRun("edit.quantize", f.app.main(), nullptr));
	f.display.select({f.events[0]});
	CHECK(f.app.keys().canRun("edit.quantize", f.app.main(), nullptr));
	CHECK(f.app.keys().canRun("edit.resetQuantize", f.app.main(), nullptr));
	// Audio events have no ends or lengths to quantize.
	CHECK(!f.app.keys().canRun("edit.quantizeEnds", f.app.main(), nullptr));
	f.menu("edit.quantize");
	CHECK(f.positionsAre(f.tracks[0], {0}));
	CHECK(f.positions(f.tracks[1]) == std::vector<int64_t>{2000});
	f.menu("edit.resetQuantize");
	CHECK(f.positionsAre(f.tracks[0], {2000}));
}

TEST_CASE("the sample editor's toolbar quantizes the event it edits", "[ui]")
{
	Fixture f;
	// Apply Quantize there quantizes the event it edits (reference manual p. 367).
	f.display.select({f.events[0]});
	f.app.keys().run("editor.openClose", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().sampleEditorWindow() != nullptr; }));
	const ui::WindowId window = f.app.appUi().sampleEditorWindowId();
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().sampleEditorWindow()->event() == f.events[0]; }));
	const auto shown = [&](const std::string& selector)
	{
		const Json found = f.app.ui().dom(window, selector, 0).at("elements");
		return !found.empty() && found[0].at("visible").get<bool>();
	};
	REQUIRE(f.app.waitUntil([&] { return shown("#toolbar-quantize-apply"); }));
	CHECK(shown("#toolbar-quantize-presets"));
	CHECK(shown("#toolbar-quantize-soft"));
	CHECK(shown("#toolbar-quantize-panel"));
	f.app.ui().click(window, {"#toolbar-quantize-apply", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.positionsAre(f.tracks[0], {0}));
	CHECK(f.positions(f.tracks[1]) == std::vector<int64_t>{2000});
}

TEST_CASE("Warp Quantize On/Off in the quantize panel follows the settings, as the toolbar does", "[ui]")
{
	Fixture f;
	f.openPanel();
	CHECK(f.shown("#quantize-slice"));
	CHECK(f.hasClass("#quantize-create-markers", "hidden"));
	f.click("#quantize-warp");
	CHECK(f.app.waitUntil([&] { return f.settings().at("warpQuantize") == true; }));
	CHECK(f.app.waitUntil([&] { return f.hasClass("#quantize-warp", "checked"); }));
	CHECK(f.app.waitUntil([&] { return f.hasClass("#toolbar-quantize-warp", "checked"); }));
	CHECK(f.hasClass("#quantize-slice", "hidden"));
	CHECK(f.shown("#quantize-create-markers"));
	f.app.call("quantize.set", {{"settings", {{"warpQuantize", false}}}});
	CHECK(f.app.waitUntil([&] { return !f.hasClass("#quantize-warp", "checked"); }));
	CHECK(f.app.waitUntil([&] { return !f.hasClass("#toolbar-quantize-warp", "checked"); }));
}

TEST_CASE("the quantize panel slices, quantizes and crossfades several audio tracks", "[ui]")
{
	Fixture f;
	f.display.select({f.events[0], f.events[1]});
	f.openPanel();
	// Sliced at the hitpoints of both tracks, which are within the range of each other: the first one's.
	f.click("#quantize-slice");
	CHECK(f.positionsAre(f.tracks[0], {2000, 3000, 27000, 49000, 75000}));
	CHECK(f.positions(f.tracks[1]) == f.positions(f.tracks[0]));

	// Slicing selects every slice, so that Quantize and Crossfade go on with all of them.
	const size_t slices = f.project().at("audioEvents").size();
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("audioEvents").size() == slices; }));
	REQUIRE(f.app.waitUntil([&] { return f.display.selection().size() == slices; }));
	// Quantized, both tracks' slices move alike: the first two to the bar's start.
	f.click("#quantize-apply");
	CHECK(f.positionsAre(f.tracks[0], {0, 0, 24000, 48000, 72000}));
	CHECK(f.positions(f.tracks[1]) == f.positions(f.tracks[0]));

	// Crossfade closes the gaps and crossfades each slice into the next, but for the two at the same place.
	f.click("#quantize-crossfade");
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json now = f.project();
			return std::count_if(now.at("audioEvents").begin(), now.at("audioEvents").end(),
					   [](const Json& event) { return event.at("crossfade").is_object(); }) == 6;
		}));

	// Nudge Crossfade Right moves every crossfaded slice's start a millisecond later, Left back again.
	const std::vector<int64_t> crossfaded = f.positions(f.tracks[0]);
	f.click("#quantize-nudge-right");
	CHECK(f.app.waitUntil([&] { return f.positions(f.tracks[0]) != crossfaded; }));
	const std::vector<int64_t> nudged = f.positions(f.tracks[0]);
	REQUIRE(nudged.size() == crossfaded.size());
	for (size_t i = 0; i < nudged.size(); ++i)
		CHECK((nudged[i] == crossfaded[i] || nudged[i] == crossfaded[i] + 48));
	f.click("#quantize-nudge-left");
	CHECK(f.positionsAre(f.tracks[0], crossfaded));

	// Open Crossfade Editor opens the editor of a selected slice's crossfade, and crossfades nothing itself.
	const Json before = f.project().at("audioEvents");
	f.click("#quantize-crossfade-editor");
	CHECK(f.app.waitUntil([&] { return f.shown("#crossfade-editor"); }));
	CHECK(f.project().at("audioEvents") == before);
}

TEST_CASE("Create Groove Quantize Preset makes a groove of the selected audio event's hitpoints", "[ui]")
{
	Fixture f;
	CHECK(!f.app.keys().canRun("edit.createGrooveQuantize", f.app.main(), nullptr));
	f.display.select({f.events[0]});
	CHECK(f.app.keys().canRun("edit.createGrooveQuantize", f.app.main(), nullptr));
	f.app.keys().run("edit.createGrooveQuantize", Json::object(), f.app.main(), nullptr);
	// A groove of the four hitpoints, which the project's settings then are.
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json groove = f.settings().at("groove");
			return groove.is_object() && groove.at("points").size() == 4;
		}));
	const Json presets = f.app.call("quantize.presets").at("presets");
	CHECK(std::any_of(presets.begin(), presets.end(),
		[&](const Json& preset) { return preset.at("name") == f.app.call("quantize.get").at("preset"); }));
}

TEST_CASE("the quantize panel's Hitpoint Tracks give each track's priority to Slice", "[ui]")
{
	Fixture f;
	f.openPanel();
	// The tracks of the selected events that have hitpoints, at priority 1.
	CHECK(f.elements("#quantize-hitpoint-tracks .quantize-priority").empty());
	f.display.select({f.events[0], f.events[1]});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#quantize-hitpoint-tracks .quantize-priority").size() == 2; }));
	// A priority field's value.
	const auto priority = [&f](const std::string& selector)
	{
		const Json fields = f.elements(selector);
		return fields.empty() ? -1.0 : std::stod(fields[0].at("attributes").at("value").get<std::string>());
	};
	CHECK(priority("#quantize-hitpoint-tracks .quantize-priority") == 1.0);
	f.display.select({f.events[1]});
	CHECK(f.app.waitUntil([&] { return f.elements("#quantize-hitpoint-tracks .quantize-priority").size() == 1; }));
	f.display.select({f.events[0], f.events[1]});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#quantize-hitpoint-tracks .quantize-priority").size() == 2; }));

	// At priority 0 the first track's hitpoints don't count: the second track's, 100 samples later, cut both.
	const std::string first =
		"#quantize-hitpoint-tracks .quantize-priority[track=\"" + std::to_string(f.tracks[0]) + "\"]";
	f.click(first, 2);
	f.app.ui().key(std::nullopt, "Mod+A");
	f.app.ui().text(std::nullopt, "0");
	f.app.ui().key(std::nullopt, "Enter");
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click("#quantize-slice");
	CHECK(f.positionsAre(f.tracks[0], {2000, 3100, 27100, 49100, 75100}));
	CHECK(f.positions(f.tracks[1]) == f.positions(f.tracks[0]));
	// The slices are selected now, and the first track keeps its priority.
	CHECK(f.app.waitUntil([&] { return priority(first) == 0.0; }));
}

TEST_CASE("a refused Slice in the quantize panel is logged", "[ui]")
{
	const test::TempDir folder;
	const std::filesystem::path file = folder.path() / "quantize.log";
	const logging::Session log(file);
	Fixture f;
	for (const uint64_t clip : f.clips)
		f.app.call("clip.setHitpoints", {{"clip", clip}, {"hitpoints", Json::array()}});
	f.display.select(f.events);
	f.openPanel();
	f.click("#quantize-slice");
	REQUIRE(f.app.waitUntil(
		[&] { return test::readText(file).find("Slicing audio events failed:") != std::string::npos; }));
	CHECK(test::readText(file).find("the events have no hitpoints that count") != std::string::npos);
}

TEST_CASE("the quantize panel slices audio parts and quantizes and crossfades every slice in them", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> parts =
		f.app.call("event.toPart", {{"events", f.events}}).at("events").get<std::vector<uint64_t>>();
	REQUIRE(parts.size() == 2);
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("audioParts").size() == 2; }));
	f.display.select(parts);
	f.openPanel();
	f.click("#quantize-slice");
	CHECK(f.positionsAre(f.tracks[0], {2000, 3000, 27000, 49000, 75000}));
	CHECK(f.positions(f.tracks[1]) == f.positions(f.tracks[0]));

	// The parts stay selected, so that Quantize and Crossfade go on with every slice in them.
	const size_t slices = f.project().at("audioEvents").size();
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("audioEvents").size() == slices; }));
	CHECK(f.app.waitUntil([&] { return f.display.selection() == parts; }));
	f.click("#quantize-apply");
	CHECK(f.positionsAre(f.tracks[0], {0, 0, 24000, 48000, 72000}));
	CHECK(f.positions(f.tracks[1]) == f.positions(f.tracks[0]));
	f.click("#quantize-crossfade");
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json now = f.project();
			return std::count_if(now.at("audioEvents").begin(), now.at("audioEvents").end(),
					   [](const Json& event) { return event.at("crossfade").is_object(); }) == 6;
		}));
}

TEST_CASE("the quantize panel creates warp markers on several audio tracks", "[ui]")
{
	Fixture f;
	f.display.select({f.events[0], f.events[1]});
	f.openPanel();
	f.click("#quantize-warp");
	f.click("#quantize-create-markers");
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json all = f.project();
			return std::all_of(all.at("clips").begin(), all.at("clips").end(),
				[](const Json& clip) { return clip.at("warp").at("markers").size() == 4; });
		}));
}
