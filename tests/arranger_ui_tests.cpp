// SPDX-License-Identifier: GPL-3.0-or-later
// The arranger events on the arranger track's lane in the project window (#1393, docs/features/arranger.md, reference
// manual pp. 404-405), driven with ui::Automation as MCP's UI tools drive them: shown with their names and lengths,
// drawn with the draw tool, moved, renamed and erased.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr double g_ratio = 0.5; // window pixels per dp

	// The project window at a dp ratio of 0.5, with snap to bars on: at 120 BPM in 4/4, a bar is 2 seconds.
	struct Window
	{
		Window() : display(app.appUi().showProject())
		{
			app.setDpRatio(static_cast<float>(g_ratio));
			app.call("snap.set", {{"on", true}});
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json events() { return app.call("arranger.get").at("events"); }

		size_t shown(const std::string& selector)
		{
			return app.ui().dom(std::nullopt, selector, 0).at("elements").size();
		}

		Json box(const std::string& selector)
		{
			const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
			REQUIRE(found.size() == 1);
			return found[0].at("box");
		}

		Json element(const std::string& selector)
		{
			const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
			REQUIRE(found.size() == 1);
			return found[0];
		}

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
			REQUIRE(found.size() == 1);
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		// The point on the arranger track's lane at a time, in the window's pixels.
		ui::Point at(double seconds)
		{
			const Json row = box(".arranger-lane-row");
			const ui::TimelineView& view = display.view();
			const double x = row.at("x").get<double>() + ((seconds - view.start) / view.scale * g_ratio);
			const double y = row.at("y").get<double>() + (row.at("height").get<double>() / 2.0);
			REQUIRE(x < row.at("x").get<double>() + row.at("width").get<double>());
			return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
		}

		test::UiApp app{{1000, 600}};
		ui::EventDisplay& display;
	};
}

TEST_CASE("the arranger track's lane shows arranger events and the draw tool draws them", "[ui]")
{
	Window window;
	window.app.call("track.add", {{"type", "arranger"}});
	window.app.call("arranger.add", {{"position", 0}, {"end", 4 * g_quarter}});
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 1; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	// An event spans its section and shows its name.
	CHECK(window.app.ui().dom(std::nullopt, ".arranger-lane-event", 0).at("elements")[0].value("text", std::string()) ==
		"A");
	const double bar = 2.0 / window.display.view().scale * g_ratio; // pixels
	CHECK(std::abs(window.box(".arranger-lane-event").at("width").get<double>() - bar) < 2.0);

	// The draw tool draws an event from the press to the release, snapped (reference manual p. 404).
	window.display.setTool(ui::Tool::Draw);
	window.app.ui().drag(std::nullopt, {"", 0, window.at(2.3)}, {"", 0, window.at(5.8)});
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 2; }));
	CHECK(window.events()[1].at("position") == 4 * g_quarter);
	CHECK(window.events()[1].at("end") == 12 * g_quarter);
	CHECK(window.events()[1].at("name") == "B");
	// A click draws one from the bar line at or before it up to the next, so that it holds the point clicked (#1597).
	window.app.ui().click(std::nullopt, {"", 0, window.at(7.2)}, ui::MouseButton::Left, 1, {});
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 3; }));
	CHECK(window.events()[2].at("position") == 12 * g_quarter);
	CHECK(window.events()[2].at("end") == 16 * g_quarter);
	CHECK(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 3; }));
	// Its name isn't opened for typing: arranger events name themselves.
	CHECK_FALSE(window.hasClass(".arranger-lane-field", "shown"));
}

TEST_CASE("arranger events on the lane move, rename and erase", "[ui]")
{
	Window window;
	window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}});
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 1; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::ObjectSelection);

	// A drag moves the event, snapped, keeping its length.
	window.app.ui().drag(std::nullopt, {"", 0, window.at(2.0)}, {"", 0, window.at(4.2)});
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("position") == 4 * g_quarter; }));
	CHECK(window.events()[0].at("end") == 12 * g_quarter);

	// A double click opens its name for typing.
	window.app.ui().click(std::nullopt, {".arranger-lane-event", 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".arranger-lane-field", "shown"); }));
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "Intro");
	// The field is as wide as the event, for its name (#1597).
	CHECK(std::abs(window.box(".arranger-lane-field").at("width").get<double>() -
			  window.box(".arranger-lane-event").at("width").get<double>()) < 2.0);
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("name") == "Intro"; }));
	CHECK(window.app.waitUntil(
		[&]
		{
			return window.app.ui()
					   .dom(std::nullopt, ".arranger-lane-event", 0)
					   .at("elements")[0]
					   .value("text", std::string()) == "Intro";
		}));
	CHECK(window.events()[0].at("position") == 4 * g_quarter);

	// Enter on an empty field keeps the name (#1597).
	window.app.ui().click(std::nullopt, {".arranger-lane-event", 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".arranger-lane-field", "shown"); }));
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().key(std::nullopt, "Backspace");
	window.app.ui().key(std::nullopt, "Enter");
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-field", "shown"); }));
	CHECK(window.events()[0].at("name") == "Intro");

	// While the button is down, the event follows the mouse, snapped; the release moves it (#1597).
	window.app.ui().press(std::nullopt, {"", 0, window.at(5.0)}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, {"", 0, window.at(7.2)});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.app.waitUntil(
		[&] { return std::abs(window.box(".arranger-lane-event").at("x").get<double>() - window.at(4.0).x) < 2.0; }));
	CHECK(window.events()[0].at("position") == 4 * g_quarter);
	window.app.ui().release(std::nullopt, {"", 0, window.at(7.2)}, ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("position") == 8 * g_quarter; }));
	CHECK(std::abs(window.box(".arranger-lane-event").at("x").get<double>() - window.at(4.0).x) < 2.0);

	// The erase tool removes the event clicked.
	window.display.setTool(ui::Tool::Erase);
	window.app.ui().click(std::nullopt, {".arranger-lane-event", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	CHECK(window.app.waitUntil([&] { return window.events().empty(); }));
}

TEST_CASE("an arranger event named X opens with its name and keeps it", "[ui]")
{
	// "X" is the chord lane's undefined chord, which opens empty; an arranger event may be named "X" (#1393).
	Window window;
	window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}, {"name", "X"}});
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 1; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::ObjectSelection);

	window.app.ui().click(std::nullopt, {".arranger-lane-event", 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".arranger-lane-field", "shown"); }));
	CHECK(
		window.app.ui().dom(std::nullopt, ".arranger-lane-field", 0).at("elements")[0].value("value", std::string()) ==
		"X");
	window.app.ui().key(std::nullopt, "Enter");
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-field", "shown"); }));
	CHECK(window.events()[0].at("name") == "X");
}

TEST_CASE("the draw tool draws arranger events over others, snapped to their edges, while the button is down", "[ui]")
{
	// Reference manual p. 404: with Snap on and the snap type Events, a new arranger event starts and ends at the other
	// arranger events' edges, and arranger events may overlap (#1597).
	Window window;
	window.app.call("arranger.add", {{"position", 0}, {"end", 6 * g_quarter}});               // 0 to 3 seconds
	window.app.call("arranger.add", {{"position", 10 * g_quarter}, {"end", 14 * g_quarter}}); // 5 to 7 seconds
	window.app.call("snap.set", {{"type", "events"}});
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 2; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::Draw);
	const double near = 3.0 * window.display.view().scale; // 3 dp in seconds, within the magnets' reach

	// While the button is down, the event drawn shows, growing with the mouse, from the edge its start snapped to.
	window.app.ui().press(std::nullopt, {"", 0, window.at(3.0 + near)}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, {"", 0, window.at(4.0)});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event.drawing") == 1; }));
	const Json drawing = window.box(".arranger-lane-event.drawing");
	CHECK(std::abs(drawing.at("x").get<double>() - window.at(3.0).x) < 2.0);
	CHECK(std::abs(drawing.at("x").get<double>() + drawing.at("width").get<double>() - window.at(4.0).x) < 2.0);
	CHECK(window.events().size() == 2);
	window.app.ui().move(std::nullopt, {"", 0, window.at(5.0 - near)});
	window.app.ui().release(std::nullopt, {"", 0, window.at(5.0 - near)}, ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 3; }));
	CHECK(window.events()[1].at("position") == 6 * g_quarter);
	CHECK(window.events()[1].at("end") == 10 * g_quarter);
	CHECK(window.app.waitUntil([&]
		{ return window.shown(".arranger-lane-event.drawing") == 0 && window.shown(".arranger-lane-event") == 3; }));

	// A press on an event draws over it instead of moving it.
	window.app.ui().drag(std::nullopt, {"", 0, window.at(1.5)}, {"", 0, window.at(3.0 + near)});
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 4; }));
	CHECK(window.events()[0].at("position") == 0);
	CHECK(window.events()[0].at("end") == 6 * g_quarter);
	CHECK(window.events()[1].at("end") == 6 * g_quarter);
}

TEST_CASE("a click just after a bar line draws an arranger event that holds it, whatever its start snaps to", "[ui]")
{
	// With the snap type Events, a click's start can snap back over a bar line to an arranger event's edge: the event
	// drawn still ends at the bar line after the click, not the one after its start (#1696).
	Window window;
	window.app.call("snap.set", {{"type", "events"}});
	const double near = 3.0 * window.display.view().scale; // 3 dp in seconds, within the magnets' reach
	const auto edge = (16 * g_quarter) - std::llround(near * 2.0 * static_cast<double>(g_quarter)); // 8 s - near
	window.app.call("arranger.add", {{"position", 12 * g_quarter}, {"end", edge}});
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 1; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::Draw);

	window.app.ui().click(std::nullopt, {"", 0, window.at(8.0 + near)}, ui::MouseButton::Left, 1, {});
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 2; }));
	const Json drawn = window.events()[1];
	CHECK(std::abs(drawn.at("position").get<int64_t>() - edge) < g_quarter / 16);
	CHECK(drawn.at("end") == 20 * g_quarter);
}

TEST_CASE("the arranger track's lane says why the engine refused a draw", "[ui]")
{
	// #1597: the lane reports refused edits as the event display's own edits do.
	Window window;
	window.app.call("track.add", {{"type", "arranger"}});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::Draw);
	CHECK(window.element("#edit-refused").at("visible") == false);
	// With snap to bars, a drag within a bar starts and ends at its bar line: nothing to draw.
	window.app.ui().drag(std::nullopt, {"", 0, window.at(2.3)}, {"", 0, window.at(2.8)});
	REQUIRE(window.app.waitUntil([&] { return window.element("#edit-refused").at("visible") == true; }));
	CHECK(window.element("#edit-refused").value("text", std::string()).find("sample") != std::string::npos);
	CHECK(window.events().empty());
	CHECK(window.shown(".arranger-lane-event") == 0);
}
