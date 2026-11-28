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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
		app.ui().key(std::nullopt, "Mod+A");
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

	// Only audio tracks have auto fades: a MIDI track selected above the audio track doesn't count, and with only the
	// MIDI track selected the command can't run.
	const auto midi = app.call("track.add", {{"type", "midi"}, {"index", 0}}).at("track").at("id").get<uint64_t>();
	const auto selects = [&app](const std::vector<uint64_t>& tracks)
	{
		return app.waitUntil(
			[&app, &tracks]
			{
				app.appUi().trackList()->select(tracks);
				return app.appUi().trackList()->selection() == tracks;
			});
	};
	REQUIRE(selects({midi, events.track}));
	run(app, "track.autoFades", "#auto-fades");
	const Json tracks = app.call("project.get").at("tracks");
	const auto audio = std::find_if(
		tracks.begin(), tracks.end(), [&events](const Json& track) { return track.at("id") == events.track; });
	REQUIRE(audio != tracks.end());
	CHECK(element(app, "#auto-fades-title").value("text", std::string()) ==
		"Auto Fades: " + audio->at("name").get<std::string>());
	click(app, "#auto-fades-cancel");
	closes(app, "#auto-fades");
	REQUIRE(selects({midi}));
	CHECK_FALSE(app.keys().canRun("track.autoFades", app.main(), nullptr));
}

TEST_CASE("Crossfade crossfades the selected events, then opens the crossfade editor for them", "[ui]")
{
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 2; }));
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

	// OK without edits leaves the lengths as they were, not rounded to the 0.1 ms shown.
	app.call("crossfade.set", {{"from", events.first}, {"to", events.second}, {"length", 4801}});
	REQUIRE(app.waitUntil(
		[&display] { return display.project().at("audioEvents")[1].at("crossfade").at("length") == 4801; }));
	run(app, "audio.crossfade", "#crossfade-editor");
	click(app, "#crossfade-ok");
	closes(app, "#crossfade-editor");
	CHECK(eventOf(app, events.second).at("crossfade").at("length") == 4801);
	CHECK(eventOf(app, events.second).at("crossfade").at("outLength") == 0);

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

TEST_CASE("Crossfade opens the editor only for a crossfade that plays between selected events", "[ui]")
{
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	// A third event from 1.5 to 2.5 seconds; the first two crossfade.
	const uint64_t third = addEvent(app, events.track, eventOf(app, events.first).at("clip"), 72000);
	app.call("event.crossfade", {{"events", {events.first, events.second}}});
	REQUIRE(app.waitUntil(
		[&display]
		{
			return display.project().at("audioEvents").size() == 3 &&
				display.project().at("audioEvents")[1].at("crossfade").is_object();
		}));

	// The second and the third: the crossfade from the first one doesn't open, they crossfade.
	display.select({events.second, third});
	app.keys().run("audio.crossfade", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, third] { return eventOf(app, third).at("crossfade").is_object(); }));
	CHECK(eventOf(app, third).at("crossfade").at("from") == events.second);
	CHECK_FALSE(open(app, "#crossfade-editor"));
	// The first event alone: it has one crossfade, which opens (reference manual p. 389).
	display.select({events.first});
	run(app, "audio.crossfade", "#crossfade-editor");
	click(app, "#crossfade-cancel");
	closes(app, "#crossfade-editor");

	// A crossfade that rests, as the events no longer overlap, doesn't open either.
	app.call("event.move", {{"events", {third}}, {"delta", 48000}});
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents")[2].at("position") == 120000; }));
	REQUIRE(eventOf(app, third).at("crossfade").is_object());
	display.select({events.second, third});
	app.keys().run("audio.crossfadeEditor", Json::object(), app.main(), nullptr);
	app.call("project.get");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(open(app, "#crossfade-editor"));
}

TEST_CASE("the fade dialog gives every selected event the fades it shows, as one undo step", "[ui]")
{
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	app.call("audioEvent.setFades", {{"event", events.first}, {"fadeIn", {{"length", 2401}, {"shape", "sCurve"}}}});
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 2; }));
	display.select({events.first, events.second});
	run(app, "audio.fadeEditor", "#fade-editor");
	CHECK(element(app, "#fade-in-shape").at("value") == "sCurve");
	CHECK(element(app, "#fade-in-length").at("value") == "50.0");
	// Apply Length is off at first (reference manual p. 386): OK gives them the curves only.
	CHECK_FALSE(checked(app, "#fade-apply-length"));
	type(app, "#fade-out-length", "125");
	click(app, "#fade-editor-ok");
	closes(app, "#fade-editor");
	CHECK(eventOf(app, events.first).at("fadeIn") == Json{{"length", 2401}, {"shape", "sCurve"}});
	CHECK(eventOf(app, events.second).at("fadeIn") == Json{{"length", 0}, {"shape", "sCurve"}});
	CHECK(eventOf(app, events.second).at("fadeOut").at("length") == 0);

	// With it, the lengths too: the one typed, and the one left as shown exactly as it was, not rounded to 0.1 ms.
	run(app, "audio.fadeEditor", "#fade-editor");
	click(app, "#fade-apply-length");
	type(app, "#fade-out-length", "125");
	click(app, "#fade-editor-ok");
	closes(app, "#fade-editor");
	for (const uint64_t id : {events.first, events.second})
	{
		const Json event = eventOf(app, id);
		CHECK(event.at("fadeIn") == Json{{"length", 2401}, {"shape", "sCurve"}});
		CHECK(event.at("fadeOut") == Json{{"length", 6000}, {"shape", "linear"}});
	}
	CHECK(app.call("history.get").at("steps").back().at("action") == "Change fades");

	// A stretched warped event's fades fit into what it plays for, its fadeRoom (#965): at 240 BPM in musical mode
	// the clip plays at half speed at the project's 120, so the one second event plays for two.
	const Json clip = eventOf(app, events.first).at("clip");
	app.call("clip.setWarp", {{"clip", clip}, {"musical", true}});
	app.call("clip.setWarp", {{"clip", clip}, {"tempo", 240.0}});
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").at(0).contains("fadeRoom"); }));
	display.select({events.first});
	run(app, "audio.fadeEditor", "#fade-editor");
	click(app, "#fade-apply-length");
	type(app, "#fade-out-length", "1500");
	click(app, "#fade-editor-ok");
	closes(app, "#fade-editor");
	CHECK(eventOf(app, events.first).at("fadeOut") == Json{{"length", 72000}, {"shape", "linear"}});
}

TEST_CASE("Apply in the fade dialog leaves it open, and Restore puts back what it showed and the events had", "[ui]")
{
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	app.call("audioEvent.setFades", {{"event", events.first}, {"fadeIn", {{"length", 2400}, {"shape", "sCurve"}}}});
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 2; }));
	display.select({events.first, events.second});
	const Json first = eventOf(app, events.first);
	const Json second = eventOf(app, events.second);
	run(app, "audio.fadeEditor", "#fade-editor");

	// Before Apply, Restore puts back what the dialog showed when it opened (reference manual p. 386).
	choose(app, "#fade-in-shape", 1); // Equal Power
	type(app, "#fade-out-length", "125");
	click(app, "#fade-editor-restore");
	CHECK(element(app, "#fade-in-shape").at("value") == "sCurve");
	CHECK(element(app, "#fade-out-length").at("value") == "0.0");

	// Apply gives every event the fades shown and leaves the dialog open.
	click(app, "#fade-apply-length");
	type(app, "#fade-out-length", "125");
	click(app, "#fade-editor-apply");
	CHECK(open(app, "#fade-editor"));
	CHECK(eventOf(app, events.second).at("fadeIn") == Json{{"length", 2400}, {"shape", "sCurve"}});
	CHECK(eventOf(app, events.second).at("fadeOut") == Json{{"length", 6000}, {"shape", "linear"}});

	// After it, Restore gives the events back the fades they had when the dialog opened, as one undo step.
	click(app, "#fade-editor-restore");
	CHECK(eventOf(app, events.first) == first);
	CHECK(eventOf(app, events.second) == second);
	CHECK(app.call("history.get").at("steps").back().at("action") == "Restore fades");
	CHECK_FALSE(checked(app, "#fade-apply-length"));
	CHECK(element(app, "#fade-out-length").at("value") == "0.0");
	click(app, "#fade-editor-cancel");
	closes(app, "#fade-editor");
}

TEST_CASE("As Default in the fade dialog sets the default fades, which Apply Standard Fade In and Out give", "[ui]")
{
	// Reference manual pp. 385-387: the default fades take the dialog's shapes and lengths, with Apply Length off too.
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 2; }));
	display.select({events.first});
	run(app, "audio.fadeEditor", "#fade-editor");
	choose(app, "#fade-in-shape", 3); // S-Curve
	type(app, "#fade-in-length", "100");
	type(app, "#fade-out-length", "250.5");
	click(app, "#fade-editor-default");
	CHECK(app.call("settings.get", {{"key", "editing.defaultFadeIn"}}).at("value") ==
		Json{{"shape", "sCurve"}, {"length", 0.1}});
	CHECK(app.call("settings.get", {{"key", "editing.defaultFadeOut"}}).at("value") ==
		Json{{"shape", "linear"}, {"length", 0.2505}});
	// It changes no event, and the dialog stays open.
	CHECK(open(app, "#fade-editor"));
	click(app, "#fade-editor-cancel");
	closes(app, "#fade-editor");
	CHECK(eventOf(app, events.first).at("fadeIn").at("length") == 0);

	// Apply Standard Fade In and Out give the selected events the default fades.
	display.select({events.first, events.second});
	app.keys().run("audio.applyStandardFadeIn", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, &events]
		{ return eventOf(app, events.second).at("fadeIn") == Json{{"length", 4800}, {"shape", "sCurve"}}; }));
	CHECK(eventOf(app, events.first).at("fadeIn") == Json{{"length", 4800}, {"shape", "sCurve"}});
	app.keys().run("audio.applyStandardFadeOut", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, &events]
		{ return eventOf(app, events.first).at("fadeOut") == Json{{"length", 12024}, {"shape", "linear"}}; }));
	CHECK(eventOf(app, events.second).at("fadeOut") == Json{{"length", 12024}, {"shape", "linear"}});
	CHECK(app.call("history.get").at("steps").back().at("action") == "Apply Standard Fade Out");
}

TEST_CASE("As Default in the crossfade editor sets the default crossfade, which new crossfades get", "[ui]")
{
	// Reference manual p. 391: As Default keeps the editor's mode and curves for the crossfades made later, Recall
	// Default shows them again.
	test::UiApp app({900, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const Events events(app);
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 2; }));
	app.call("event.crossfade", {{"events", {events.first, events.second}}});
	REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents")[1].at("crossfade").is_object(); }));
	display.select({events.first, events.second});
	run(app, "audio.crossfade", "#crossfade-editor");
	choose(app, "#crossfade-mode", 2);      // custom
	choose(app, "#crossfade-out-shape", 3); // S-curve
	click(app, "#crossfade-default");
	CHECK(app.call("settings.get", {{"key", "editing.defaultCrossfade"}}).at("value") ==
		Json{{"mode", "custom"}, {"shape", "linear"}, {"outShape", "sCurve"}, {"length", 0.25}});
	// It changes no crossfade, and the editor stays open.
	CHECK(open(app, "#crossfade-editor"));
	click(app, "#crossfade-cancel");
	closes(app, "#crossfade-editor");
	CHECK(eventOf(app, events.second).at("crossfade").at("mode") == "equalGain");

	// A new crossfade is the default one.
	app.call("crossfade.remove", {{"event", events.second}});
	app.call("event.crossfade", {{"events", {events.first, events.second}}});
	CHECK(eventOf(app, events.second).at("crossfade").at("mode") == "custom");
	CHECK(eventOf(app, events.second).at("crossfade").at("outShape") == "sCurve");

	// Recall Default shows the default in the editor, which OK then gives the crossfade, with its length (p. 388).
	app.call("settings.set",
		{{"key", "editing.defaultCrossfade"},
			{"value", {{"mode", "custom"}, {"shape", "linear"}, {"outShape", "sCurve"}, {"length", 0.1}}}});
	app.call("crossfade.set", {{"from", events.first}, {"to", events.second}, {"mode", "equalPower"}});
	REQUIRE(app.waitUntil(
		[&display] { return display.project().at("audioEvents")[1].at("crossfade").at("mode") == "equalPower"; }));
	run(app, "audio.crossfade", "#crossfade-editor");
	CHECK(element(app, "#crossfade-mode").at("value") == "equalPower");
	click(app, "#crossfade-recall-default");
	CHECK(element(app, "#crossfade-mode").at("value") == "custom");
	CHECK(element(app, "#crossfade-out-shape").at("value") == "sCurve");
	CHECK(element(app, "#crossfade-in-length").at("value") == "100.0");
	click(app, "#crossfade-ok");
	closes(app, "#crossfade-editor");
	CHECK(eventOf(app, events.second).at("crossfade").at("mode") == "custom");
	CHECK(eventOf(app, events.second).at("crossfade").at("outShape") == "sCurve");
	CHECK(eventOf(app, events.second).at("crossfade").at("length") == 4800);
	CHECK(eventOf(app, events.second).at("crossfade").at("outLength") == 4800);
}
