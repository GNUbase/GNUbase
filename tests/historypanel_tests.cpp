// SPDX-License-Identifier: GPL-3.0-or-later
// The edit history panel (#95, docs/features/edit-history.md) in the app's project window, driven with ui::Automation
// as MCP's UI tools drive it: it lists the engine's undo steps, and a click on a step undoes or redoes up to it.
#include "uiapp.h"

#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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

	Json row(test::UiApp& app, int index)
	{
		const Json rows = elements(app, "#history tbody tr");
		REQUIRE(static_cast<int>(rows.size()) > index);
		return rows[static_cast<size_t>(index)];
	}

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	std::string cell(test::UiApp& app, int index, const std::string& column)
	{
		const Json found = elements(app, "#history tbody tr td." + column);
		REQUIRE(static_cast<int>(found.size()) > index);
		return found[static_cast<size_t>(index)].at("text").get<std::string>();
	}

	void click(test::UiApp& app, const std::string& selector, int index)
	{
		app.ui().click(std::nullopt, {selector, index, std::nullopt}, ui::MouseButton::Left, 1, {});
	}

	size_t position(test::UiApp& app)
	{
		return app.call("history.get").at("position").get<size_t>();
	}

	// The panel shows the engine's position: the step before it is the selected row, those after it are undone.
	bool showsPosition(test::UiApp& app, size_t at)
	{
		const Json rows = elements(app, "#history tbody tr");
		if (rows.size() != 4)
			return false;
		for (size_t i = 0; i < rows.size(); ++i)
			if (has(rows[i], "selected") != (i == at) || has(rows[i], "undone") != (i > at))
				return false;
		return true;
	}
}

TEST_CASE("the edit history panel lists the undo steps and undoes and redoes up to the one clicked", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "midi"}});
	app.call("project.set", {{"name", "History"}});

	// Edit > History in the reference (manual p. 110): the panel opens in the right zone.
	app.keys().run("window.history", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	CHECK(elements(app, "#right-zone").at(0).at("visible") == true);
	CHECK(elements(app, "#history").at(0).at("visible") == true);

	// The initial state, then a row per step with its action, time, state and details (pp. 110-111).
	CHECK(cell(app, 0, "action") == "Initial state");
	CHECK(cell(app, 1, "action") == "Add track");
	CHECK(cell(app, 3, "action") == "Change project");
	CHECK(cell(app, 1, "state") == "Done");
	const std::string time = cell(app, 1, "time");
	CHECK(time.size() == 8);
	CHECK(time[2] == ':');
	CHECK(time[5] == ':');

	// A click on a step undoes the steps after it, a click on an undone step redoes up to it.
	click(app, "#history tbody tr", 1);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 1); }));
	CHECK(position(app) == 1);
	CHECK(cell(app, 2, "state") == "Undone");
	click(app, "#history tbody tr", 0);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 0); }));
	CHECK(position(app) == 0);
	CHECK(app.call("project.get").at("tracks").empty());
	click(app, "#history tbody tr", 2);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2); }));
	CHECK(position(app) == 2);
	CHECK(app.call("project.get").at("tracks").size() == 2);

	// The details take a note for the step; typing it in doesn't undo or redo.
	click(app, "#history tbody tr input", 0);
	app.ui().text(std::nullopt, "first track");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("history.get").at("steps").at(0).at("details") == "first track"; }));
	CHECK(position(app) == 2);
	CHECK(showsPosition(app, 2));

	// Undo and redo from other clients move the selection, and the panel shows the details they set.
	app.call("history.redo");
	app.call("history.describe", {{"step", 2}, {"details", "renamed"}});
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	REQUIRE(app.waitUntil([&app]
		{ return elements(app, "#history tbody tr input").at(2).at("attributes").value("value", "") == "renamed"; }));

	// A new project clears the history.
	app.call("project.new");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#history tbody tr").size() == 1; }));
	CHECK(has(row(app, 0), "selected"));
}
