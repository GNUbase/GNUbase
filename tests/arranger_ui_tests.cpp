// SPDX-License-Identifier: GPL-3.0-or-later
// The arranger events on the arranger track's lane in the project window (#1393, docs/features/arranger.md, reference
// manual pp. 404-405), driven with ui::Automation as MCP's UI tools drive them: shown with their names and lengths,
// drawn with the draw tool, moved, renamed and erased; and the global tracks' lanes' reads of the project.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <tuple>

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

namespace
{
	// How many project.get the UI sends while edit runs and until the event display's project passes done and no
	// project.get is out.
	size_t reads(Window& window, const std::function<void()>& edit, const std::function<bool(const Json&)>& done)
	{
		ui::EngineBinding& binding = window.app.appUi().binding();
		REQUIRE(window.app.waitUntil([&] { return binding.waiting("project.get") == 0; }));
		size_t count = 0;
		const ui::Subscription counted = binding.onResult("project.get", [&count](const engine::Result&) { ++count; });
		edit();
		REQUIRE(window.app.waitUntil(
			[&] { return done(window.display.project()) && binding.waiting("project.get") == 0; }));
		return count;
	}

	// The position of the event of that id in a section of the project ("transpose", "chords" or "arranger").
	Json position(const Json& project, const char* section, const Json& id)
	{
		if (project.is_object())
			for (const Json& event : project.at(section).at("events"))
				if (event.at("id") == id)
					return event.at("position");
		return nullptr;
	}
}

TEST_CASE("an edit on a global track's lane sends no project.get of its own", "[ui]")
{
	// #1757: the lanes show the project that the event display reads, so a move or a draw on them sends as many
	// project.get as the same command sent through the UI's binding, which only the other views' reads answer.
	Window window;
	window.app.call("track.add", {{"type", "transpose"}});
	window.app.call("track.add", {{"type", "chord"}});
	const Json transpose = window.app.call("transpose.add", {{"position", 0}, {"value", 3}}).at("event").at("id");
	const Json chord = window.app.call("chord.add", {{"position", 0}, {"chord", "C"}}).at("event").at("id");
	const Json arranger =
		window.app.call("arranger.add", {{"position", 0}, {"end", 4 * g_quarter}}).at("event").at("id");
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return window.shown(".transpose-event") == 1 && window.shown(".chord-event") == 1 &&
				window.shown(".arranger-lane-event") == 1 && !window.hasClass(".arranger-lane-row", "hidden");
		}));
	window.display.setTool(ui::Tool::ObjectSelection);
	ui::EngineBinding& binding = window.app.appUi().binding();
	const double bar = 2.0 / window.display.view().scale * g_ratio; // pixels
	// Drags the event that selector finds by a bar to the right.
	const auto drag = [&](const std::string& selector)
	{
		const Json box = window.box(selector);
		const double x = box.at("x").get<double>() + (box.at("width").get<double>() / 2.0);
		const int y = static_cast<int>(std::lround(box.at("y").get<double>() + (box.at("height").get<double>() / 2.0)));
		window.app.ui().drag(std::nullopt, {"", 0, ui::Point{static_cast<int>(std::lround(x)), y}},
			{"", 0, ui::Point{static_cast<int>(std::lround(x + bar)), y}});
	};

	for (const auto& [selector, section, id] :
		{std::tuple{".transpose-event", "transpose", transpose}, std::tuple{".chord-event", "chords", chord}})
	{
		INFO(section);
		const std::string command = std::string(section) == "chords" ? "chord.set" : "transpose.set";
		const size_t moved = reads(
			window, [&, selector = selector] { drag(selector); }, [&, section = section, id = id](const Json& project)
			{ return position(project, section, id) == 4 * g_quarter; });
		const size_t sent = reads(
			window,
			[&, id = id] { binding.send(command, {{"events", {{{"event", id}, {"position", 8 * g_quarter}}}}}); },
			[&, section = section, id = id](const Json& project)
			{ return position(project, section, id) == 8 * g_quarter; });
		CHECK(moved == sent);
	}

	// The arranger track's lane: a move, and a draw.
	const size_t moved = reads(
		window, [&] { window.app.ui().drag(std::nullopt, {"", 0, window.at(1.0)}, {"", 0, window.at(3.0)}); },
		[&](const Json& project) { return position(project, "arranger", arranger) == 4 * g_quarter; });
	const size_t set = reads(
		window, [&] { binding.send("arranger.set", {{"event", arranger}, {"position", 8 * g_quarter}}); },
		[&](const Json& project) { return position(project, "arranger", arranger) == 8 * g_quarter; });
	CHECK(moved == set);
	window.display.setTool(ui::Tool::Draw);
	const auto events = [](size_t count)
	{
		return [count](const Json& project)
		{ return project.is_object() && project.at("arranger").at("events").size() == count; };
	};
	const size_t drawn = reads(
		window, [&] { window.app.ui().drag(std::nullopt, {"", 0, window.at(6.3)}, {"", 0, window.at(7.8)}); },
		events(2));
	const size_t added = reads(
		window, [&] { binding.send("arranger.add", {{"position", 16 * g_quarter}, {"end", 20 * g_quarter}}); },
		events(3));
	CHECK(drawn == added);
	CHECK(window.events()[1].at("position") == 12 * g_quarter);
}

TEST_CASE("the arranger track's controls switch arranger mode and the chain and show the step playing", "[ui]")
{
	// The track list's arranger controls (reference manual p. 164): Activate Arranger Mode, Select Active Arranger
	// Chain, Current Item and Current Repeat, besides Switch Time Base.
	Window window;
	const Json a = window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}}).at("event").at("id");
	window.app.call("arranger.setChain", {{"steps", {{{"event", a}, {"repeats", 3}}}}});
	const Json first = window.app.call("arranger.get").at("chain");
	window.app.call("arranger.addChain", {{"name", "Short"}}); // the active chain now
	const std::string row = "listitem[type=arranger]";
	const auto click = [&window](const std::string& selector)
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto value = [&window](const std::string& selector)
	{ return window.element(selector).value("attributes", Json::object()).value("value", std::string()); };
	REQUIRE(window.app.waitUntil([&] { return window.shown(row + " select[control=arrangerChain]") == 1; }));
	CHECK(window.shown(row + " button[control=timeBase]") == 1);
	// In the default controls area Current Repeat sits beside Current Item, on the second line.
	CHECK(window.box(row + " [control=arrangerRepeat]").at("y") == window.box(row + " [control=arrangerItem]").at("y"));
	CHECK(window.box(row + " [control=arrangerItem]").at("y").get<double>() >
		window.box(row + " [control=arrangerChain]").at("y").get<double>());

	click(row + " button[control=arrangerMode]");
	REQUIRE(window.app.waitUntil([&] { return window.app.call("arranger.get").at("mode") == true; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass(row + " button[control=arrangerMode]", "checked"); }));

	// The first chain again, above the active one in the list.
	click(row + " select[control=arrangerChain]");
	window.app.ui().key(std::nullopt, "Up");
	click(row + " select[control=arrangerChain]");
	REQUIRE(window.app.waitUntil([&] { return window.app.call("arranger.get").at("chain") == first; }));

	// The event and the repeat playing; nothing outside the chain.
	CHECK(value(row + " [control=arrangerItem]").empty());
	window.app.call("arranger.goToStep", {{"step", 0}, {"repeat", 1}});
	CHECK(window.app.waitUntil([&] { return value(row + " [control=arrangerItem]") == "A"; }));
	CHECK(window.app.waitUntil([&] { return value(row + " [control=arrangerRepeat]") == "2/3"; }));

	// They only show: a double click doesn't edit them.
	for (const std::string& display : {row + " [control=arrangerItem]", row + " [control=arrangerRepeat]"})
	{
		window.app.ui().click(std::nullopt, {display, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		window.app.shell().step(std::chrono::milliseconds(0));
		CHECK_FALSE(window.hasClass(display, "editing"));
	}

	// Arranger mode off from elsewhere, which brings no project read: the button follows it, also after its own click,
	// and the displays are empty again.
	window.app.call("arranger.setMode", {{"mode", false}});
	CHECK(window.app.waitUntil([&] { return !window.hasClass(row + " button[control=arrangerMode]", "checked"); }));
	CHECK(window.app.waitUntil(
		[&]
		{
			return value(row + " [control=arrangerItem]").empty() && value(row + " [control=arrangerRepeat]").empty();
		}));
}
