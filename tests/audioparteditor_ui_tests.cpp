// SPDX-License-Identifier: GPL-3.0-or-later
// The audio part editor in the app (#146, docs/features/audio-part-editor.md), driven with ui::Automation as MCP's UI
// tools drive it: opening it, the part display with its lanes, events and waveforms, every tool, the info line and
// opening an event in the sample editor. tests/audioparteditor_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/audioparteditor.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/midieditor.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
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
			clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
			first = addEvent(0, 0);
			second = addEvent(g_second, g_second);
			part = app.call("event.toPart", {{"events", {first, second}}}).at("events").at(0).get<uint64_t>();
			app.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", g_second / 2}});
			// Two steps of the shell: the project window's first frames, which take a quarter second each in a Debug
			// build and several seconds on a machine busy compiling.
			REQUIRE(app.waitUntil([&]
				{ return display.project().is_object() && display.project().at("audioParts").size() == 1; },
				std::chrono::seconds(30)));
		}

		// Half a second of the clip from offset on.
		uint64_t addEvent(int64_t position, int64_t offset)
		{
			return app
				.call("audioEvent.add",
					{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset},
						{"length", g_second / 2}})
				.at("event")
				.at("id")
				.get<uint64_t>();
		}

		// A second audio part, from 3 to 3.5 seconds, with the event third.
		void addOtherPart()
		{
			third = addEvent(3 * g_second, 0);
			other = app.call("event.toPart", {{"events", {third}}}).at("events").at(0).get<uint64_t>();
			REQUIRE(app.waitUntil([&] { return display.project().at("audioParts").size() == 2; }));
		}

		void open()
		{
			openParts({part});
			settle();
		}

		// Open/Close Editor with the parts selected (p. 692).
		void openParts(const std::vector<uint64_t>& parts)
		{
			display.select(parts);
			app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
			// Opening a window takes several seconds in a Debug build on a busy machine.
			REQUIRE(app.waitUntil([&]
				{ return app.appUi().editorWindow() != nullptr && app.appUi().editorWindow()->audioPartEditor(); },
				std::chrono::seconds(30)));
			window = app.appUi().editorWindowId();
		}

		// Only the other part in the part editing mode; waits until the part's events show that.
		void editOtherPartOnly()
		{
			app.call("editor.set", {{"editor", "window"}, {"activePart", other}, {"editingMode", "activePart"}});
			REQUIRE(app.waitUntil([&] { return has(eventSelector(first), "inactive"); }));
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

		// Waits until the engine has these events selected and the editor shows them selected. A click with Shift or
		// Ctrl starts from the selection the editor shows, which follows the engine's a step or more later: an
		// answer to an earlier editor.get can show the older selection until then (#951).
		void selects(std::vector<uint64_t> events)
		{
			std::sort(events.begin(), events.end());
			REQUIRE(app.waitUntil(
				[&]
				{
					std::vector<uint64_t> engine = selection();
					std::sort(engine.begin(), engine.end());
					return engine == events;
				}));
			REQUIRE(app.waitUntil(
				[&]
				{
					std::vector<uint64_t> shown;
					for (const Json& each : elements(".ape-event.selected"))
						shown.push_back(std::stoull(each.at("attributes").at("event").get<std::string>()));
					std::sort(shown.begin(), shown.end());
					return shown == events;
				}));
		}

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

		const ui::AudioPartEditor* audioPartEditor()
		{
			ui::MidiEditor* shown = app.appUi().editorWindow();
			return shown != nullptr ? shown->audioPartEditor() : nullptr;
		}

		// Waits until the editor has the part's events as the engine has them and the display shows them so, each at
		// its time and lane, and one lane more than they are on. A drag already shows the events where it moves them
		// before the editor has the engine's project, and any render until then puts them back. The display shows the
		// part, and events after its end that it hides entirely up to theirs.
		void settle()
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json shown = box(".ape-display");
					const Json all = events();
					const ui::AudioPartEditor* editor = audioPartEditor();
					if (editor == nullptr || !editor->project().contains("audioEvents"))
						return false;
					const Json& known = editor->project().at("audioEvents");
					if (!std::all_of(all.begin(), all.end(), [&](const Json& each)
							{ return std::find(known.begin(), known.end(), each) != known.end(); }))
						return false;
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

		// Turns snap off and waits until the audio part editor has shown the project that says so.
		void snapOff()
		{
			app.call("snap.set", {{"on", false}});
			REQUIRE(app.waitUntil(
				[&]
				{
					const ui::AudioPartEditor* editor = audioPartEditor();
					return editor != nullptr && editor->project().contains("snap") &&
						editor->project().at("snap").at("on") == false;
				}));
		}

		// Past the window's right edge, level with the first lane.
		ui::Target outside()
		{
			return {"", 0,
				ui::Point{static_cast<int>(std::lround(context().GetDimensions().x + 40.0)), pointOf(at(0.25, 0)).y}};
		}

		double shownX(uint64_t id) { return box(eventSelector(id)).at("x").get<double>(); }

		// Whether the event shows on the lane.
		bool onLane(uint64_t id, int lane)
		{
			return std::abs(box(eventSelector(id)).at("y").get<double>() -
					   box(".ape-lane[lane=\"" + std::to_string(lane) + "\"]").at("y").get<double>()) <= 1.5;
		}

		// The lane under a point of the display, of the first count, or -1.
		int laneUnder(const ui::Target& target, int count)
		{
			for (int lane = 0; lane < count; ++lane)
			{
				const Json row = box(".ape-lane[lane=\"" + std::to_string(lane) + "\"]");
				if (row.at("y").get<double>() <= pointOf(target).y &&
					pointOf(target).y < row.at("y").get<double>() + row.at("height").get<double>())
					return lane;
			}
			return -1;
		}

		// Waits until the audio part editor has a project in which the event is as found says.
		void knows(uint64_t id, const std::function<bool(const Json&)>& found)
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					const ui::AudioPartEditor* editor = audioPartEditor();
					if (editor == nullptr)
						return false;
					const Json known = editor->project().value("audioEvents", Json::array());
					return std::any_of(known.begin(), known.end(),
						[&](const Json& each) { return each.at("id") == id && found(each); });
				}));
		}

		// The middle of what the selector finds.
		ui::Point middle(const std::string& selector)
		{
			const Json found = box(selector);
			return {
				static_cast<int>(std::lround(found.at("x").get<double>() + (found.at("width").get<double>() / 2.0))),
				static_cast<int>(std::lround(found.at("y").get<double>() + (found.at("height").get<double>() / 2.0)))};
		}

		// A finger from start up by 80 px, in steps, and off the screen.
		void pan(ui::Point start)
		{
			const auto touch = [&](int up)
			{
				return Rml::TouchList{
					Rml::Touch{1, Rml::Vector2f(static_cast<float>(start.x), static_cast<float>(start.y - up))}};
			};
			context().ProcessTouchStart(touch(0), 0);
			for (int up = 20; up <= 80; up += 20)
				context().ProcessTouchMove(touch(up), 0);
			context().ProcessTouchEnd(touch(80), 0);
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
		uint64_t clip = 0;
		uint64_t first = 0;
		uint64_t second = 0;
		uint64_t third = 0; // addOtherPart()'s
		uint64_t part = 0;
		uint64_t other = 0; // addOtherPart()'s
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
	f.selects({f.first});
	REQUIRE(f.app.waitUntil([&] { return !f.has(".ape-info", "empty"); }));
	CHECK(f.value("start") == "1.1.1.0");
	CHECK(f.value("end") == "1.2.1.0"); // half a second at 120 BPM is a beat
	CHECK(f.value("length") == "0.1.0.0");
	f.click(f.at(1.25, 0), 1, {"shift"});
	f.selects({f.first, f.second});
	REQUIRE(f.app.waitUntil([&] { return f.has(".ape-info", "multiple"); }));
	f.click(f.at(1.75, 0));
	f.selects({});

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
	f.selects({f.second});
	// With Ctrl a drag moves across lanes only, and a click adds to the selection.
	f.drag(f.at(0.5, 0), f.at(0.9, 1), {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("lane") == 1; }));
	f.settle();
	CHECK(f.event(f.first).at("position") == g_second / 4);
	f.click(f.at(1.25, 1));
	f.selects({f.second});
	f.click(f.at(0.5, 1), 1, {"ctrl"});
	f.selects({f.first, f.second});

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
	f.selects({f.first});
	// Commands from the click's release come before this one.
	f.click(f.at(1.25, 0), 1, {"shift"});
	f.selects({f.first, f.second});
	CHECK(f.event(f.first).at("position") == offGrid);
}

TEST_CASE("a view the engine answered before a click leaves the click's selection in the audio part editor", "[ui]")
{
	Fixture f;
	f.open();
	// What a request sent before the click gets, e.g. the editor.get that follows a move's project.get.
	const Json project = f.app.call("project.get");
	const Json before = f.view();
	// Without a step of the shell, so that the click's editor.set hasn't been answered when that view comes.
	f.app.ui().click(f.window, f.at(1.25, 0), ui::MouseButton::Left, 1, {});
	f.app.appUi().editorWindow()->audioPartEditor()->show(project, before);
	CHECK(f.has(eventSelector(f.second), "selected"));
	// Ctrl and a click adds to the selection the click made, not to the older one.
	f.click(f.at(0.25, 0), 1, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));
}

TEST_CASE("the audio part editor shows a selection from the engine once a click's selection is answered", "[ui]")
{
	Fixture f;
	f.open();
	f.click(f.at(0.25, 0));
	f.selects({f.first});
	// Comes after the answer to the click's editor.set: results and events arrive in order.
	f.app.call("editor.set", {{"editor", "window"}, {"eventSelection", {f.second}}});
	f.selects({f.second});
	CHECK(!f.has(eventSelector(f.first), "selected"));
	f.app.call("editor.set", {{"editor", "window"}, {"eventSelection", Json::array()}});
	f.selects({});
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
	const ui::Target outside = f.outside();
	REQUIRE(pointOf(outside).x > shown.at("x").get<double>() + shown.at("width").get<double>());

	// A move released there moves the event to the time there, on the grid of sixteenths (an eighth of a second at
	// 120 BPM), in one undo step.
	const size_t before = f.steps();
	f.drag(f.at(0.25, 0), outside);
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	const double there = (pointOf(outside).x - shown.at("x").get<double>()) / shown.at("width").get<double>() * 2.0;
	CHECK(std::abs(f.event(f.first).at("position").get<double>() - ((there - 0.25) * g_second)) <= g_second / 16.0);
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

TEST_CASE("a finger pan over an empty spot of the audio part editor scrolls its lanes", "[ui]")
{
	// Tools whose press there starts no gesture. The split tool doesn't render on it either, so that the element
	// under the finger stays the press's.
	const std::string tool = GENERATE("objectSelection", "split");
	CAPTURE(tool);
	Fixture f;
	// An event far down: the lanes don't fit, and the display scrolls.
	f.app.call("event.set", {{"events", {f.second}}, {"lane", 30}});
	f.open();
	f.tool(tool);
	const double top = f.box(".ape-lane[lane=\"0\"]").at("y").get<double>();
	f.pan(pointOf(f.at(1.75, 1)));
	REQUIRE(f.app.waitUntil([&] { return f.box(".ape-lane[lane=\"0\"]").at("y").get<double>() <= top - 60.0; }));
}

TEST_CASE("a finger pan over an event outside the part editing mode scrolls the audio part editor's lanes", "[ui]")
{
	Fixture f;
	f.addOtherPart();
	// An event far down: the lanes don't fit, and the display scrolls.
	f.app.call("event.set", {{"events", {f.third}}, {"lane", 30}});
	f.openParts({f.part, f.other});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 3; }));
	f.editOtherPartOnly();
	const double top = f.box(".ape-lane[lane=\"0\"]").at("y").get<double>();
	// With the object selection tool, the press there deselects, which draws no event again: the element under the
	// finger stays the press's, and it mustn't be a drag element.
	f.pan(f.middle(eventSelector(f.first)));
	REQUIRE(f.app.waitUntil([&] { return f.box(".ape-lane[lane=\"0\"]").at("y").get<double>() <= top - 60.0; }));
}

TEST_CASE("a move of the audio part editor goes on when a command adds an event to the part during the drag", "[ui]")
{
	Fixture f;
	f.open();
	const size_t before = f.steps();
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// From MCP or Lua, with the button held: the events' elements change.
	f.app.call("event.split", {{"events", {f.second}}, {"positions", {(g_second * 5) / 4}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 3; }));
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// The drag's events still come, as the pressed event's element stayed.
	f.moveTo(f.at(1.27, 0));
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(1.0, 0)).x) <= 2.0);
	// And its end, where no element gets a mouseup.
	const Json shown = f.box(".ape-display");
	const ui::Target outside = f.outside();
	f.moveTo(outside);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	const double there = (pointOf(outside).x - shown.at("x").get<double>()) / shown.at("width").get<double>() * 2.0;
	CHECK(std::abs(f.event(f.first).at("position").get<double>() - ((there - 0.25) * g_second)) <= g_second / 16.0);
	f.settle();
	CHECK(f.steps() == before + 2); // the split and the move
}

TEST_CASE("a move of the audio part editor keeps its event under the mouse when the display's range changes", "[ui]")
{
	Fixture f;
	f.open();
	// The first event pressed a quarter second into it.
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// The other event out of the part, up to 3.5 seconds: the display reaches it, and shows the same seconds narrower.
	f.app.call("event.move", {{"events", {f.second}}, {"delta", 2 * g_second}});
	REQUIRE(f.app.waitUntil([&] { return f.has(eventSelector(f.second), "outside"); }));
	// The mouse hasn't moved, and is over 1.35 seconds now: the pressed point goes there, the event to 1.1, on the
	// grid 1.125, where a release would take it.
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(1.125, 0, 3.5)).x) <= 2.0);
	// The mouse over 1.6 seconds: the event to 1.35, on the grid 1.375. By the mouse's way on the display it would go
	// 1.16 seconds, on the grid 1.125.
	f.moveTo(f.at(1.6, 0, 3.5));
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(1.375, 0, 3.5)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	CHECK(f.event(f.first).at("position") == (g_second * 11) / 8);
	f.settle();
}

// The press is kept as an offset into the pressed event, so that nothing a command does to the event or to the
// display's range takes the pressed point away from the mouse (#1124).
TEST_CASE(
	"a move of the audio part editor keeps its pressed point under the mouse when a command moves its event", "[ui]")
{
	Fixture f;
	f.open();
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// From MCP or Lua, with the button held: the pressed event off the grid, to 0.075 seconds.
	f.app.call("event.move", {{"events", {f.first}}, {"delta", (g_second * 3) / 40}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const ui::AudioPartEditor* editor = f.audioPartEditor();
			if (editor == nullptr)
				return false;
			const Json known = editor->project().value("audioEvents", Json::array());
			return std::any_of(known.begin(), known.end(), [&](const Json& each)
				{ return each.at("id") == f.first && each.at("position") == (g_second * 3) / 40; });
		}));
	// The command's move doesn't carry the move along: the quarter second pressed is still under the mouse, the event
	// at 0.52 seconds, on the grid 0.5, where the release takes it.
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != (g_second * 3) / 40; }));
	CHECK(f.event(f.first).at("position") == g_second / 2);
	f.settle();
}

TEST_CASE("a move of the audio part editor keeps its pressed point under the mouse when the part moves with its events",
	"[ui]")
{
	Fixture f;
	f.open();
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// A second to the right, from MCP or Lua, with the button held: the events go with the part and the display's range
	// with them, so that nothing moves on the display.
	f.app.call("event.move", {{"events", {f.part}}, {"delta", g_second}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const ui::AudioPartEditor* editor = f.audioPartEditor();
			if (editor == nullptr)
				return false;
			const Json known = editor->project().value("audioEvents", Json::array());
			return std::any_of(known.begin(), known.end(),
				[&](const Json& each) { return each.at("id") == f.first && each.at("position") == g_second; });
		}));
	// The event at 1.5 seconds now, half a second from its part's start as before, under the mouse and not a second
	// further.
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != g_second; }));
	CHECK(f.event(f.first).at("position") == (g_second * 3) / 2);
}

TEST_CASE("a move of the audio part editor keeps its pressed point under the mouse when an undo moves the part", "[ui]")
{
	Fixture f;
	// The part from 1 to 3 seconds: the display starts at 1.
	f.app.call("event.move", {{"events", {f.part}}, {"delta", g_second}});
	f.openParts({f.part}); // not open(): settle() expects the display to start at 0
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.shownX(f.first) - pointOf(f.at(0.0, 0)).x) <= 1.5; }));
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// The part's move undone with the button held: the events go back a second, and the display's range with them.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const ui::AudioPartEditor* editor = f.audioPartEditor();
			if (editor == nullptr)
				return false;
			const Json known = editor->project().value("audioParts", Json::array());
			return std::any_of(
				known.begin(), known.end(), [&](const Json& each) { return each.at("position").get<int64_t>() == 0; });
		}));
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	CHECK(f.event(f.first).at("position") == g_second / 2);
}

TEST_CASE(
	"a move of the audio part editor keeps its pressed point under the mouse when the display's start changes", "[ui]")
{
	Fixture f;
	// The part from 1 to 3 seconds: the display starts at 1.
	f.app.call("event.move", {{"events", {f.part}}, {"delta", g_second}});
	f.openParts({f.part}); // not open(): settle() expects the display to start at 0
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.shownX(f.first) - pointOf(f.at(0.0, 0)).x) <= 1.5; }));
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0)); // the event at 1.52 seconds, on the grid 1.5
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// The part's start a second to the left, its events where they are: the display starts at 0 and shows 3 seconds.
	f.app.call("event.resize", {{"events", {f.part}}, {"edge", "start"}, {"delta", -g_second}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const ui::AudioPartEditor* editor = f.audioPartEditor();
			if (editor == nullptr)
				return false;
			const Json known = editor->project().value("audioParts", Json::array());
			return std::any_of(
				known.begin(), known.end(), [&](const Json& each) { return each.at("position").get<int64_t>() == 0; });
		}));
	// The mouse is over 1.155 seconds now: the pressed point goes there, the event to 0.905, on the grid 0.875.
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.875, 0, 3.0)).x) <= 2.0);
	// The mouse over 1.6 seconds: the event to 1.35, on the grid 1.375, where a release takes it.
	f.moveTo(f.at(1.6, 0, 3.0));
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(1.375, 0, 3.0)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != g_second; }));
	CHECK(f.event(f.first).at("position") == (g_second * 11) / 8);
}

// The lane under the mouse and the display's width come from the layout, which a scroll, a new lane's scrollbar and a
// resize change while the mouse stays (#1124).
TEST_CASE(
	"a wheel scroll during a Ctrl drag of the audio part editor shows the event on the lane under the mouse", "[ui]")
{
	Fixture f;
	// An event far down: the lanes don't fit, and the display scrolls.
	f.app.call("event.set", {{"events", {f.second}}, {"lane", 30}});
	f.open();
	const int ctrl = Rml::Input::KM_CTRL;
	const ui::Target mouse = f.at(0.25, 1);
	f.press(f.at(0.25, 0), ctrl);
	f.moveTo(mouse, ctrl);
	const double top = f.box(".ape-lane[lane=\"0\"]").at("y").get<double>();
	REQUIRE(std::abs(f.box(eventSelector(f.first)).at("y").get<double>() -
				f.box(".ape-lane[lane=\"1\"]").at("y").get<double>()) <= 1.5);
	// At once: by default RmlUi scrolls by the wheel over several frames, which would change the lane under the mouse
	// while the test looks.
	f.context().SetDefaultScrollBehavior(Rml::ScrollBehavior::Instant, 1.0F);
	f.context().ProcessMouseWheel(Rml::Vector2f(0.0F, 3.0F), 0);
	REQUIRE(f.box(".ape-lane[lane=\"0\"]").at("y").get<double>() <= top - 60.0);
	// The lane under the mouse now, which the mouse hasn't moved to.
	const int under = f.laneUnder(mouse, 31);
	REQUIRE(under > 1);
	CHECK(f.onLane(f.first, under));
	f.release(ctrl);
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("lane") == under; }));
	f.settle();
}

TEST_CASE(
	"a move of the audio part editor measures on the display's width once a new lane's scrollbar changes it", "[ui]")
{
	Fixture f;
	f.open();
	f.snapOff();
	// Near the display's right edge: measured on the old width, the move would be off by the mouse's share of the
	// width times the scrollbar's, several pixels here.
	const ui::Target mouse = f.at(1.77, 0);
	f.press(f.at(0.25, 0));
	f.moveTo(mouse);
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(1.52, 0)).x) <= 2.5);
	const double width = f.box(".ape-display").at("width").get<double>();
	// From MCP or Lua, with the button held: an event far down adds lanes and the body's scrollbar, which narrows the
	// display. The mouse hasn't moved, and is over another time now.
	f.app.call("event.set", {{"events", {f.second}}, {"lane", 30}});
	REQUIRE(f.app.waitUntil([&] { return f.box(".ape-display").at("width").get<double>() < width - 1.0; }));
	const Json shown = f.box(".ape-display");
	const double seconds = (pointOf(mouse).x - shown.at("x").get<double>()) / shown.at("width").get<double>() * 2.0;
	CHECK(std::abs(f.shownX(f.first) -
			  (shown.at("x").get<double>() + ((seconds - 0.25) / 2.0 * shown.at("width").get<double>()))) <= 1.5);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	CHECK(std::abs(f.event(f.first).at("position").get<double>() - ((seconds - 0.25) * g_second)) <= g_second / 50.0);
}

TEST_CASE("a move of the audio part editor measures on the display's width once the window's size changes", "[ui]")
{
	Fixture f;
	f.open();
	f.snapOff();
	const ui::Target mouse = f.at(0.77, 0);
	f.press(f.at(0.25, 0));
	f.moveTo(mouse);
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.52, 0)).x) <= 2.5);
	const double width = f.box(".ape-display").at("width").get<double>();
	// The window narrower, with the button held and the mouse where it was.
	const Rml::Vector2i size = f.context().GetDimensions();
	f.context().SetDimensions({size.x - 300, size.y});
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json shown = f.box(".ape-display");
	REQUIRE(shown.at("width").get<double>() < width - 1.0);
	const double seconds = (pointOf(mouse).x - shown.at("x").get<double>()) / shown.at("width").get<double>() * 2.0;
	CHECK(std::abs(f.shownX(f.first) -
			  (shown.at("x").get<double>() + ((seconds - 0.25) / 2.0 * shown.at("width").get<double>()))) <= 1.5);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	CHECK(std::abs(f.event(f.first).at("position").get<double>() - ((seconds - 0.25) * g_second)) <= g_second / 50.0);
}

TEST_CASE("a Ctrl drag of the audio part editor keeps its element in order and goes on with its event as it is", "[ui]")
{
	Fixture f;
	f.open();
	const int ctrl = Rml::Input::KM_CTRL;
	f.press(f.at(1.25, 0), ctrl);
	f.moveTo(f.at(1.25, 1), ctrl);
	// The events before the pressed one are made again, in the project's order around its element.
	f.app.call("event.split", {{"events", {f.first}}, {"positions", {g_second / 4}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 3; }));
	std::vector<std::string> shown;
	for (const Json& each : f.elements(".ape-event"))
		shown.push_back(each.at("attributes").at("event").get<std::string>());
	std::vector<std::string> project;
	for (const Json& each : f.events())
		project.push_back(each.at("id").dump());
	CHECK(shown == project);
	// A command takes the pressed event a lane down, to the lane under the mouse: it shows there, not a lane further.
	f.app.call("event.set", {{"events", {f.second}}, {"lane", 1}});
	f.knows(f.second, [](const Json& each) { return each.at("lane") == 1; });
	CHECK(f.onLane(f.second, 1));
	// The release leaves it there, and the selection as it is: it ends a drag, not a click.
	f.release(ctrl);
	f.settle();
	CHECK(f.event(f.second).at("lane") == 1);
	CHECK(f.event(f.second).at("position") == g_second);
	CHECK(f.selection().empty());
}

// The press is kept as a time in the pressed event's clip, so that a resize or a split of the event during the drag
// doesn't take the pressed point away from the mouse either (#1226).
TEST_CASE("a move of the audio part editor goes on when a command resizes or splits its event during the drag", "[ui]")
{
	const std::string how = GENERATE("normal sizing", "sizing moves contents", "split");
	CAPTURE(how);
	Fixture f;
	f.open();
	// The first event pressed a quarter second into it.
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// From MCP or Lua, with the button held: the pressed event's start an eighth of a second later, or the event split
	// there, before the pressed point.
	if (how == "split")
	{
		f.app.call("event.split", {{"events", {f.first}}, {"positions", {g_second / 8}}});
		REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 3; }));
	}
	else
	{
		f.app.call("event.resize",
			{{"events", {f.first}}, {"edge", "start"}, {"delta", g_second / 8},
				{"mode", how == "normal sizing" ? "normal" : "moveContents"}});
		f.knows(f.first, [](const Json& each) { return each.at("position") == g_second / 8; });
	}
	// Normal sizing leaves the content where it is: the pressed point is an eighth of a second into the event now, and
	// under the mouse it takes the start to 0.645 seconds, on the grid 0.625. Sizing that moves the contents moves the
	// pressed point with the start, which goes to 0.5 as before; so does the event before the split, which the press
	// is past now.
	const double to = how == "normal sizing" ? 0.625 : 0.5;
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(to, 0)).x) <= 2.0);
	const Json before = f.event(f.first).at("position");
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != before; }));
	CHECK(f.event(f.first).at("position").get<double>() == to * g_second);
	f.settle();
}

TEST_CASE("a move of the audio part editor moves the other selected events as far as the pressed one", "[ui]")
{
	Fixture f;
	f.open();
	f.click(f.at(0.25, 0));
	f.selects({f.first});
	f.click(f.at(1.25, 0), 1, {"shift"});
	f.selects({f.first, f.second});
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	REQUIRE(std::abs(f.shownX(f.second) - pointOf(f.at(1.5, 0)).x) <= 2.0);
	// From MCP or Lua, with the button held: only the pressed event a quarter second later.
	f.app.call("event.move", {{"events", {f.first}}, {"delta", g_second / 4}});
	f.knows(f.first, [](const Json& each) { return each.at("position") == g_second / 4; });
	// The pressed event stays under the mouse, at 0.5 seconds: the move is a quarter second shorter, and the other
	// event goes back by as much, to 1.25 seconds.
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	CHECK(std::abs(f.shownX(f.second) - pointOf(f.at(1.25, 0)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("position") != g_second; }));
	CHECK(f.event(f.first).at("position") == g_second / 2);
	CHECK(f.event(f.second).at("position") == (g_second * 5) / 4);
	f.settle();
}

TEST_CASE(
	"a wheel scroll during a move of the audio part editor keeps its time and follows the lane under the mouse", "[ui]")
{
	Fixture f;
	// An event far down: the lanes don't fit, and the display scrolls.
	f.app.call("event.set", {{"events", {f.second}}, {"lane", 30}});
	f.open();
	const ui::Target mouse = f.at(0.77, 1);
	const double x = pointOf(f.at(0.5, 1)).x;
	f.press(f.at(0.25, 0));
	f.moveTo(mouse);
	const double top = f.box(".ape-lane[lane=\"0\"]").at("y").get<double>();
	REQUIRE(f.onLane(f.first, 1));
	REQUIRE(std::abs(f.shownX(f.first) - x) <= 2.0);
	// At once, as in the Ctrl drag's test.
	f.context().SetDefaultScrollBehavior(Rml::ScrollBehavior::Instant, 1.0F);
	f.context().ProcessMouseWheel(Rml::Vector2f(0.0F, 3.0F), 0);
	REQUIRE(f.box(".ape-lane[lane=\"0\"]").at("y").get<double>() <= top - 60.0);
	// The event on the lane under the mouse now, at the time it was at.
	const int under = f.laneUnder(mouse, 31);
	REQUIRE(under > 1);
	CHECK(f.onLane(f.first, under));
	CHECK(std::abs(f.shownX(f.first) - x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("lane") == under; }));
	CHECK(f.event(f.first).at("position") == g_second / 2);
	f.settle();
}

// On a musical track a tempo change moves the events in seconds and changes the display's scale with them.
TEST_CASE("a move of the audio part editor on a musical track keeps its pressed point under the mouse when the tempo "
		  "changes",
	"[ui]")
{
	Fixture f;
	// The events keep their time: the part takes four quarters, the second event starts at the third.
	f.app.call("track.set", {{"track", f.track}, {"timeBase", "musical"}});
	f.openParts({f.part}); // not open(): settle() reads the positions in samples
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return std::abs(f.shownX(f.first) - pointOf(f.at(0.0, 0)).x) <= 1.5 &&
				std::abs(f.shownX(f.second) - pointOf(f.at(1.0, 0)).x) <= 1.5;
		}));
	// The second event pressed a quarter second into it.
	f.press(f.at(1.25, 0));
	f.moveTo(f.at(1.77, 0));
	REQUIRE(std::abs(f.shownX(f.second) - pointOf(f.at(1.5, 0)).x) <= 2.0);
	// Half the tempo, with the button held: the part and the second event's start take twice the seconds, the display
	// shows four seconds, and the events keep their length in samples.
	f.app.call("project.set", {{"tempo", 60.0}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const ui::AudioPartEditor* editor = f.audioPartEditor();
			return editor != nullptr && editor->project().contains("tempo") &&
				editor->project().at("tempo").at(0).at("bpm") == 60.0;
		}));
	// The mouse hasn't moved, and is over 3.54 seconds now: the pressed point goes there, the event, which starts at 2
	// seconds now, to 3.29, on the grid's sixteenths, a quarter second at 60 BPM, to 3.25.
	CHECK(std::abs(f.shownX(f.second) - pointOf(f.at(3.25, 0, 4.0)).x) <= 2.0);
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.0, 0, 4.0)).x) <= 2.0);
	// The mouse over 2.5 seconds: the event to 2.25, where the release takes it, two quarters and a sixteenth.
	f.moveTo(f.at(2.5, 0, 4.0));
	CHECK(std::abs(f.shownX(f.second) - pointOf(f.at(2.25, 0, 4.0)).x) <= 2.0);
	const Json before = f.event(f.second).at("position");
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.second).at("position") != before; }));
	CHECK(f.event(f.second).at("position") == (gnubase::time::g_ticksPerQuarter * 9) / 4);
	CHECK(f.event(f.first).at("position") == 0);
}

TEST_CASE(
	"a move of the audio part editor keeps its pressed point under the mouse when its event's clip changes", "[ui]")
{
	Fixture f;
	f.open();
	// The first event pressed a quarter second into it.
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// From MCP or Lua, with the button held: the event plays another clip of the file from half a second on, where it
	// is, as when it is flattened into a new file.
	const auto other = f.app.call("clip.add", {{"media", f.app.call("project.get").at("clips").at(0).at("media")}})
						   .at("clip")
						   .at("id");
	f.app.call("audioEvent.set", {{"event", f.first}, {"clip", other}, {"offset", g_second / 2}});
	f.knows(f.first, [&](const Json& each) { return each.at("clip") == other; });
	// The press keeps its place in the event: the event stays at 0.5 seconds, and the release takes it there.
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	CHECK(f.event(f.first).at("position") == g_second / 2);
	f.settle();
}

TEST_CASE("a Ctrl drag of the audio part editor along its event's lane moves nothing and keeps the selection", "[ui]")
{
	Fixture f;
	f.open();
	const int ctrl = Rml::Input::KM_CTRL;
	const size_t before = f.steps();
	f.press(f.at(1.25, 0), ctrl);
	f.moveTo(f.at(1.6, 0), ctrl);
	f.release(ctrl);
	// Not a click: the event stays out of the selection (commands from the release come before this one), and nothing
	// moves.
	CHECK(f.selection().empty());
	CHECK(f.steps() == before);
	CHECK(f.event(f.second).at("position") == g_second);
	CHECK(f.event(f.second).at("lane") == 0);
}

TEST_CASE("a move of the audio part editor ends without its effect when its event goes during the drag", "[ui]")
{
	const std::string how = GENERATE("removed", "out of the part editing mode");
	CAPTURE(how);
	const bool removed = how == "removed";
	Fixture f;
	if (removed)
	{
		f.open();
	}
	else
	{
		f.addOtherPart();
		f.openParts({f.part, f.other});
		REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 3; }));
	}
	const double range = removed ? 2.0 : 3.5;
	f.click(f.at(0.25, 0, range));
	f.selects({f.first});
	f.click(f.at(1.25, 0, range), 1, {"shift"});
	f.selects({f.first, f.second});
	const size_t before = f.steps();
	f.press(f.at(0.25, 0, range));
	f.moveTo(f.at(0.77, 0, range));
	REQUIRE(std::abs(f.shownX(f.second) - pointOf(f.at(1.5, 0, range)).x) <= 2.0);

	if (removed)
	{
		f.app.call("event.remove", {{"events", {f.first}}});
		REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 1; }));
	}
	else
	{
		f.editOtherPartOnly();
	}
	// The other selected event goes back where it is, and the mouse no longer takes it along.
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.shownX(f.second) - pointOf(f.at(1.0, 0, range)).x) <= 2.0; }));
	f.moveTo(f.at(1.27, 0, range));
	f.release();
	CHECK(f.event(f.second).at("position") == g_second); // commands from the release come before this one
	CHECK(std::abs(f.shownX(f.second) - pointOf(f.at(1.0, 0, range)).x) <= 2.0);
	CHECK(f.steps() == before + (removed ? 1 : 0));
}

// The engine drops the events of a part that leaves the part editing mode from the selection, so a plain move has
// nothing left to move then; a Ctrl drag of an event that isn't selected moves the pressed event itself.
TEST_CASE("a Ctrl drag of the audio part editor moves nothing when its part leaves the part editing mode", "[ui]")
{
	Fixture f;
	f.addOtherPart();
	f.openParts({f.part, f.other});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".ape-event").size() == 3; }));
	REQUIRE(f.selection().empty());
	const int ctrl = Rml::Input::KM_CTRL;
	const double top = f.box(eventSelector(f.first)).at("y").get<double>();
	const size_t before = f.steps();
	f.press(f.at(0.25, 0, 3.5), ctrl);
	f.moveTo(f.at(0.25, 1, 3.5), ctrl);
	const double laneHeight = f.box(".ape-lane[lane=\"1\"]").at("y").get<double>() - top;
	REQUIRE(std::abs(f.box(eventSelector(f.first)).at("y").get<double>() - (top + laneHeight)) <= 1.5);
	// Only the other part in the part editing mode: the pressed event's part leaves it, as the event that isn't
	// pressed shows.
	f.app.call("editor.set", {{"editor", "window"}, {"activePart", f.other}, {"editingMode", "activePart"}});
	REQUIRE(f.app.waitUntil([&] { return f.has(eventSelector(f.second), "inactive"); }));
	// The event goes back to its lane, and the mouse no longer takes it along.
	REQUIRE(
		f.app.waitUntil([&] { return std::abs(f.box(eventSelector(f.first)).at("y").get<double>() - top) <= 1.5; }));
	f.moveTo(f.at(0.27, 1, 3.5), ctrl);
	f.release(ctrl);
	CHECK(f.event(f.first).at("lane") == 0); // commands from the release come before this one
	CHECK(std::abs(f.box(eventSelector(f.first)).at("y").get<double>() - top) <= 1.5);
	CHECK(f.steps() == before);
}

TEST_CASE("the audio part editor keeps showing a move while it renders during the drag", "[ui]")
{
	Fixture f;
	f.open();
	f.press(f.at(0.25, 0));
	f.moveTo(f.at(0.77, 0));
	REQUIRE(std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0);
	// The project shown again, whenever the press's selection came back, with snap off: the move shows off the grid
	// before the mouse moves again, the mouse's 0.52 seconds.
	f.snapOff();
	CHECK(std::abs(f.shownX(f.first) - pointOf(f.at(0.52, 0)).x) <= 2.5);
	// And back on the grid with snap on, and on a coarser one, of half notes (a second), with the grid changed.
	f.app.call("snap.set", {{"on", true}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.shownX(f.first) - pointOf(f.at(0.5, 0)).x) <= 2.0; }));
	f.app.call("quantize.set", {{"settings", {{"grid", "1/2"}}}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.shownX(f.first) - pointOf(f.at(1.0, 0)).x) <= 2.0; }));
	f.snapOff();
	const double shown = f.shownX(f.first);
	f.release();
	REQUIRE(f.app.waitUntil([&] { return f.event(f.first).at("position") != 0; }));
	// Where the move showed it: the mouse's way off the grid, within a pixel.
	const double perSecond = f.box(".ape-display").at("width").get<double>() / 2.0; // px
	CHECK(std::abs(f.event(f.first).at("position").get<double>() - (0.52 * g_second)) <= (1.5 * g_second) / perSecond);
	f.settle();
	CHECK(std::abs(f.shownX(f.first) - shown) <= 1.5);
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
	f.snapOff();
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
