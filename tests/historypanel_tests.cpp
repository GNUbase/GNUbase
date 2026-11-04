// SPDX-License-Identifier: GPL-3.0-or-later
// The edit history panel (#95, docs/features/edit-history.md) in the app's project window, driven with ui::Automation
// as MCP's UI tools drive it: it lists the engine's undo steps, a click on a step undoes or redoes up to it, and so
// does dragging the separator between the done and the undone steps.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"

#include <RmlUi/Core/Context.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <variant>

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

	int boxOf(const Json& element, const char* side)
	{
		return element.at("box").at(side).get<int>();
	}

	// The separator is drawn between the row of the last done step and the first undone one (boxes are rounded to
	// pixels).
	bool separatorAfter(test::UiApp& app, size_t at)
	{
		const Json rows = elements(app, "#history tbody tr");
		const Json separator = elements(app, "#history .history-separator");
		if (separator.size() != 1 || !separator[0].at("visible").get<bool>() || boxOf(separator[0], "height") <= 0)
			return false;
		const int top = boxOf(separator[0], "y");
		const Json& done = rows[at];
		if (top + 1 < boxOf(done, "y") + boxOf(done, "height"))
			return false;
		return at + 1 == rows.size() || top + boxOf(separator[0], "height") <= boxOf(rows[at + 1], "y") + 1;
	}

	// The separator is under the row, and the rows after it are undone.
	bool undoneAfter(test::UiApp& app, size_t at)
	{
		const Json rows = elements(app, "#history tbody tr");
		for (size_t i = 0; i < rows.size(); ++i)
			if (has(rows[i], "undone") != (i > at))
				return false;
		return at < rows.size() && separatorAfter(app, at);
	}

	// The panel shows the engine's position among its rows: the step before it is the selected row with the
	// separator under it, those after it are undone.
	bool showsPosition(test::UiApp& app, size_t at, size_t count = 4)
	{
		const Json rows = elements(app, "#history tbody tr");
		if (rows.size() != count)
			return false;
		for (size_t i = 0; i < rows.size(); ++i)
			if (has(rows[i], "selected") != (i == at))
				return false;
		return undoneAfter(app, at);
	}

	ui::Target point(int x, int y)
	{
		return {"", 0, ui::Point{x, y}};
	}

	// The middle of the separator, in the window's pixels.
	ui::Target separator(test::UiApp& app)
	{
		const Json found = elements(app, "#history .history-separator");
		REQUIRE(found.size() == 1);
		return point(boxOf(found[0], "x") + (boxOf(found[0], "width") / 2),
			boxOf(found[0], "y") + (boxOf(found[0], "height") / 2));
	}

	// A point at the bottom of the row, above the middle of the row after it: the separator goes under the row there.
	ui::Target under(test::UiApp& app, int index, const ui::Target& from)
	{
		const Json found = row(app, index);
		return point(from.point.value_or(ui::Point{}).x, boxOf(found, "y") + boxOf(found, "height") - 1);
	}

	// The element has the focus.
	bool focused(const Json& element)
	{
		const Json names = element.value("pseudoClasses", Json::array());
		return std::find(names.begin(), names.end(), "focus") != names.end();
	}

	// The list view's box, as MCP's ui_dom has it.
	Json list(test::UiApp& app)
	{
		const Json found = elements(app, "#history");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	// Where the rows show the separator going: the number of rows before it, less the initial state's.
	size_t previewed(test::UiApp& app)
	{
		const Json rows = elements(app, "#history tbody tr");
		return static_cast<size_t>(
				   std::count_if(rows.begin(), rows.end(), [](const Json& r) { return !has(r, "undone"); })) -
			1;
	}

	// The mouse's y is where a drag puts the separator after the row: between its middle and the next row's (give
	// or take the separator's height, which moves the rows between the two).
	bool between(test::UiApp& app, size_t at, int y)
	{
		const Json rows = elements(app, "#history tbody tr");
		if (at + 1 >= rows.size())
			return false;
		const auto middle = [&rows](size_t index)
		{ return boxOf(rows[index], "y") + (boxOf(rows[index], "height") / 2); };
		constexpr int slack = 4;
		return middle(at) - slack <= y && y <= middle(at + 1) + slack;
	}

	// Three steps, with the panel open: Add track, Add track, Change project.
	void threeSteps(test::UiApp& app)
	{
		app.appUi().showProject();
		app.call("track.add", {{"type", "audio"}});
		app.call("track.add", {{"type", "midi"}});
		app.call("project.set", {{"name", "History"}});
		app.keys().run("window.history", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	}

	size_t historyChanges(engine::Client& client)
	{
		size_t count = 0;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<engine::Event>(&*message);
				event != nullptr && event->name == "history.changed")
				++count;
		return count;
	}

	// The history.changed events that come, waiting a moment for any more.
	size_t changesSoon(test::UiApp& app, engine::Client& client, size_t expected)
	{
		size_t changes = 0;
		app.waitUntil(
			[&]
			{
				changes += historyChanges(client);
				return changes > expected;
			},
			std::chrono::milliseconds(300));
		return changes;
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

	// The arrow keys step from the position another client undid to, also after a new action rebuilt the rows.
	click(app, "#history tbody tr", 2);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2); }));
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 1); }));
	app.ui().key(std::nullopt, "Down");
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2); }));
	CHECK(position(app) == 2);
	app.call("history.undo");
	app.call("project.set", {{"name", "Rebuilt"}});
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "#history tbody tr").size() == 3 && has(row(app, 2), "selected"); }));
	app.ui().key(std::nullopt, "Up");
	REQUIRE(app.waitUntil([&app] { return position(app) == 1; }));
	CHECK(app.call("project.get").at("tracks").size() == 1);

	// A new project clears the history.
	app.call("project.new");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#history tbody tr").size() == 1; }));
	CHECK(has(row(app, 0), "selected"));
}

TEST_CASE("dragging the edit history's separator up undoes and down redoes, with one command when it ends", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "midi"}});
	app.call("project.set", {{"name", "History"}});
	app.keys().run("window.history", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	engine::Client listener(app.engine());

	// Up to under the first step undoes the two after it, in one command (manual p. 111).
	ui::Target from = separator(app);
	app.ui().drag(std::nullopt, from, under(app, 1, from));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 1); }));
	CHECK(position(app) == 1);
	CHECK(app.call("project.get").at("tracks").size() == 1);
	CHECK(historyChanges(listener) == 1);

	// Down redoes; above the first step undoes everything; a drag past the end redoes everything.
	from = separator(app);
	app.ui().drag(std::nullopt, from, under(app, 2, from));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2); }));
	CHECK(position(app) == 2);
	CHECK(app.call("project.get").at("tracks").size() == 2);
	from = separator(app);
	app.ui().drag(std::nullopt, from, under(app, 0, from));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 0); }));
	CHECK(position(app) == 0);
	CHECK(app.call("project.get").at("tracks").empty());
	from = separator(app);
	app.ui().drag(std::nullopt, from, under(app, 3, from));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	CHECK(position(app) == 3);
	CHECK(historyChanges(listener) == 3);

	// Dropped above the list, in the tab bar, it undoes everything; dropped below the last row, it redoes everything.
	from = separator(app);
	const int x = from.point.value_or(ui::Point{}).x;
	app.ui().drag(std::nullopt, from, point(x, boxOf(list(app), "y") - 10));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 0); }));
	CHECK(position(app) == 0);
	from = separator(app);
	const Json last = row(app, 3);
	app.ui().drag(std::nullopt, from, point(x, boxOf(last, "y") + boxOf(last, "height") + 30));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	CHECK(position(app) == 3);
	CHECK(historyChanges(listener) == 2);

	// A drag that ends where it started sends nothing.
	from = separator(app);
	const ui::Point at = from.point.value_or(ui::Point{});
	app.ui().drag(std::nullopt, from, point(at.x + 20, at.y + 1));
	size_t changes = 0;
	CHECK(!app.waitUntil(
		[&]
		{
			changes += historyChanges(listener);
			return changes > 0;
		},
		std::chrono::milliseconds(300)));
	CHECK(position(app) == 3);
	CHECK(showsPosition(app, 3));
}

TEST_CASE(
	"the edit history's separator shows where a drag goes, and a drag that the history changed under sends nothing",
	"[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "midi"}});
	app.call("project.set", {{"name", "History"}});
	app.keys().run("window.history", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	engine::Client listener(app.engine());

	// Through the window's context, to look before the button is released.
	Rml::Context& context = app.main().context();
	const ui::Point from = separator(app).point.value_or(ui::Point{});
	const int to = under(app, 1, separator(app)).point.value_or(ui::Point{}).y;
	context.ProcessMouseMove(from.x, from.y, 0);
	context.ProcessMouseButtonDown(0, 0);
	for (const int y : {from.y - 2, (from.y + to) / 2, to})
		context.ProcessMouseMove(from.x, y, 0);
	REQUIRE(app.waitUntil([&app] { return undoneAfter(app, 1); }));
	CHECK(cell(app, 2, "state") == "Undone");
	CHECK(has(row(app, 3), "selected"));
	CHECK(position(app) == 3);
	CHECK(historyChanges(listener) == 0);

	// Another client adds a step meanwhile: the rows show it, and the drag no longer undoes anything.
	app.call("project.set", {{"name", "Changed"}});
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 4, 5); }));
	CHECK(historyChanges(listener) == 1);
	context.ProcessMouseButtonUp(0, 0);
	size_t changes = 0;
	CHECK(!app.waitUntil(
		[&]
		{
			changes += historyChanges(listener);
			return changes > 0;
		},
		std::chrono::milliseconds(300)));
	CHECK(position(app) == 4);
	CHECK(showsPosition(app, 4, 5));
}

TEST_CASE("typing a step's details keeps the focus while another client undoes and redoes", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "midi"}});
	app.call("project.set", {{"name", "History"}});
	app.keys().run("window.history", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	engine::Client listener(app.engine());

	// The row of the step being typed in moves past the separator and back; the text goes on where it was.
	click(app, "#history tbody tr input", 2);
	app.ui().text(std::nullopt, "ab");
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2); }));
	app.ui().text(std::nullopt, "c");
	app.call("history.redo");
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	app.ui().text(std::nullopt, "d");
	CHECK(app.call("history.get").at("steps").at(2).at("details").get<std::string>().empty());
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("history.get").at("steps").at(2).at("details") == "abcd"; }));
	// Undo, redo and the details once, not what was typed before each move.
	size_t changes = 0;
	app.waitUntil(
		[&]
		{
			changes += historyChanges(listener);
			return changes > 3;
		},
		std::chrono::milliseconds(300));
	CHECK(changes == 3);
}

TEST_CASE("the edit history panel is drawn again when another client changes the history", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "midi"}});
	app.call("project.set", {{"name", "History"}});
	// In a window of its own, where no other element asks for frames when the project changes.
	ui::Docking& docking = app.appUi().docking();
	docking.setFloating("history", true);
	const ui::Window* window = docking.windowOf("history");
	REQUIRE(window != nullptr);

	// Without ui::Automation, which updates the context before it reads the DOM: the shell alone has to draw the
	// window, as in the app, where keys reach the panel before its next draw. The list view moves its selection,
	// which the arrow keys step from, to the engine's position only then.
	const auto selected = [window](size_t index)
	{
		Rml::ElementList rows;
		Rml::Context& context = window->context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			context.GetDocument(i)->QuerySelectorAll(rows, "#history tbody tr");
		return rows.size() == 4 && rows[index]->IsClassSet("selected");
	};
	REQUIRE(app.waitUntil([&selected] { return selected(3); }));
	REQUIRE(app.waitUntil([window] { return !window->renderRequested(); }));
	uint64_t frames = window->frames();
	app.call("history.undo");
	CHECK(app.waitUntil([&] { return selected(2) && window->frames() > frames; }));

	REQUIRE(app.waitUntil([window] { return !window->renderRequested(); }));
	frames = window->frames();
	app.call("history.describe", {{"step", 1}, {"details", "drawn"}});
	CHECK(app.waitUntil([&] { return window->frames() > frames; }));
}

TEST_CASE(
	"details typed in the edit history go with their step when another client adds one, and go when it goes", "[ui]")
{
	test::UiApp app({1200, 700});
	threeSteps(app);
	click(app, "#history tbody tr", 1);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 1); }));
	engine::Client listener(app.engine());

	// At position 1 of 3, a note for step 2, which is undone; another client's new action drops it and takes its
	// place (manual p. 111). The note goes with its step instead of onto the new one, and its field loses the focus.
	click(app, "#history tbody tr input", 1);
	app.ui().text(std::nullopt, "dropped");
	app.call("project.set", {{"name", "Other"}});
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2, 3); }));
	CHECK(cell(app, 2, "action") == "Change project");
	for (const Json& field : elements(app, "#history tbody tr input"))
	{
		CHECK(field.at("attributes").value("value", "").empty());
		CHECK(!focused(field));
	}
	CHECK(focused(list(app)));
	CHECK(changesSoon(app, listener, 1) == 1);
	CHECK(app.call("history.get").at("steps").at(1).at("details").get<std::string>().empty());

	// A note for a step that stays: the rows are made again for the new step, and the note goes on in its field.
	click(app, "#history tbody tr input", 0);
	app.ui().text(std::nullopt, "ke");
	app.call("track.add", {{"type", "audio"}});
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3, 4); }));
	CHECK(focused(elements(app, "#history tbody tr input").at(0)));
	app.ui().text(std::nullopt, "pt");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("history.get").at("steps").at(0).at("details") == "kept"; }));
	CHECK(changesSoon(app, listener, 2) == 2); // the new step and the note, once

	// At the maximum number of steps, a new one drops the oldest: the note moves up a row with its step.
	app.call("history.set", {{"maxSteps", 3}});
	click(app, "#history tbody tr input", 1);
	app.ui().text(std::nullopt, "mov");
	app.call("project.set", {{"name", "Third"}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			return elements(app, "#history tbody tr").size() == 4 && cell(app, 1, "action") == "Change project" &&
				cell(app, 2, "action") == "Add track";
		}));
	const Json field = elements(app, "#history tbody tr input").at(0);
	CHECK(focused(field));
	CHECK(field.at("attributes").value("value", "") == "mov");
	CHECK(elements(app, "#history tbody tr input").at(1).at("attributes").value("value", "").empty());
	app.ui().text(std::nullopt, "ed");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("history.get").at("steps").at(0).at("details") == "moved"; }));
	const Json steps = app.call("history.get").at("steps");
	CHECK(steps.at(1).at("details").get<std::string>().empty());
	CHECK(steps.at(2).at("details").get<std::string>().empty());
	CHECK(changesSoon(app, listener, 3) == 3); // history.set, the new step and the note

	// A note for the oldest step, which a new step drops: as many rows as before, but the note goes with its step.
	click(app, "#history tbody tr input", 0);
	app.ui().text(std::nullopt, "gone");
	app.call("project.set", {{"name", "Fourth"}});
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "#history tbody tr").size() == 4 && cell(app, 1, "action") == "Add track"; }));
	for (const Json& input : elements(app, "#history tbody tr input"))
	{
		CHECK(input.at("attributes").value("value", "").find("gone") == std::string::npos);
		CHECK(!focused(input));
	}
	CHECK(focused(list(app)));
	CHECK(changesSoon(app, listener, 1) == 1); // the new step only
	const Json after = app.call("history.get");
	for (const Json& step : after.at("steps"))
		CHECK(step.at("details").get<std::string>().find("gone") == std::string::npos);
}

TEST_CASE("dragging the edit history's separator follows the wheel and scrolls the list past its edges", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	constexpr size_t steps = 60;
	for (size_t i = 0; i < steps; ++i)
		app.call("project.set", {{"name", "Step " + std::to_string(i)}});
	app.call("history.undo", {{"steps", steps}});
	app.keys().run("window.history", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 0, steps + 1); }));
	const int top = boxOf(list(app), "y");
	const int bottom = top + boxOf(list(app), "height");
	REQUIRE(boxOf(row(app, steps), "y") > bottom); // longer than the panel
	engine::Client listener(app.engine());
	Rml::Context& context = app.main().context();

	// The wheel during a drag scrolls the rows under the mouse, and the separator follows them.
	const ui::Point from = separator(app).point.value_or(ui::Point{});
	const int y = under(app, 2, separator(app)).point.value_or(ui::Point{}).y;
	context.ProcessMouseMove(from.x, from.y, 0);
	context.ProcessMouseButtonDown(0, 0);
	for (const int at : {from.y + 2, y})
		context.ProcessMouseMove(from.x, at, 0);
	REQUIRE(app.waitUntil([&app] { return undoneAfter(app, 2); }));
	context.ProcessMouseWheel(Rml::Vector2f(0.0F, 3.0F), 0);
	REQUIRE(app.waitUntil([&] { return previewed(app) > 4 && between(app, previewed(app), y); }));
	const size_t wheeled = previewed(app);
	context.ProcessMouseButtonUp(0, 0);
	REQUIRE(app.waitUntil([&] { return position(app) == wheeled; }));
	CHECK(historyChanges(listener) == 1);

	// Held still below the list's bottom edge, the mouse scrolls it on over time: the separator reaches the last step,
	// and the list goes on to its end, which brings the separator into view.
	const auto inView = [&app, bottom]
	{
		const Json found = elements(app, "#history .history-separator");
		return found.size() == 1 && boxOf(found[0], "y") + boxOf(found[0], "height") <= bottom;
	};
	const auto drag = [&](int to)
	{
		const ui::Point start = separator(app).point.value_or(ui::Point{});
		context.ProcessMouseMove(start.x, start.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		context.ProcessMouseMove(start.x, start.y + 2, 0);
		context.ProcessMouseMove(start.x, to, 0);
	};
	drag(bottom + 80);
	REQUIRE(app.waitUntil([&app] { return undoneAfter(app, steps); }));
	REQUIRE(app.waitUntil(inView));
	context.ProcessMouseButtonUp(0, 0);
	REQUIRE(app.waitUntil([&app] { return position(app) == steps; }));

	// Above its top edge, back to the initial state.
	drag(top - 80);
	REQUIRE(app.waitUntil([&app] { return undoneAfter(app, 0); }));
	context.ProcessMouseButtonUp(0, 0);
	REQUIRE(app.waitUntil([&app] { return position(app) == 0; }));
	CHECK(changesSoon(app, listener, 2) == 2);
}

TEST_CASE("a drag of the edit history's separator that the docking drops puts the rows back", "[ui]")
{
	test::UiApp app({1200, 700});
	threeSteps(app);
	engine::Client listener(app.engine());
	Rml::Context& context = app.main().context();
	const ui::Point from = separator(app).point.value_or(ui::Point{});
	const int to = under(app, 1, separator(app)).point.value_or(ui::Point{}).y;
	context.ProcessMouseMove(from.x, from.y, 0);
	context.ProcessMouseButtonDown(0, 0);
	for (const int y : {from.y - 2, to})
		context.ProcessMouseMove(from.x, y, 0);
	REQUIRE(app.waitUntil([&app] { return undoneAfter(app, 1); }));
	CHECK(focused(list(app)));

	// The panel's tab moves in its tab group, which is made again with the panel moved into it: RmlUi drops the drag
	// without a dragend, and the rows show the steps as they are again.
	ui::Docking& docking = app.appUi().docking();
	docking.moveTab("history", "media");
	docking.open("history");
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	CHECK(cell(app, 2, "state") == "Done");
	// The press gave the list the focus, which the docking moved away; the grip never had it.
	CHECK(!focused(list(app)));
	CHECK(!focused(elements(app, "#history .history-grip").at(0)));
	CHECK(!focused(elements(app, "#history .history-separator").at(0)));
	context.ProcessMouseMove(from.x, to + 2, 0);
	context.ProcessMouseButtonUp(0, 0);
	CHECK(changesSoon(app, listener, 0) == 0);
	CHECK(position(app) == 3);
	CHECK(showsPosition(app, 3));
}

TEST_CASE("the edit history's separator is grabbed a little above and below its line", "[ui]")
{
	test::UiApp app({1200, 700});
	threeSteps(app);
	click(app, "#history tbody tr", 1);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 1); }));
	const Json line = elements(app, "#history .history-separator").at(0);
	const int x = boxOf(line, "x") + (boxOf(line, "width") / 2);
	const int below = boxOf(list(app), "y") + boxOf(list(app), "height") + 20;

	// Just under the line, on the next row: dropped below the list, everything is redone, and the row isn't clicked.
	const int justUnder = boxOf(line, "y") + boxOf(line, "height") + 1;
	REQUIRE(boxOf(row(app, 2), "y") <= justUnder);
	app.ui().drag(std::nullopt, point(x, justUnder), point(x, below));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 3); }));
	CHECK(position(app) == 3);

	// Just over it, on the row of the last done step: dropped above the list, everything is undone.
	click(app, "#history tbody tr", 2);
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 2); }));
	const int justOver = boxOf(elements(app, "#history .history-separator").at(0), "y") - 2;
	REQUIRE(boxOf(row(app, 2), "y") + boxOf(row(app, 2), "height") > justOver);
	app.ui().drag(std::nullopt, point(x, justOver), point(x, boxOf(list(app), "y") - 10));
	REQUIRE(app.waitUntil([&app] { return showsPosition(app, 0); }));
	CHECK(position(app) == 0);
}
