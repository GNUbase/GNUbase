// SPDX-License-Identifier: GPL-3.0-or-later
// The audio part editor in the app (#146, docs/features/audio-part-editor.md), driven with ui::Automation as MCP's UI
// tools drive it: opening it, the part display with its lanes, events and waveforms, every tool, the info line and
// opening an event in the sample editor. tests/audioparteditor_tests.cpp checks the engine's side.
#include "ui/audioparteditor.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/midieditor.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
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

	ui::Point pointOf(const ui::Target& target)
	{
		return target.point.value_or(ui::Point{});
	}

	std::string eventSelector(uint64_t id)
	{
		return ".ape-event[event=\"" + std::to_string(id) + "\"]";
	}

	// An audio part from 0 to 2 seconds on a linear audio track, with the events first (0 to 0.5 seconds) and second
	// (1 to 1.5 seconds) of a sine, selected in the project window. Snap is on: at 120 BPM the quantize grid's
	// sixteenths are an eighth of a second.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			app.call("snap.set", {{"on", true}});
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
			// Opening a window takes several seconds in a Debug build on a busy machine.
			REQUIRE(app.waitUntil([&]
				{ return app.appUi().editorWindow() != nullptr && app.appUi().editorWindow()->audioPartEditor(); },
				std::chrono::seconds(30)));
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
		// the lane that it adds shows only with the engine's. The display shows the part, and events after its end
		// that it hides entirely up to theirs.
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
					double range = 2.0;
					for (const Json& each : all)
					{
						lanes = std::max(lanes, each.at("lane").get<int>() + 1);
						if (each.at("position").get<double>() >= 2.0 * g_second)
							range = std::max(range,
								(each.at("position").get<double>() + each.at("length").get<double>()) / g_second);
					}
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
								(each.at("position").get<double>() / (range * g_second) *
									shown.at("width").get<double>());
							return std::abs(found.at(0).at("box").at("x").get<double>() - x) <= 1.5 &&
								std::abs(found.at(0).at("box").at("y").get<double>() -
									lane.at(0).at("box").at("y").get<double>()) <= 1.5;
						});
				}));
		}

		// A point of the part display: seconds from the part's start, in the middle of the lane, with the display
		// showing range seconds.
		ui::Target at(double seconds, int lane, double range = 2.0)
		{
			const Json shown = box(".ape-display");
			const Json row = box(".ape-lane[lane=\"" + std::to_string(lane) + "\"]");
			const double x = shown.at("x").get<double>() + (seconds / range * shown.at("width").get<double>());
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

		// The editor window's context: through it a test presses a button and keeps it down, as a mouseup lost
		// outside the window leaves it, where RmlUi has no element to send the mouseup to.
		Rml::Context& context()
		{
			const ui::Window* shown = app.shell().find(window);
			if (shown == nullptr)
				throw std::logic_error("the editor window doesn't show");
			return shown->context();
		}

		void press(const ui::Target& target, int modifiers = 0)
		{
			context().ProcessMouseMove(pointOf(target).x, pointOf(target).y, modifiers);
			context().ProcessMouseButtonDown(0, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void moveTo(const ui::Target& target, int modifiers = 0)
		{
			context().ProcessMouseMove(pointOf(target).x, pointOf(target).y, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void release(int modifiers = 0)
		{
			context().ProcessMouseButtonUp(0, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// A point on the editor window's toolbar, above the audio part editor, at the x of a point of the display.
		ui::Target toolbar(const ui::Target& below)
		{
			const Json shown = box(".midi-editor-toolbar");
			const auto y =
				static_cast<int>(std::lround(shown.at("y").get<double>() + (shown.at("height").get<double>() / 2.0)));
			return {"", 0, ui::Point{pointOf(below).x, y}};
		}

		// The toolbar's empty space, where a click does nothing.
		ui::Target spare()
		{
			const Json shown = box(".midi-editor-toolbar .spacer");
			return {"", 0,
				ui::Point{
					static_cast<int>(std::lround(shown.at("x").get<double>() + shown.at("width").get<double>() - 5)),
					pointOf(toolbar(at(0.0, 0))).y}};
		}

		size_t steps() { return app.call("history.get").at("steps").size(); }

		// The commands that ran, oldest first (engine.commandLog), by name.
		Json ran(const std::string& command)
		{
			Json found = Json::array();
			const Json log = app.call("engine.commandLog");
			for (const Json& each : log.at("commands"))
				if (each.at("command") == command)
					found.push_back(each);
			return found;
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
	const auto keys = f.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
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
	// The Visibility tab lists the audio track, whose part shows, and not the MIDI track.
	const std::string row = ".editor-visibility-track[track=\"" + std::to_string(f.track) + "\"]";
	REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-visibility-track").size() == 1; }));
	CHECK(f.has(row + " .editor-visibility-check", "checked"));
	CHECK(f.elements(".editor-visibility-track[track=\"" + std::to_string(keys) + "\"]").empty());

	// Closed, the Audio menu's Open Audio Part Editor opens it again (p. 692), and the lower zone's editor takes the
	// selected audio parts too.
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.keys().run("audio.openPartEditor", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	f.app.keys().run("window.lowerZoneEditor", Json::object(), f.app.main(), nullptr);
	REQUIRE(
		f.app.waitUntil([&] { return f.app.call("editor.get", {{"editor", "lower"}}).at("content") == "audioPart"; }));
	// The lower zone shows the audio part editor with the part's events.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json shown = f.app.ui().dom(std::nullopt, "#lower-zone .ape-event", 0).at("elements");
			return shown.size() == 2 && shown[0].at("visible").get<bool>() && shown[1].at("visible").get<bool>();
		}));
}

TEST_CASE("an open audio part editor follows the audio parts selected in the project window", "[ui]")
{
	Fixture f;
	f.open();
	// Another part on the track, from 3 to 4 seconds: selected in the project window, the editor shows it instead.
	const auto clip = f.event(f.first).at("clip").get<uint64_t>();
	const auto event = f.app
						   .call("audioEvent.add",
							   {{"track", f.track}, {"clip", clip}, {"position", 3 * g_second}, {"length", g_second}})
						   .at("event")
						   .at("id")
						   .get<uint64_t>();
	const auto other = f.app.call("event.toPart", {{"events", {event}}}).at("events").at(0).get<uint64_t>();
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("audioParts").size() == 2; }));
	f.display.select({other});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("parts") == Json::array({other}); }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 1; }));
	CHECK(f.elements(eventSelector(event)).size() == 1);
	// With Keep Editor Contents it stays with what it shows, while the lower zone's editor follows.
	f.app.call("editor.set", {{"editor", "window"}, {"keepContents", true}});
	f.app.keys().run("window.lowerZoneEditor", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil(
		[&] { return f.app.call("editor.get", {{"editor", "lower"}}).at("parts") == Json::array({other}); }));
	f.display.select({f.part});
	REQUIRE(f.app.waitUntil(
		[&] { return f.app.call("editor.get", {{"editor", "lower"}}).at("parts") == Json::array({f.part}); }));
	CHECK(f.view().at("parts") == Json::array({other}));
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
	CHECK(f.value("end") == "1.2.1.0"); // half a second at 120 BPM is a beat
	CHECK(f.value("length") == "0.1.0.0");
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
	// The lanes are the part's own: the project window's Show Lanes shows the track's lane the part is on, and no
	// lane for the event's lane in the part.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json events = f.display.project().at("audioEvents");
			return std::any_of(events.begin(), events.end(),
				[&](const Json& each) { return each.at("id") == f.second && each.at("lane") == 1; });
		}));
	f.display.showLanes(f.track, true);
	f.app.shell().step(std::chrono::milliseconds(0));
	std::vector<int> takes;
	for (size_t i = 0; i < f.display.laneTakes().size(); ++i)
		if (f.display.laneTracks()[i] == f.track)
			takes.push_back(f.display.laneTakes()[i]);
	CHECK(takes == std::vector<int>{-1, 0});
	f.display.showLanes(f.track, false);
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

	// The glue tool glues the second half to nothing: the next event on its lane doesn't continue it in the clip,
	// so it sends nothing the engine would refuse. The first half it glues to the second half again.
	f.tool("glue");
	const size_t before = f.steps();
	f.click(f.at(0.4, 0));
	f.settle();
	CHECK(f.ran("event.glue").empty());
	CHECK(f.events().size() == 3);
	CHECK(f.steps() == before);
	f.click(f.at(0.1, 0));
	REQUIRE(f.app.waitUntil([&] { return f.events().size() == 2; }));
	f.settle();
	CHECK(f.event(f.first).at("length") == g_second / 2);
	REQUIRE(f.ran("event.glue").size() == 1);
	CHECK(!f.ran("event.glue").at(0).contains("error"));

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
	// It plays up to the end of the part clicked (p. 700).
	const Json started = f.ran("audition.start");
	REQUIRE(started.size() == 1);
	CHECK(started.at(0).at("parameters").at("end") == 2 * g_second);
	CHECK(std::abs(started.at(0).at("parameters").at("position").get<double>() - (1.25 * g_second)) < g_second / 50.0);
	f.tool("scrub");
	f.drag(f.at(0.25, 0), f.at(0.4, 0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes.size() >= 4 && modes[2] == "scrub" && modes.back() == "off";
		}));
}

TEST_CASE("the play tool auditions the active part from an empty spot and nothing after a part's end", "[ui]")
{
	Fixture f;
	// The second event, from 3 to 3.5 seconds, is out of its part (0 to 2 seconds): the display reaches it.
	f.app.call("event.move", {{"events", {f.second}}, {"delta", 2 * g_second}});
	f.open();
	f.tool("play");
	// After the part's end, on the event out of it and in the empty display: nothing.
	f.click(f.at(3.25, 0, 3.5));
	f.click(f.at(2.5, 0, 3.5));
	// In the empty display within the part: the active part's track up to its end.
	f.click(f.at(1.5, 0, 3.5));
	REQUIRE(f.app.waitUntil([&] { return f.ran("audition.stop").size() == 1; }));
	const Json started = f.ran("audition.start");
	REQUIRE(started.size() == 1);
	CHECK(started.at(0).at("parameters").at("track") == f.track);
	CHECK(started.at(0).at("parameters").at("end") == 2 * g_second);
	CHECK(std::abs(started.at(0).at("parameters").at("position").get<double>() - (1.5 * g_second)) < g_second / 50.0);
}

TEST_CASE("the info line edits the selected events", "[ui]")
{
	Fixture f;
	f.open();
	f.click(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil([&] { return f.value("start") == "1.3.1.0"; }));
	// Typing a start moves the event, a volume sets it.
	f.click(of(".ape-info textdisplay[field=start]"), 2);
	f.app.ui().key(f.window, "Ctrl+A");
	f.app.ui().text(f.window, "1.4.1.0");
	f.app.ui().key(f.window, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("position") == 3 * g_second / 2; }));
	f.settle();
	f.click(of(".ape-info textdisplay[field=volume]"), 2);
	f.app.ui().key(f.window, "Ctrl+A");
	f.app.ui().text(f.window, "-6");
	f.app.ui().key(f.window, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("volume") == -6.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.value("volume") == "-6.00"; }));
}

TEST_CASE("a Ctrl drag of an event that isn't selected shows and moves only that event", "[ui]")
{
	Fixture f;
	f.open();
	f.click(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{f.second}; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(eventSelector(f.second), "selected"); }));
	const double top = f.box(eventSelector(f.second)).at("y").get<double>();
	// While it drags, the pressed event shows a lane further down, the selected one stays.
	f.press(f.at(0.25, 0), Rml::Input::KM_CTRL);
	f.moveTo(f.at(0.25, 1), Rml::Input::KM_CTRL);
	const double laneHeight = f.box(".ape-lane[lane=\"1\"]").at("y").get<double>() - top;
	CHECK(std::abs(f.box(eventSelector(f.first)).at("y").get<double>() - (top + laneHeight)) <= 1.5);
	CHECK(std::abs(f.box(eventSelector(f.second)).at("y").get<double>() - top) <= 1.5);
	f.release(Rml::Input::KM_CTRL);
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("lane") == 1; }));
	f.settle();
	CHECK(f.event(f.second).at("lane") == 0);
	CHECK(f.event(f.first).at("position") == 0);
}

TEST_CASE("a gesture released outside the audio part editor ends there", "[ui]")
{
	Fixture f;
	f.open();
	// A move released over the editor window's toolbar moves the event to the time there; a later click in the
	// display moves nothing.
	const size_t before = f.steps();
	f.drag(f.at(0.25, 0), f.toolbar(f.at(0.52, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") == g_second / 4; }));
	f.settle();
	CHECK(f.steps() == before + 1);
	f.click(f.at(1.75, 0));
	f.settle();
	CHECK(f.event(f.first).at("position") == g_second / 4);
	CHECK(f.event(f.second).at("position") == g_second);
	CHECK(f.steps() == before + 1);

	// The play tool auditions until the mouse goes up over the toolbar.
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&](const engine::Event& event) { modes.push_back(event.data.at("mode").get<std::string>()); });
	f.tool("play");
	f.drag(f.at(1.25, 0), f.toolbar(f.at(1.25, 0)));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play", "off"};
		}));
}

// The next test loses a mouseup, as when the mouse goes up outside the window: it presses through the editor window's
// context and never releases the button.
TEST_CASE("a gesture of the audio part editor ends when the button goes up outside the window", "[ui]")
{
	Fixture f;
	f.open();
	const Json shown = f.box(".ape-display");
	// Past the window's right edge, level with the first lane.
	const ui::Target outside{"", 0,
		ui::Point{static_cast<int>(std::lround(f.context().GetDimensions().x + 40.0)), pointOf(f.at(0.25, 0)).y}};
	REQUIRE(pointOf(outside).x > shown.at("x").get<double>() + shown.at("width").get<double>());

	// A move released there moves the event later than the part's end, in one undo step.
	const size_t before = f.steps();
	f.drag(f.at(0.25, 0), outside);
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position").get<int64_t>() > 2 * g_second; }));
	f.settle();
	CHECK(f.event(f.first).at("lane") == 0);
	CHECK(f.steps() == before + 1);
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") == 0; }));
	f.settle();

	// The play and the scrub tool each stop auditioning there.
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&](const engine::Event& event) { modes.push_back(event.data.at("mode").get<std::string>()); });
	f.tool("play");
	f.drag(f.at(1.25, 0), outside);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play", "off"};
		}));
	f.tool("scrub");
	f.drag(f.at(0.25, 0), outside);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes.size() >= 4 && modes[2] == "scrub" && modes.back() == "off";
		}));
	// The mouse over the display afterwards, with no button held, scrubs nothing.
	const size_t scrubs = f.ran("audition.scrub").size();
	f.app.ui().move(f.window, f.at(0.5, 0));
	f.app.ui().move(f.window, f.at(1.0, 0));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.ran("audition.scrub").size() == scrubs);
	binding.update();
	CHECK(modes.back() == "off");
}

TEST_CASE("a press in the window ends a gesture of the audio part editor whose mouseup never came", "[ui]")
{
	Fixture f;
	f.open();
	const size_t before = f.steps();
	// A move: a click on the toolbar's empty space, and one in the display, move nothing. The click on the toolbar
	// already puts the event back where it is, from where the move showed it.
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 1));
	REQUIRE(std::abs(f.box(eventSelector(f.first)).at("x").get<double>() - pointOf(f.at(0.5, 0)).x) <= 2.0);
	f.click(f.spare());
	f.settle();
	f.click(f.at(1.75, 0));
	f.settle();
	CHECK(f.event(f.first).at("position") == 0);
	CHECK(f.event(f.first).at("lane") == 0);
	CHECK(f.steps() == before);

	// The play tool's audition ends with the next press.
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&](const engine::Event& event) { modes.push_back(event.data.at("mode").get<std::string>()); });
	f.tool("play");
	f.press(f.at(1.25, 0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play"};
		}));
	f.click(f.spare());
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play", "off"};
		}));
}

TEST_CASE("events moved out of their part show where they are and move back into it", "[ui]")
{
	Fixture f;
	// The second event, from 3 to 3.5 seconds, is out of its part (0 to 2 seconds): the display reaches it.
	f.app.call("event.move", {{"events", {f.second}}, {"delta", 2 * g_second}});
	f.open();
	REQUIRE(f.app.waitUntil([&] { return f.elements(eventSelector(f.second)).size() == 1; }));
	CHECK(f.has(eventSelector(f.second), "outside"));
	CHECK(!f.has(eventSelector(f.first), "outside"));
	const Json shown = f.box(".ape-display");
	const Json outside = f.box(eventSelector(f.second));
	CHECK(std::abs(outside.at("x").get<double>() + outside.at("width").get<double>() - shown.at("x").get<double>() -
			  shown.at("width").get<double>()) <= 1.5);
	// It selects and drags back into the part.
	f.click(f.at(3.25, 0, 3.5));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{f.second}; }));
	// Away from the click, so that the press isn't a double click.
	f.drag(f.at(3.4, 0, 3.5), f.at(1.4, 0, 3.5));
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("position") == g_second; }));
	f.settle();
	CHECK(!f.has(eventSelector(f.second), "outside"));
}

TEST_CASE("with snap off the audio part editor moves and splits events where the mouse is", "[ui]")
{
	Fixture f;
	f.open();
	// Turned off while the editor is open.
	f.app.call("snap.set", {{"on", false}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			ui::MidiEditor* window = f.app.appUi().editorWindow();
			const ui::AudioPartEditor* editor = window != nullptr ? window->audioPartEditor() : nullptr;
			return editor != nullptr && editor->project().contains("snap") &&
				editor->project().at("snap").at("on") == false;
		}));
	// 0.27 seconds is off the grid of eighths of a second; a pixel of the display is under 2 ms.
	f.drag(f.at(0.25, 0), f.at(0.52, 0));
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	const auto moved = f.event(f.first).at("position").get<int64_t>();
	CHECK(std::abs(static_cast<double>(moved) - (0.27 * g_second)) < g_second / 200.0);
	f.tool("split");
	f.click(f.at(1.3, 0));
	REQUIRE(f.app.waitUntil([&] { return f.events().size() == 3; }));
	const auto length = f.event(f.second).at("length").get<int64_t>();
	CHECK(std::abs(static_cast<double>(length) - (0.3 * g_second)) < g_second / 200.0);
}
