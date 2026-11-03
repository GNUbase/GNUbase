// SPDX-License-Identifier: GPL-3.0-or-later
// The fade dialogs (#138, docs/features/fades.md) in the app's UI, driven with ui::Automation as MCP's UI tools drive
// them: the Auto Fades dialog of the project and of a track, the crossfade editor and the fade dialog. Fade handles and
// the Fades menu's commands are in tests/eventediting_tests.cpp, the commands themselves in tests/fade_tests.cpp.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"

#include <RmlUi/Core/Element.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool open(test::UiApp& app, const std::string& selector)
	{
		return !elements(app, selector).empty();
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool checked(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("attributes", Json::object()).contains("checked");
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void type(test::UiApp& app, const std::string& selector, const std::string& text)
	{
		click(app, selector);
		app.ui().key(std::nullopt, "Ctrl+A");
		app.ui().text(std::nullopt, text);
	}

	// Opens a drop-down list, moves the selection that many options down with the arrow keys, and closes it.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < down; ++i)
			app.ui().key(std::nullopt, "Down");
		click(app, select);
	}

	void run(test::UiApp& app, const std::string& command, const std::string& dialog)
	{
		app.keys().run(command, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, &dialog] { return open(app, dialog); }));
		app.shell().step(std::chrono::milliseconds(0));
	}

	void closes(test::UiApp& app, const std::string& dialog)
	{
		CHECK(app.waitUntil([&app, &dialog] { return !open(app, dialog); }));
	}

	uint64_t addEvent(test::UiApp& app, uint64_t track, const Json& clip, int64_t position)
	{
		return app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", position}, {"length", 48000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	}

	// Two overlapping audio events on a linear audio track, from 0 to 1 second and from 0.75 to 1.75 seconds, of a
	// one minute 48 kHz clip.
	struct Events
	{
		explicit Events(test::UiApp& app)
			: track(app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}})
					  .at("track")
					  .at("id")
					  .get<uint64_t>())
		{
			const Json media =
				app.call(
					   "media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 2880000}})
					.at("media");
			const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id");
			first = addEvent(app, track, clip, 0);
			second = addEvent(app, track, clip, 36000);
		}

		uint64_t track = 0;
		uint64_t first = 0;
		uint64_t second = 0;
	};

	// An audio event as project.get has it.
	Json eventOf(test::UiApp& app, uint64_t id)
	{
		const Json project = app.call("project.get");
		for (const Json& found : project.at("audioEvents"))
			if (found.at("id") == id)
				return found;
		return {};
	}
}

TEST_CASE("the auto fades dialog sets the project's auto fades, and a track's own or the project's", "[ui]")
{
	test::UiApp app({900, 700});
	app.appUi().showProject();
	const Events events(app);

	run(app, "project.autoFades", "#auto-fades");
	CHECK(element(app, "#auto-fades-title").value("text", std::string()) == "Auto Fades: Project");
	CHECK(element(app, "#auto-fades-length").at("value") == "10.0");
	click(app, "#auto-fades-in");
	type(app, "#auto-fades-length", "20");
	choose(app, "#auto-fades-crossfade-mode", 1); // equal power
	click(app, "#auto-fades-ok");
	closes(app, "#auto-fades");
	const Json settings = app.call("project.get").at("autoFades");
	CHECK(settings.at("fadeIn") == true);
	CHECK(settings.at("fadeOut") == false);
	CHECK(settings.at("length") == 20.0);
	CHECK(settings.at("crossfadeMode") == "equalPower");

	// A length the engine refuses keeps the dialog open with its message.
	run(app, "project.autoFades", "#auto-fades");
	type(app, "#auto-fades-length", "900");
	click(app, "#auto-fades-ok");
	CHECK(open(app, "#auto-fades"));
	CHECK_THAT(
		element(app, "#auto-fades-message").value("text", std::string()), Catch::Matchers::ContainsSubstring("500"));
	click(app, "#auto-fades-cancel");
	closes(app, "#auto-fades");

	// The selected track: its own settings once Use Project Settings is off, the project's again once it is on.
	app.appUi().trackList()->select({events.track});
	run(app, "track.autoFades", "#auto-fades");
	CHECK(checked(app, "#auto-fades-project"));
	click(app, "#auto-fades-project");
	click(app, "#auto-fades-out");
	click(app, "#auto-fades-ok");
	closes(app, "#auto-fades");
	const Json track = app.call("project.get").at("tracks")[0];
	REQUIRE(track.at("autoFades").is_object());
	CHECK(track.at("autoFades").at("fadeOut") == true);
	CHECK(track.at("autoFades").at("fadeIn") == true); // from the project's
	run(app, "track.autoFades", "#auto-fades");
	CHECK(!checked(app, "#auto-fades-project"));
	click(app, "#auto-fades-project");
	click(app, "#auto-fades-ok");
	closes(app, "#auto-fades");
	CHECK(app.call("project.get").at("tracks")[0].at("autoFades").is_null());
}

TEST_CASE("Crossfade crossfades the selected events, then opens the crossfade editor for them", "[ui]")
{
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	REQUIRE(app.waitUntil(
		[&display] { return display.project().is_object() && display.project().at("audioEvents").size() == 2; }));
	display.select({events.first, events.second});
	app.keys().run("audio.crossfade", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, &events] { return eventOf(app, events.second).at("crossfade").is_object(); }));
	CHECK(eventOf(app, events.second).at("crossfade").at("from") == events.first);

	// Now the editor: the crossfade over the whole overlap of 250 ms.
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents")[1].at("crossfade").is_object(); }));
	run(app, "audio.crossfade", "#crossfade-editor");
	CHECK(element(app, "#crossfade-mode").at("value") == "equalGain");
	CHECK(element(app, "#crossfade-in-length").at("value") == "250.0");
	choose(app, "#crossfade-mode", 2);      // custom
	choose(app, "#crossfade-out-shape", 3); // S-curve
	type(app, "#crossfade-in-length", "100");
	click(app, "#crossfade-ok");
	closes(app, "#crossfade-editor");
	const Json crossfade = eventOf(app, events.second).at("crossfade");
	CHECK(crossfade.at("mode") == "custom");
	CHECK(crossfade.at("outShape") == "sCurve");
	CHECK(crossfade.at("length") == 4800);
	CHECK(crossfade.at("outLength") == 0);

	// A double click in the crossfade's zone, at 0.875 seconds on the track's lane, opens it too; Remove removes the
	// crossfade.
	app.setDpRatio(1.0F);
	app.shell().step(std::chrono::milliseconds(0));
	Rml::Element& area = display.eventArea();
	const double x = area.GetAbsoluteLeft() + ((0.875 - display.view().start) / display.view().scale);
	const double y = area.GetAbsoluteTop() + 20.0;
	app.ui().click(
		std::nullopt, {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}}, ui::MouseButton::Left, 2, {});
	REQUIRE(app.waitUntil([&app] { return open(app, "#crossfade-editor"); }));
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#crossfade-remove");
	closes(app, "#crossfade-editor");
	CHECK(eventOf(app, events.second).at("crossfade").is_null());
}

TEST_CASE("the fade dialog gives every selected event the fades it shows, as one undo step", "[ui]")
{
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	app.call("audioEvent.setFades", {{"event", events.first}, {"fadeIn", {{"length", 2400}, {"shape", "sCurve"}}}});
	REQUIRE(app.waitUntil(
		[&display] { return display.project().is_object() && display.project().at("audioEvents").size() == 2; }));
	display.select({events.first, events.second});
	run(app, "audio.fadeEditor", "#fade-editor");
	CHECK(element(app, "#fade-in-shape").at("value") == "sCurve");
	CHECK(element(app, "#fade-in-length").at("value") == "50.0");
	type(app, "#fade-out-length", "125");
	click(app, "#fade-editor-ok");
	closes(app, "#fade-editor");
	for (const uint64_t id : {events.first, events.second})
	{
		const Json event = eventOf(app, id);
		CHECK(event.at("fadeIn") == Json{{"length", 2400}, {"shape", "sCurve"}});
		CHECK(event.at("fadeOut") == Json{{"length", 6000}, {"shape", "linear"}});
	}
	CHECK(app.call("history.get").at("steps").back().at("action") == "Change fades");
}
