// SPDX-License-Identifier: GPL-3.0-or-later
// The info line and the inspector (#89, docs/features/inspector.md) in the app's project window, driven with
// ui::Automation as MCP's UI tools drive them: the info line shows the first selected event or part and its edits
// change every selected one, as one undo step; the inspector shows the selected track's basics, routing, inserts and
// notepad, and its controls change the track's channel.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json classes = element(app, selector).value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
	}

	std::string attribute(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json attributes = element(app, selector).value("attributes", Json::object());
		return attributes.contains(name) ? attributes.at(name).get<std::string>() : std::string();
	}

	// The project window at a dp ratio of 1 with the info line and the inspector shown.
	struct Window
	{
		Window() : display(app.appUi().showProject()), list(*app.appUi().trackList())
		{
			app.setDpRatio(1.0F);
			app.appUi().docking().setVisible("project", "infoLine", true);
			app.appUi().docking().open("inspector");
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		uint64_t addTrack(const Json& parameters)
		{
			return call("track.add", parameters).at("track").at("id").get<uint64_t>();
		}

		uint64_t addPart(uint64_t track, int64_t position, int64_t length, const std::string& name)
		{
			return call("midiPart.add", {{"track", track}, {"position", position}, {"length", length}, {"name", name}})
				.at("part")
				.at("id")
				.get<uint64_t>();
		}

		// The object with that id in a list of the project.
		Json find(const char* list, uint64_t id)
		{
			const Json objects = call("project.get").at(list);
			const auto found = std::find_if(
				objects.begin(), objects.end(), [id](const Json& object) { return object.at("id") == id; });
			REQUIRE(found != objects.end());
			return *found;
		}

		Json part(uint64_t id) { return find("midiParts", id); }
		Json channel(uint64_t track) { return find("channels", find("tracks", track).at("channel").get<uint64_t>()); }

		void click(const std::string& selector, ui::MouseButton button = ui::MouseButton::Left, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, button, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Double-clicks a display or value field, types into it and presses Enter, or keys such as Ctrl+Enter.
		void type(const std::string& selector, const std::string& text, const std::string& enter = "Enter")
		{
			click(selector, ui::MouseButton::Left, 2);
			app.ui().key(std::nullopt, "Ctrl+A");
			app.ui().text(std::nullopt, text);
			app.ui().key(std::nullopt, enter);
			app.shell().step(std::chrono::milliseconds(0));
		}

		bool shows(const std::string& selector, const std::string& value)
		{
			return app.waitUntil([&] { return attribute(app, selector, "value") == value; });
		}

		test::UiApp app{{1200, 600}};
		ui::EventDisplay& display;
		ui::TrackList& list;
	};
}

TEST_CASE("the info line shows the first selected part, and its edits change every selected one, undoably", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t first = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	const uint64_t second = window.addPart(midi, 12 * g_quarter, 8 * g_quarter, "Verse");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));

	// Nothing selected: the info line says so.
	CHECK(hasClass(window.app, "#info-line", "empty"));
	window.display.select({first});
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "2.1.1.0"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "empty"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "multiple"));
	CHECK(attribute(window.app, "#info-line textdisplay[field=name]", "value") == "Intro");
	CHECK(attribute(window.app, "#info-line textdisplay[field=end]", "value") == "3.1.1.0");
	CHECK(attribute(window.app, "#info-line textdisplay[field=length]", "value") == "1.0.0.0");
	// Parts have no volume.
	CHECK(hasClass(window.app, "#info-line [info-field=volume]", "hidden"));

	// Two selected: the first one shows, and a new start moves both by as much, as one undo step.
	window.display.select({second, first});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	CHECK(attribute(window.app, "#info-line textdisplay[field=name]", "value") == "Intro");
	window.type("#info-line textdisplay[field=start]", "3.1.1.0");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("position") == 8 * g_quarter; }));
	CHECK(window.part(second).at("position") == 16 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "3.1.1.0"));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("position") == 4 * g_quarter; }));
	CHECK(window.part(second).at("position") == 12 * g_quarter);

	// A length relative to the current ones, and with Ctrl the same length for all.
	REQUIRE(window.shows("#info-line textdisplay[field=length]", "1.0.0.0"));
	window.type("#info-line textdisplay[field=length]", "2.0.0.0");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("length") == 8 * g_quarter; }));
	CHECK(window.part(second).at("length") == 12 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=length]", "2.0.0.0"));
	window.type("#info-line textdisplay[field=length]", "1.0.0.0", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("length") == 4 * g_quarter; }));
	CHECK(window.part(first).at("length") == 4 * g_quarter);
	// The end, relative too.
	REQUIRE(window.shows("#info-line textdisplay[field=end]", "3.1.1.0"));
	window.type("#info-line textdisplay[field=end]", "3.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("length") == 6 * g_quarter; }));
	CHECK(window.part(second).at("length") == 6 * g_quarter);

	// The name, mute and lock go to every selected part.
	window.type("#info-line textdisplay[field=name]", "Chorus");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("name") == "Chorus"; }));
	CHECK(window.part(first).at("name") == "Chorus");
	window.click("#info-line button[field=mute]");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("mute") == true; }));
	CHECK(window.part(first).at("mute") == true);
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line button[field=mute]", "checked"); }));
	window.click("#info-line button[field=lock]");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("lock") == true; }));
	CHECK(window.part(second).at("lock") == true);

	// Changes from another client show.
	window.call("midiPart.set", {{"part", first}, {"name", "Bridge"}});
	CHECK(window.shows("#info-line textdisplay[field=name]", "Bridge"));
	window.display.select({});
	CHECK(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "empty"); }));
}

TEST_CASE("the info line edits an audio event's volume", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const uint64_t event =
		window
			.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", 96000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "take"));
	CHECK(element(window.app, "#info-line textdisplay[field=name]").at("attributes").contains("disabled"));
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=volume]", "hidden"));
	window.type("#info-line valuefield[field=volume]", "-6");
	REQUIRE(window.app.waitUntil([&] { return window.find("audioEvents", event).at("volume") == -6.0; }));
	window.call("history.undo");
	CHECK(window.app.waitUntil([&] { return window.find("audioEvents", event).at("volume") == 0.0; }));
}

TEST_CASE("the inspector shows the selected track, and its controls change the track's channel", "[ui]")
{
	Window window;
	CHECK(hasClass(window.app, ".inspector", "empty"));
	const uint64_t group = window.addTrack({{"type", "group"}, {"name", "Drums"}});
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t marker = window.addTrack({{"type", "marker"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 3; }));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	CHECK_FALSE(hasClass(window.app, ".inspector", "empty"));
	for (const char* section : {"basics", "routing", "inserts", "notepad"})
		CHECK(elements(window.app, ".inspector [sections=inspector] > [section=" + std::string(section) + "]").size() ==
			1);

	// Basics: the switches, volume and pan.
	for (const char* control : {"mute", "solo", "record", "monitor"})
	{
		INFO(control);
		const std::string key = std::string(control) == "record" ? "recordEnable" : control;
		const std::string button = ".inspector button[control=" + std::string(control) + "]";
		window.click(button);
		REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at(key) == true; }));
		REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, button, "checked"); }));
		window.call("channel.set", {{"channel", window.channel(audio).at("id")}, {key, false}});
		REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, button, "checked"); }));
	}
	window.type(".inspector valuefield[control=volume]", "-6");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == -6.0; }));
	window.type(".inspector valuefield[control=pan]", "-50");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("pan") == -0.5; }));

	// Routing: the output from another client shows; one chosen in the drop-down list goes to the engine.
	const uint64_t groupChannel = window.channel(group).at("id").get<uint64_t>();
	window.call("channel.set", {{"channel", window.channel(audio).at("id")}, {"output", groupChannel}});
	REQUIRE(window.shows(".inspector select[control=output]", std::to_string(groupChannel)));
	window.click(".inspector select[control=output]");
	window.app.ui().key(std::nullopt, "Up");
	window.click(".inspector select[control=output]");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("output") != groupChannel; }));
	CHECK(attribute(window.app, ".inspector select[control=input]", "value") ==
		std::to_string(window.channel(audio).at("input").get<uint64_t>()));

	// Inserts: an overview of the slots, bypassed ones marked.
	const uint64_t slot =
		window.call("plugin.insert", {{"channel", window.channel(audio).at("id")}, {"plugin", "builtin:eq"}})
			.at("slot")
			.at("id")
			.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return elements(window.app, ".inspector .insert").size() == 1; }));
	window.call("plugin.set", {{"slot", slot}, {"bypass", true}});
	CHECK(window.app.waitUntil([&] { return hasClass(window.app, ".inspector .insert", "bypassed"); }));

	// The notepad: what is typed is the channel's notes once the field loses the focus.
	window.click(".inspector textarea.notepad");
	window.app.ui().text(std::nullopt, "Double the chorus");
	window.app.ui().key(std::nullopt, "Escape");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("notes") == "Double the chorus"; }));

	// The name.
	window.type(".inspector textdisplay.inspector-name", "Lead");
	REQUIRE(window.app.waitUntil([&] { return window.find("tracks", audio).at("name") == "Lead"; }));

	// A section header opens and closes its section.
	window.click(".inspector [section=basics] .section-header");
	CHECK(hasClass(window.app, ".inspector [section=basics]", "collapsed"));
	window.click(".inspector [section=basics] .section-header");
	CHECK_FALSE(hasClass(window.app, ".inspector [section=basics]", "collapsed"));

	// Set up Sections, from the context menu: hiding a section hides it and keeps it in the user's layout.
	window.click(".inspector", ui::MouseButton::Right);
	REQUIRE(hasClass(window.app, "#inspector-setup", "open"));
	window.click("#inspector-setup [setup-section=notepad]");
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, ".inspector [section=notepad]", "hidden"); }));
	CHECK(window.call("settings.get", {{"key", "ui.layout"}}).dump().find("notepad") != std::string::npos);

	// Another track: the group has no input or record buttons; a marker track has no channel to show.
	window.list.select({group});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Drums"));
	CHECK(hasClass(window.app, ".inspector [row=input]", "unavailable"));
	CHECK(hasClass(window.app, ".inspector button[control=record]", "unavailable"));
	window.list.select({marker});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, ".inspector [section=basics]", "unavailable"); }));
	CHECK(hasClass(window.app, ".inspector [section=notepad]", "unavailable"));
}
