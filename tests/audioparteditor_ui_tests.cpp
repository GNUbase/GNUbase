// SPDX-License-Identifier: GPL-3.0-or-later
// The audio part editor in the app (#146, docs/features/audio-part-editor.md), driven with ui::Automation as MCP's UI
// tools drive it: opening it, the part display with its lanes, events and waveforms, every tool, the info line and
// opening an event in the sample editor. tests/audioparteditor_tests.cpp checks the engine's side.
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_second = 48000;

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	std::string eventSelector(uint64_t id)
	{
		return ".ape-event[event=\"" + std::to_string(id) + "\"]";
	}

	// An audio part from 0 to 2 seconds on a linear audio track, with the events first (0 to 0.5 seconds) and second
	// (1 to 1.5 seconds) of a sine, selected in the project window. At 120 BPM the quantize grid's sixteenths are an
	// eighth of a second.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			test::Audio sine;
			for (int64_t frame = 0; frame < 2 * g_second; ++frame)
				sine.samples.push_back(static_cast<float>(0.5 * std::sin(static_cast<double>(frame) * 0.01)));
			test::writeWav(file, sine);
			track =
				app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<uint64_t>();
			const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media");
			const auto clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
			const auto add = [&](int64_t position)
			{
				return app
					.call("audioEvent.add",
						{{"track", track}, {"clip", clip}, {"position", position}, {"offset", position},
							{"length", g_second / 2}})
					.at("event")
					.at("id")
					.get<uint64_t>();
			};
			first = add(0);
			second = add(g_second);
			part = app.call("event.toPart", {{"events", {first, second}}}).at("events").at(0).get<uint64_t>();
			app.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", g_second / 2}});
			REQUIRE(app.waitUntil(
				[&] { return display.project().is_object() && display.project().at("audioParts").size() == 1; }));
		}

		// Open/Close Editor with the part selected (p. 692).
		void open()
		{
			display.select({part});
			app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([&]
				{ return app.appUi().editorWindow() != nullptr && app.appUi().editorWindow()->audioPartEditor(); }));
			window = app.appUi().editorWindowId();
			settle();
		}

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		std::string value(const std::string& field)
		{
			return elements(".ape-info textdisplay[field=" + field + "]").at(0).at("attributes").value("value", "");
		}

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json names = found.at(0).value("classes", Json::array());
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		Json view() { return app.call("editor.get", {{"editor", "window"}}); }

		std::vector<uint64_t> selection() { return view().at("eventSelection").get<std::vector<uint64_t>>(); }

		Json events()
		{
			Json found = Json::array();
			const Json project = app.call("project.get");
			for (const Json& each : project.at("audioEvents"))
				if (each.at("part") == part)
					found.push_back(each);
			return found;
		}

		Json event(uint64_t id)
		{
			for (const Json& each : events())
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		// Waits until the display shows the part's events as the engine has them, each at its time and lane, and one
		// lane more than they are on. A drag already shows the events where it moves them before the engine has them;
		// the lane that it adds shows only with the engine's.
		void settle()
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json shown = box(".ape-display");
					const Json all = events();
					if (elements(".ape-event").size() != all.size())
						return false;
					int lanes = 1;
					for (const Json& each : all)
						lanes = std::max(lanes, each.at("lane").get<int>() + 1);
					if (elements(".ape-lane").size() != static_cast<size_t>(lanes) + 1)
						return false;
					return std::all_of(all.begin(), all.end(),
						[&](const Json& each)
						{
							const Json found = elements(eventSelector(each.at("id").get<uint64_t>()));
							const Json lane = elements(".ape-lane[lane=\"" + each.at("lane").dump() + "\"]");
							if (found.empty() || lane.empty())
								return false;
							const double x = shown.at("x").get<double>() +
								(each.at("position").get<double>() / (2.0 * g_second) *
									shown.at("width").get<double>());
							return std::abs(found.at(0).at("box").at("x").get<double>() - x) <= 1.5 &&
								std::abs(found.at(0).at("box").at("y").get<double>() -
									lane.at(0).at("box").at("y").get<double>()) <= 1.5;
						});
				}));
		}

		// A point of the part display: seconds from the part's start, in the middle of the lane.
		ui::Target at(double seconds, int lane)
		{
			const Json shown = box(".ape-display");
			const Json row = box(".ape-lane[lane=\"" + std::to_string(lane) + "\"]");
			const double x = shown.at("x").get<double>() + (seconds / 2.0 * shown.at("width").get<double>());
			const double y = row.at("y").get<double>() + (row.at("height").get<double>() / 2.0);
			return {"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, int count = 1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void drag(const ui::Target& from, const ui::Target& to, const std::vector<std::string>& modifiers = {})
		{
			app.ui().drag(window, from, to, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void tool(const std::string& name)
		{
			click(of(".ape-tool[ape-tool=" + name + "]"));
			REQUIRE(has(".ape-tool[ape-tool=" + name + "]", "checked"));
		}

		test::UiApp app{{1200, 800}};
		ui::EventDisplay& display;
		test::TempDir dir;
		std::filesystem::path file = dir.path() / "sine.wav";
		ui::WindowId window = 0;
		uint64_t track = 0;
		uint64_t first = 0;
		uint64_t second = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("Open/Close Editor opens selected audio parts in the audio part editor", "[ui]")
{
	Fixture f;
	f.open();
	CHECK(f.view().at("content") == "audioPart");
	CHECK(f.view().at("parts") == Json::array({f.part}));
	// The events at their time on their lane, with one more lane to move them onto, and a waveform each.
	CHECK(f.elements(".ape-lane").size() == 2);
	const Json shown = f.box(".ape-display");
	const Json later = f.box(eventSelector(f.second));
	CHECK(std::abs(later.at("x").get<double>() - shown.at("x").get<double>() - (shown.at("width").get<double>() / 2)) <=
		1.5);
	CHECK(std::abs(later.at("width").get<double>() - (shown.at("width").get<double>() / 4)) <= 1.5);
	REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-waveforms waveform").size() == 2; }));
	CHECK(f.elements(".editor-part").size() == 1);
	CHECK(f.has(".ape-info", "empty"));

	// Closed, the Audio menu's Open Audio Part Editor opens it again (p. 692), and the lower zone's editor takes the
	// selected audio parts too.
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.keys().run("audio.openPartEditor", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	f.app.keys().run("window.lowerZoneEditor", Json::object(), f.app.main(), nullptr);
	REQUIRE(
		f.app.waitUntil([&] { return f.app.call("editor.get", {{"editor", "lower"}}).at("content") == "audioPart"; }));
}

TEST_CASE("the quantize commands are disabled in the audio part editor as it has no notes", "[ui]")
{
	Fixture f;
	f.open();
	f.app.keys().run("window.lowerZoneEditor", Json::object(), f.app.main(), nullptr);
	REQUIRE(
		f.app.waitUntil([&] { return f.app.call("editor.get", {{"editor", "lower"}}).at("content") == "audioPart"; }));
	ui::Window* window = f.app.shell().find(f.window);
	REQUIRE(window != nullptr);
	Rml::Element* editorZone = window->context().GetDocument(0)->GetElementById("main-zone");
	Rml::Element* lowerZone = f.app.main().context().GetDocument(0)->GetElementById("lower-zone");
	REQUIRE(editorZone != nullptr);
	REQUIRE(lowerZone != nullptr);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json shown = f.app.ui().dom(std::nullopt, "#lower-zone .ape-display", 0).at("elements");
			return !shown.empty() && shown[0].at("visible").get<bool>();
		}));
	for (const char* command : {"edit.quantize", "edit.resetQuantize", "edit.createGrooveQuantize"})
	{
		INFO(command);
		CHECK(!f.app.keys().canRun(command, *window, editorZone));
		CHECK(!f.app.keys().canRun(command, f.app.main(), lowerZone));
	}
	// Q there sends nothing, and the events stay where they are.
	const Json before = f.app.call("project.get").at("audioParts");
	f.app.ui().key(f.window, "Q");
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.app.call("project.get").at("audioParts") == before);
}

TEST_CASE("the object selection tool selects events and moves them in time and across lanes", "[ui]")
{
	Fixture f;
	f.open();
	// A click selects, Shift and a click adds, a click in the empty display deselects; the info line shows the first.
	f.click(f.at(0.25, 0));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{f.first}; }));
	REQUIRE(f.app.waitUntil([&] { return !f.has(".ape-info", "empty"); }));
	CHECK(f.value("start") == "1.1.1.0");
	CHECK(f.has(eventSelector(f.first), "selected"));
	f.click(f.at(1.25, 0), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".ape-info", "multiple"); }));
	f.click(f.at(1.75, 0));
	REQUIRE(f.app.waitUntil([&] { return f.selection().empty(); }));

	// A drag moves the event, snapped to the quantize grid.
	f.drag(f.at(0.25, 0), f.at(0.52, 0));
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") == g_second / 4; }));
	f.settle();
	// Down onto the next lane (p. 699), which makes another lane to move onto.
	f.drag(f.at(1.25, 0), f.at(1.25, 1));
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("lane") == 1; }));
	f.settle();
	CHECK(f.event(f.second).at("position") == g_second);
	CHECK(f.elements(".ape-lane").size() == 3);
	// With Ctrl a drag moves across lanes only, and a click adds to the selection.
	f.drag(f.at(0.5, 0), f.at(0.9, 1), {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("lane") == 1; }));
	f.settle();
	CHECK(f.event(f.first).at("position") == g_second / 4);
	f.click(f.at(1.25, 1));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{f.second}; }));
	f.click(f.at(0.5, 1), 1, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));

	// A double click on an event opens it in the sample editor.
	f.click(f.at(1.25, 1), 2);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("sampleEditor.get").at("open") == true; }));
	CHECK(f.app.call("sampleEditor.get").at("event") == f.second);
}

TEST_CASE("a click with the object selection tool selects an event off the grid without moving it", "[ui]")
{
	Fixture f;
	const int64_t offGrid = (g_second * 3) / 10; // 0.3 seconds, between sixteenths
	f.app.call("event.move", {{"events", {f.first}}, {"delta", offGrid}});
	f.open();
	f.click(f.at(0.4, 0));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{f.first}; }));
	// Commands from the click's release come before this one.
	f.click(f.at(1.25, 0), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));
	CHECK(f.event(f.first).at("position") == offGrid);
}

TEST_CASE("the split, glue, mute and erase tools edit the events in the part", "[ui]")
{
	Fixture f;
	f.open();
	f.tool("split");
	f.click(f.at(0.27, 0));
	REQUIRE(f.app.waitUntil([&] { return f.events().size() == 3; }));
	f.settle();
	CHECK(f.event(f.first).at("length") == g_second / 4); // split on the grid at a quarter second

	f.tool("glue");
	f.click(f.at(0.1, 0));
	REQUIRE(f.app.waitUntil([&] { return f.events().size() == 2; }));
	f.settle();
	CHECK(f.event(f.first).at("length") == g_second / 2);

	f.tool("mute");
	f.click(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("mute") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(eventSelector(f.second), "muted"); }));

	f.tool("erase");
	f.click(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil([&] { return f.events().size() == 1; }));
	f.settle();
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.events().size() == 2; }));
	f.settle();
}

TEST_CASE("the play and scrub tools audition the part's track", "[ui]")
{
	Fixture f;
	f.open();
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	std::vector<uint64_t> tracks;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&](const engine::Event& event)
		{
			modes.push_back(event.data.at("mode").get<std::string>());
			tracks.push_back(event.data.at("track").get<uint64_t>());
		});
	f.tool("play");
	f.click(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play", "off"};
		}));
	CHECK(tracks.front() == f.track);
	f.tool("scrub");
	f.drag(f.at(0.25, 0), f.at(0.4, 0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes.size() >= 4 && modes[2] == "scrub" && modes.back() == "off";
		}));
}

TEST_CASE("the info line edits the selected events", "[ui]")
{
	Fixture f;
	f.open();
	f.click(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil([&] { return f.value("start") == "1.3.1.0"; }));
	// Typing a start moves the event, a volume sets it.
	f.click(of(".ape-info textdisplay[field=start]"), 2);
	f.app.ui().key(f.window, "Mod+A");
	f.app.ui().text(f.window, "1.4.1.0");
	f.app.ui().key(f.window, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("position") == 3 * g_second / 2; }));
	f.settle();
	f.click(of(".ape-info textdisplay[field=volume]"), 2);
	f.app.ui().key(f.window, "Mod+A");
	f.app.ui().text(f.window, "-6");
	f.app.ui().key(f.window, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("volume") == -6.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.value("volume") == "-6.00"; }));
}
