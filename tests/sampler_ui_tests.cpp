// SPDX-License-Identifier: GPL-3.0-or-later
// The sampler panel and Add Sampler Track in the app (#1029, docs/features/sampler-track.md), driven with
// ui::Automation as MCP's UI tools drive it: the panel follows the selected sampler track, its controls, markers and
// keyboard set the sampler's parameters, a dropped file loads into it, Normalize Sample sets its gain, and the add
// track dialog's Sampler page adds sampler tracks. tests/sampler_tests.cpp checks the engine's side.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/shell.h"
#include "ui/tracklist.h"

#include <SDL3/SDL.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::WithinAbs;
using engine::Json;

namespace
{
	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	// A project with a sampler track that plays a mono sine, half a second at 48 kHz at half level, and an audio
	// track, the project window shown.
	struct Fixture
	{
		Fixture()
		{
			app.appUi().showProject();
			test::Audio sine;
			for (int frame = 0; frame < 24000; ++frame)
				sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 0.05)));
			test::writeWav(file, sine);
			test::writeWav(other, sine);
			audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
			const Json created = app.call("sampler.create", {{"path", file.u8string()}});
			track = created.at("track").at("id").get<uint64_t>();
			channel = created.at("channel").at("id").get<uint64_t>();
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.size() != 1)
				return false;
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector)
		{
			app.ui().click(std::nullopt, of(selector), ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void select(uint64_t id)
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					app.appUi().trackList()->select({id});
					return app.appUi().trackList()->selection() == std::vector<uint64_t>{id};
				}));
		}

		// Shows the sampler panel with the sampler track selected.
		void open()
		{
			select(track);
			app.keys().run("window.sampler", Json::object(), app.main(), nullptr);
			REQUIRE(
				app.waitUntil([&] { return !elements("#sampler-panel").empty() && !has("#sampler-panel", "empty"); }));
			REQUIRE(app.waitUntil([&] { return !elements("#sampler-panel [parameter]").empty(); }));
		}

		Json samplerChannel()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("channels"))
				if (each.at("id") == channel)
					return each;
			return {};
		}

		// The sampler's parameter, by processor.list's name: its value or default.
		double parameter(const std::string& name)
		{
			const Json processors = app.call("processor.list");
			for (const Json& processor : processors.at("processors"))
				if (processor.at("id") == "sampler")
					for (const Json& each : processor.at("parameters"))
						if (each.at("name") == name)
						{
							const Json values = samplerChannel().at("instrumentParameters");
							for (const Json& item : values)
								if (item.at("parameter") == each.at("id"))
									return item.at("value").get<double>();
							return each.at("default").get<double>();
						}
			return -1.0;
		}

		int id(const std::string& name)
		{
			const Json processors = app.call("processor.list");
			for (const Json& processor : processors.at("processors"))
				if (processor.at("id") == "sampler")
					for (const Json& each : processor.at("parameters"))
						if (each.at("name") == name)
							return each.at("id").get<int>();
			return -1;
		}

		std::string control(const std::string& name) { return "#sampler-parameter-" + std::to_string(id(name)); }

		void set(const std::string& name, double value)
		{
			app.call("parameter.set",
				{{"channel", channel}, {"target", {{"type", "instrument"}, {"parameter", id(name)}}},
					{"value", value}});
		}

		test::TempDir folder;
		std::filesystem::path file = folder.path() / "sine.wav";
		std::filesystem::path other = folder.path() / "other.wav";
		test::UiApp app{{1200, 800}};
		uint64_t audio = 0;
		uint64_t track = 0;
		uint64_t channel = 0;
	};
}

TEST_CASE("the sampler panel shows the selected sampler track and sets its parameters", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("window.sampler", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-panel", "empty"); }));
	f.open();
	CHECK(f.elements("#sampler-file").at(0).value("text", "") == "sine");
	// Every parameter has a control, in its section.
	CHECK(f.elements("#sampler-panel [parameter]").size() == 32);
	CHECK(!f.elements(".sampler-section[section=keyboard] " + f.control("Root key")).empty());
	CHECK(f.has(".sampler-key[key=\"60\"]", "root"));
	CHECK(f.has(".sampler-loop", "off"));

	// A toggle and a value field's wheel set the parameters.
	f.click(f.control("Reverse"));
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Reverse") == 1.0; }));
	f.click(f.control("Reverse"));
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Reverse") == 0.0; }));
	f.app.ui().wheel(std::nullopt, of(f.control("Coarse")), 0.0F, -1.0F);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Coarse") == 1.0; }));

	// A click on a key sets the root key, and the keyboard shows the key range.
	f.click(".sampler-key[key=\"48\"]");
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Root key") == 48.0; }));
	CHECK(f.has(".sampler-key[key=\"48\"]", "root"));
	f.set("Key high", 72.0);
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-key[key=\"73\"]", "outside"); }));
	CHECK(!f.has(".sampler-key[key=\"72\"]", "outside"));

	// Dragging the start marker to the middle of the display sets Start, in one undo step.
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const double y = display.at("y").get<double>() + (display.at("height").get<double>() / 2);
	const size_t steps = f.app.call("history.get").at("steps").size();
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"),
		{"", 0, ui::Point{static_cast<int>(x + (width / 2)), static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.5) < 0.02; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	const double left = f.box(".sampler-marker[marker=\"Start\"]").at("x").get<double>() +
		(f.box(".sampler-marker[marker=\"Start\"]").at("width").get<double>() / 2);
	CHECK_THAT(left, WithinAbs(x + (width / 2), 3.0));
	// The end marker can't go before the start.
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"End\"]"),
		{"", 0, ui::Point{static_cast<int>(x + 2), static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("End") - f.parameter("Start")) < 1e-9; }));

	// A loop mode shows the loop.
	f.set("Loop mode", 1.0);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-loop", "off"); }));
}

TEST_CASE("the sampler panel loads dropped files and normalizes the sample", "[ui]")
{
	Fixture f;
	f.open();
	const uint64_t before = f.samplerChannel().at("sample").get<uint64_t>();
	CHECK(!f.app.ui().drop(std::nullopt, of(".sampler-display"), {f.other}).at("element").is_null());
	REQUIRE(f.app.waitUntil([&] { return f.samplerChannel().at("sample") != before; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "other"; }));

	// The sine's peak is half the full level: Normalize Sample raises it by 6 dB.
	f.click("#sampler-normalize");
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Sample gain") - 6.02) < 0.1; }));
}

TEST_CASE("Add Sampler Track opens the add track dialog on its Sampler page", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("project.addSamplerTrack", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#add-track-dialog").empty(); }));
	CHECK(f.elements("#track-types").at(0).at("attributes").at("value") == "sampler");
	CHECK(f.elements("#track-instrument").empty());
	f.click("#add-track-confirm");
	REQUIRE(f.app.waitUntil([&] { return f.elements("#add-track-dialog").empty(); }));
	const Json project = f.app.call("project.get");
	// Below the selected audio track, named as sampler.create names it.
	REQUIRE(project.at("tracks").size() == 3);
	const Json& added = project.at("tracks").at(1);
	CHECK(added.at("name") == "Sampler 02");
	CHECK(added.at("type") == "instrument");
	for (const Json& each : project.at("channels"))
		if (each.at("id") == added.at("channel"))
			CHECK(each.at("instrument") == "sampler");
}

TEST_CASE("a change in a value field and a drag of its marker are two undo steps", "[ui]")
{
	Fixture f;
	f.open();
	const size_t steps = f.app.call("history.get").at("steps").size();
	// The session's first value field gesture, then the first marker drag: their numbers mustn't meet.
	f.app.ui().wheel(std::nullopt, of(f.control("Start")), 0.0F, -1.0F);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Start") > 0.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("history.get").at("steps").size() == steps + 1; }));
	const Json display = f.box("#sampler-display");
	const double y = display.at("y").get<double>() + (display.at("height").get<double>() / 2);
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"),
		{"", 0,
			ui::Point{static_cast<int>(display.at("x").get<double>() + (display.at("width").get<double>() / 2)),
				static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.5) < 0.02; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 2);
}

TEST_CASE("the sampler panel shows why a sample didn't load or normalize", "[ui]")
{
	Fixture f;
	f.open();
	CHECK(!f.has("#sampler-message", "error"));
	const std::filesystem::path text = f.folder.path() / "notes.txt";
	std::ofstream(text) << "not audio";
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {text});
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));
	const Json message = f.elements("#sampler-message").at(0);
	CHECK(!message.value("text", "").empty());
	CHECK(message.at("attributes").value("title", "") == message.value("text", "")); // the whole text

	// Another track shown takes the message away, as it concerned the track before.
	f.select(f.audio);
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-panel", "empty"); }));
	CHECK(!f.has("#sampler-message", "error"));
	f.select(f.track);
	REQUIRE(f.app.waitUntil([&] { return !f.has("#sampler-panel", "empty"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {text});
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));

	// A file that loads clears the message.
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {f.other});
	REQUIRE(f.app.waitUntil([&] { return !f.has("#sampler-message", "error"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());

	// Normalize Sample is refused for a silent sample.
	test::Audio silence;
	silence.samples.assign(4800, 0.0f);
	const std::filesystem::path silent = f.folder.path() / "silent.wav";
	test::writeWav(silent, silence);
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {silent});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "silent"; }));
	f.click("#sampler-normalize");
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").find("silent between its start and end") !=
		std::string::npos);
}

TEST_CASE("Lock Parameter Settings and the normalization level last when the panel is made anew", "[ui]")
{
	Fixture f;
	// What a later version adds to the setting stays in it.
	f.app.call("settings.set", {{"key", "ui.samplerPanel"}, {"value", {{"later", 1}}}});
	f.open();
	const auto setting = [&] { return f.app.call("settings.get", {{"key", "ui.samplerPanel"}}).at("value"); };
	f.click("#sampler-lock");
	REQUIRE(f.app.waitUntil([&] { return setting().value("lockParameters", false); }));
	CHECK(setting().value("later", 0) == 1);
	f.app.ui().wheel(std::nullopt, of("#sampler-level"), 0.0F, 1.0F);
	REQUIRE(f.app.waitUntil([&] { return setting().value("normalizeLevel", 0.0) < 0.0; }));
	const double step = setting().at("normalizeLevel").get<double>();
	// Changes in quick succession, as a drag of the field makes them: the older ones that settings.changed brings back
	// don't set the field back meanwhile (#1180).
	for (int i = 0; i < 20; ++i)
	{
		f.app.ui().wheel(std::nullopt, of("#sampler-level"), 0.0F, 1.0F);
		f.app.shell().step(std::chrono::milliseconds(0));
	}
	REQUIRE(f.app.waitUntil([&] { return std::abs(setting().value("normalizeLevel", 0.0) - (21 * step)) < 1e-6; }));
	const double level = setting().at("normalizeLevel").get<double>();

	// In a window of its own the panel is a new one, with the lock and the level as they were.
	f.app.appUi().docking().setFloating("sampler", true);
	const ui::Window* window = nullptr;
	REQUIRE(f.app.waitUntil(
		[&]
		{
			window = f.app.appUi().docking().windowOf("sampler");
			return window != nullptr &&
				!f.app.ui().dom(window->id(), "#sampler-lock[checked]", 0).at("elements").empty();
		}));
	REQUIRE(f.app.waitUntil([&] { return !f.app.ui().dom(window->id(), "[parameter]", 0).at("elements").empty(); }));
	const Json field = f.app.ui().dom(window->id(), "#sampler-level", 0).at("elements").at(0);
	CHECK_THAT(std::stod(field.at("attributes").at("value").get<std::string>()), WithinAbs(level, 1e-3));
	// Normalize Sample brings the sine's peak, half the full level, to the level.
	f.app.ui().click(window->id(), of("#sampler-normalize"), ui::MouseButton::Left, 1, {});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Sample gain") - (6.02 + level)) < 0.1; }));
}

TEST_CASE("the keyboard's handles drag the root key and the key range", "[ui]")
{
	Fixture f;
	f.open();
	const Json keyboard = f.box("#sampler-keyboard");
	const double x = keyboard.at("x").get<double>();
	const double key = keyboard.at("width").get<double>() / 128.0;
	const auto at = [&](int pitch) { return static_cast<int>(x + ((pitch + 0.5) * key)); };
	const auto centre = [&](const std::string& selector)
	{
		const Json handle = f.box(selector);
		return handle.at("x").get<double>() + (handle.at("width").get<double>() / 2);
	};
	const auto drag = [&](const std::string& handle, int pitch)
	{
		const std::string selector = ".sampler-handle[handle=" + handle + "]";
		const Json box = f.box(selector);
		const int y = static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
		f.app.ui().drag(std::nullopt, of(selector), {"", 0, ui::Point{at(pitch), y}});
	};
	CHECK_THAT(centre(".sampler-handle[handle=root]"), WithinAbs(at(60), 2.0));

	// A drag of the root key handle is one undo step.
	const size_t steps = f.app.call("history.get").at("steps").size();
	drag("root", 50);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Root key") == 50.0; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre(".sampler-handle[handle=root]") - at(50)) < 2.0; }));
	CHECK(f.has(".sampler-key[key=\"50\"]", "root"));

	// The key range's handles, the high one not below the low one.
	drag("low", 40);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 40.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-key[key=\"39\"]", "outside"); }));
	drag("high", 30);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key high") == 40.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-key[key=\"41\"]", "outside"); }));
	// Nor the low one above the high one.
	f.set("Key high", 50.0);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-key[key=\"50\"]", "outside"); }));
	drag("low", 70);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 50.0; }));

	// On the same key the three handles lie side by side, so each can be grabbed (#1180).
	f.set("Root key", 50.0);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre(".sampler-handle[handle=root]") - at(50)) < 2.0; }));
	const std::vector<std::string> handles = {"root", "low", "high"};
	for (size_t i = 0; i < handles.size(); ++i)
		for (size_t j = i + 1; j < handles.size(); ++j)
		{
			const Json a = f.box(".sampler-handle[handle=" + handles[i] + "]");
			const Json b = f.box(".sampler-handle[handle=" + handles[j] + "]");
			const auto apart = [](const Json& first, const Json& second, const char* position, const char* size)
			{
				return first.at(position).get<double>() + first.at(size).get<double>() <=
					second.at(position).get<double>() + 1.0; // rounded to whole pixels
			};
			INFO(handles[i] << " and " << handles[j]);
			CHECK((apart(a, b, "x", "width") || apart(b, a, "x", "width") || apart(a, b, "y", "height") ||
				apart(b, a, "y", "height")));
		}
	drag("high", 60);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key high") == 60.0; }));
	CHECK(f.parameter("Key low") == 50.0);
}

TEST_CASE("a drag of a handle or a marker ends when the button goes up outside the window", "[ui]")
{
	Fixture f;
	f.open();
	const Json keyboard = f.box("#sampler-keyboard");
	const double x = keyboard.at("x").get<double>();
	const double key = keyboard.at("width").get<double>() / 128.0;
	const auto at = [&](int pitch) { return static_cast<int>(x + ((pitch + 0.5) * key)); };
	const auto handleY = [&]
	{
		const Json box = f.box(".sampler-handle[handle=low]");
		return static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	};
	f.app.ui().drag(std::nullopt, of(".sampler-handle[handle=low]"), {"", 0, ui::Point{at(40), handleY()}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 40.0; }));

	// Dragged past the window's left edge, where the button goes up: the lowest key.
	f.app.ui().drag(std::nullopt, of(".sampler-handle[handle=low]"), {"", 0, ui::Point{-40, handleY()}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 0.0; }));
	// The mouse back over the keyboard with no button held moves the handle no more.
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{at(60), handleY()}});
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{at(70), handleY()}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.parameter("Key low") == 0.0);
	CHECK(!f.has(".sampler-handle[handle=low]", "dragged"));

	// The same for the display's markers: Start dragged past the left edge, then the mouse over the display.
	const Json display = f.box("#sampler-display");
	const int y = static_cast<int>(display.at("y").get<double>() + (display.at("height").get<double>() / 2));
	const auto across = [&](double share)
	{ return static_cast<int>(display.at("x").get<double>() + (display.at("width").get<double>() * share)); };
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"), {"", 0, ui::Point{across(0.3), y}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.3) < 0.02; }));
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"), {"", 0, ui::Point{-40, y}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Start") == 0.0; }));
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{across(0.5), y}});
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{across(0.6), y}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.parameter("Start") == 0.0);
	CHECK(!f.has(".sampler-marker[marker=\"Start\"]", "dragged"));
}

TEST_CASE("a drag of a handle ends when its window loses the focus", "[ui]")
{
	Fixture f;
	f.open();
	const Json keyboard = f.box("#sampler-keyboard");
	const double x = keyboard.at("x").get<double>();
	const double key = keyboard.at("width").get<double>() / 128.0;
	const auto at = [&](int pitch) { return static_cast<float>(x + ((pitch + 0.5) * key)); };
	const Json box = f.box(".sampler-handle[handle=low]");
	const auto y = static_cast<float>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	// Through SDL's own events, as the OS sends them: the button down on the low key's handle, a drag to key 40.
	const auto push = [&](SDL_EventType type, float to)
	{
		SDL_Event event{};
		event.type = type;
		if (type == SDL_EVENT_MOUSE_MOTION)
		{
			event.motion.windowID = f.app.main().id();
			event.motion.x = to;
			event.motion.y = y;
			event.motion.state = SDL_BUTTON_LMASK;
		}
		else if (type == SDL_EVENT_WINDOW_FOCUS_LOST)
		{
			event.window.windowID = f.app.main().id();
		}
		else
		{
			event.button.windowID = f.app.main().id();
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.clicks = 1;
			event.button.x = to;
			event.button.y = y;
		}
		REQUIRE(SDL_PushEvent(&event));
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	push(SDL_EVENT_MOUSE_MOTION, at(0));
	push(SDL_EVENT_MOUSE_BUTTON_DOWN, at(0));
	for (int pitch = 5; pitch <= 40; pitch += 5)
		push(SDL_EVENT_MOUSE_MOTION, at(pitch));
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 40.0; }));
	REQUIRE(f.has(".sampler-handle[handle=low]", "dragged"));

	// The focus goes, e.g. to Alt+Tab, and the button goes up elsewhere: the drag ends (#1191).
	push(SDL_EVENT_WINDOW_FOCUS_LOST, at(40));
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-handle[handle=low]", "dragged"); }));
	push(SDL_EVENT_MOUSE_MOTION, at(60));
	push(SDL_EVENT_MOUSE_MOTION, at(70));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.parameter("Key low") == 40.0);
}

TEST_CASE("Import Audio File loads the file chosen, with the lock, and shows why one didn't load", "[ui]")
{
	Fixture f;
	f.open();
	const auto answer = [&](const std::filesystem::path& file)
	{
		f.click("#sampler-import");
		REQUIRE(f.app.waitUntil([&] { return f.app.shell().fileDialogs().size() == 1; }));
		CHECK(f.app.shell().answerFileDialog(f.app.shell().fileDialogs().begin()->first, {file}));
	};
	f.click("#sampler-lock");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#sampler-lock[checked]").empty(); }));
	f.set("Cutoff", 1000.0);

	// A file that isn't audio: the panel says why, and the sample stays.
	const uint64_t before = f.samplerChannel().at("sample").get<uint64_t>();
	const std::filesystem::path text = f.folder.path() / "notes.txt";
	std::ofstream(text) << "not audio";
	answer(text);
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));
	CHECK(!f.elements("#sampler-message").at(0).value("text", "").empty());
	CHECK(f.samplerChannel().at("sample") == before);

	// An audio file loads, keeping the settings as the lock asks, and the message goes.
	answer(f.other);
	REQUIRE(f.app.waitUntil([&] { return f.samplerChannel().at("sample") != before; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "other"; }));
	CHECK(f.parameter("Cutoff") == 1000.0);
	REQUIRE(f.app.waitUntil([&] { return !f.has("#sampler-message", "error"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());

	// Closed while the dialog is open, the panel can't show why: the file loads all the same, with the lock.
	f.set("Cutoff", 2000.0);
	f.click("#sampler-import");
	REQUIRE(f.app.waitUntil([&] { return f.app.shell().fileDialogs().size() == 1; }));
	f.app.appUi().docking().setTabHidden("sampler", true);
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-panel").empty(); }));
	const uint64_t loaded = f.samplerChannel().at("sample").get<uint64_t>();
	CHECK(f.app.shell().answerFileDialog(f.app.shell().fileDialogs().begin()->first, {f.file}));
	REQUIRE(f.app.waitUntil([&] { return f.samplerChannel().at("sample") != loaded; }));
	CHECK(f.parameter("Cutoff") == 2000.0);
}
