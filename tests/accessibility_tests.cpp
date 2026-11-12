// SPDX-License-Identifier: GPL-3.0-or-later
// The accessibility baseline (#60, docs/features/accessibility.md): GNUbase's core dialogs work with the keyboard
// alone, driven in the app's UI with ui::Automation's keys as MCP's ui_key drives them, and the control with the
// keyboard focus shows it.
#include "uiapp.h"

#include "engine/engine.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/keycommands.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	// Presses the keys and returns the element with the focus then.
	Json key(test::UiApp& app, const std::string& keys)
	{
		const Json focus = app.ui().key(std::nullopt, keys).at("focus");
		app.shell().step(std::chrono::milliseconds(0));
		return focus;
	}

	std::string idOf(const Json& element)
	{
		return element.is_object() ? element.value("id", "") : "";
	}

	// Whether the element shows that it has the keyboard focus, as it does after Tab.
	bool focusShows(const Json& element)
	{
		const Json pseudo = element.value("pseudoClasses", Json::array());
		return std::find(pseudo.begin(), pseudo.end(), "focus-visible") != pseudo.end();
	}

	std::string attribute(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].value("attributes", Json::object()).value(name, "");
	}

	std::string value(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].value("value", "");
	}

	// Runs a key command as its key would, for commands without a key by default.
	void command(test::UiApp& app, const std::string& name)
	{
		app.keys().run(name, Json::object(), app.main(), nullptr);
		app.shell().step(std::chrono::milliseconds(0));
	}

	size_t tracks(test::UiApp& app)
	{
		return app.call("project.get").at("tracks").size();
	}
}

TEST_CASE("the add track dialog works with the keyboard alone", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	command(app, "project.addTrack");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#add-track-dialog"); }));

	// Tab goes to the list of track types, which shows the focus; the arrow keys choose the group page.
	Json focus = key(app, "Tab");
	CHECK(idOf(focus) == "track-types");
	CHECK(focusShows(focus));
	for (int i = 0; i < 4; ++i) // instrument, sampler, MIDI, group
		key(app, "Down");
	CHECK(attribute(app, "#track-types", "value") == "group");

	// A drop-down list opens with Enter, steps with the arrows and closes with Enter, keeping the option.
	CHECK(idOf(key(app, "Tab")) == "track-format");
	key(app, "Enter");
	key(app, "Up"); // mono, before stereo
	key(app, "Enter");
	CHECK(value(app, "#track-format") == "mono");
	CHECK(shown(app, "#add-track-dialog"));

	// Escape closes an open list without choosing, and leaves the dialog open.
	CHECK(idOf(key(app, "Tab")) == "track-output");
	const std::string output = value(app, "#track-output");
	key(app, "Enter");
	key(app, "Down");
	key(app, "Escape");
	CHECK(value(app, "#track-output") == output);
	CHECK(shown(app, "#add-track-dialog"));

	// A check box is in the order too.
	CHECK(idOf(key(app, "Tab")) == "track-project-folder");

	// A text field takes typed text; a value field edits as text with Enter and steps with the arrows.
	CHECK(idOf(key(app, "Tab")) == "track-name");
	app.ui().text(std::nullopt, "Bus");
	focus = key(app, "Tab");
	CHECK(idOf(focus) == "track-count");
	CHECK(focusShows(focus));
	key(app, "Enter");
	CHECK(elements(app, "#track-count.editing").size() == 1);
	app.ui().text(std::nullopt, "2");
	key(app, "Enter");
	CHECK(elements(app, "#track-count.editing").empty());
	CHECK(std::stof(attribute(app, "#track-count", "value")) == 2.0F);
	key(app, "Up");
	key(app, "Down");
	CHECK(std::stof(attribute(app, "#track-count", "value")) == 2.0F);

	// A check box toggles with Space; the buttons follow, and Tab goes around the ends both ways.
	focus = key(app, "Tab");
	CHECK(idOf(focus) == "keep-open");
	CHECK(focusShows(focus));
	key(app, "Space");
	CHECK(elements(app, "#keep-open").at(0).at("attributes").contains("checked"));
	CHECK(idOf(key(app, "Tab")) == "add-track-cancel");
	CHECK(idOf(key(app, "Tab")) == "add-track-confirm");
	CHECK(idOf(key(app, "Tab")) == "track-types");
	CHECK(idOf(key(app, "Shift+Tab")) == "add-track-confirm");

	// Enter on the focused Add Track button adds two mono group tracks called Bus; Keep Dialog Open keeps it open.
	key(app, "Enter");
	REQUIRE(app.waitUntil([&app] { return tracks(app) == 2; }));
	const Json project = app.call("project.get");
	for (const Json& track : project.at("tracks"))
	{
		CHECK(track.at("type") == "group");
		CHECK(track.at("name").get<std::string>().find("Bus") == 0);
	}
	CHECK(shown(app, "#add-track-dialog"));

	// Escape closes it without adding more.
	key(app, "Escape");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#add-track-dialog"); }));
	CHECK(tracks(app) == 2);
}

TEST_CASE("Enter clicks the focused button of a dialog, and its primary button from elsewhere", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	app.call("track.add", {{"type", "audio"}});
	app.call("project.save", {{"path", (dir.path() / "Song.dawproject").u8string()}});
	app.call("track.add", {{"type", "audio"}});

	// Revert asks first: Enter on the focused Cancel cancels, although Discard is the primary button.
	command(app, "file.revert");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	const Json cancel = key(app, "Tab");
	CHECK(cancel.at("attributes").at("result") == "cancel");
	CHECK(focusShows(cancel));
	key(app, "Enter");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	CHECK(tracks(app) == 2);

	// Space on the focused Discard discards the changes.
	command(app, "file.revert");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	key(app, "Tab");
	CHECK(idOf(key(app, "Tab")) == "discard-changes");
	key(app, "Space");
	REQUIRE(app.waitUntil([&app] { return tracks(app) == 1; }));

	// Save as Template starts in its name field, and Enter there clicks OK.
	command(app, "file.saveTemplate");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#save-template-dialog"); }));
	app.ui().text(std::nullopt, "Drums");
	key(app, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("project.templates").at("templates").size() == 1; }));
	CHECK(app.call("project.templates").at("templates")[0].at("name") == "Drums");
}

TEST_CASE("the start screen creates a project from a template with the keyboard alone", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "audio"}});
	app.call("project.saveTemplate", {{"name", "Two Tracks"}});
	app.call("project.new");

	// File > New Project's key opens it; Tab and Down select the template, Tab and Enter create the project.
	key(app, "Mod+N");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#start-templates listitem").size() == 1; }));
	CHECK(idOf(key(app, "Tab")) == "start-templates");
	key(app, "Down");
	CHECK(elements(app, "#start-templates listitem.selected").size() == 1);
	CHECK(idOf(key(app, "Tab")) == "create-template");
	key(app, "Enter");
	REQUIRE(app.waitUntil([&app] { return tracks(app) == 2; }));
	CHECK(!shown(app, "#start-screen"));
}

TEST_CASE("every button that shows only an icon has a title, for its tooltip and a screen reader's name", "[unit]")
{
	const std::regex button("<button[^>]*icon-only[^>]*>");
	for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "ui"))
	{
		const std::string text = test::readText(entry.path());
		for (auto it = std::sregex_iterator(text.begin(), text.end(), button); it != std::sregex_iterator(); ++it)
		{
			INFO(entry.path().filename().u8string() << ": " << it->str());
			CHECK(it->str().find("title=") != std::string::npos);
		}
	}
}
