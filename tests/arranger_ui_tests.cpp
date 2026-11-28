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
#include "ui/markers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

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

TEST_CASE(
	"Open Arranger Editor in the track list and on the inspector's Arranger Track section opens the editor", "[ui]")
{
	// Reference manual pp. 164, 405: both open the editor, and leave it open when it is.
	Window window;
	window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}});
	const std::string row = "listitem[type=arranger]";
	const auto click = [&window](const std::string& selector)
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	app::AppUi& appUi = window.app.appUi();
	const auto open = [&] { return window.app.shell().find(appUi.arrangerWindow()) != nullptr; };
	REQUIRE(window.app.waitUntil([&] { return window.shown(row + " button[control=arrangerEditor]") == 1; }));
	// In the default controls area it sits beside Current Item and Current Repeat, on the second line.
	CHECK(window.box(row + " button[control=arrangerEditor]").at("y") ==
		window.box(row + " [control=arrangerItem]").at("y"));
	click(row + " button[control=arrangerEditor]");
	REQUIRE(window.app.waitUntil(open));
	click(row + " button[control=arrangerEditor]");
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(open());
	appUi.toggleArranger();
	REQUIRE_FALSE(open());

	// The inspector's Arranger Track section has it on its header, which it doesn't close.
	window.app.appUi().docking().open("inspector");
	window.display.setTool(ui::Tool::ObjectSelection);
	click(row + " .track-name");
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [section=arranger]", "unavailable"); }));
	click(".inspector .inspector-arranger-editor");
	CHECK(window.app.waitUntil(open));
	CHECK_FALSE(window.hasClass(".inspector [section=arranger]", "collapsed"));
	appUi.toggleArranger();
	// The header itself closes and opens the section.
	click(".inspector [section=arranger] button.section-header");
	CHECK(window.hasClass(".inspector [section=arranger]", "collapsed"));
	CHECK_FALSE(window.hasClass(".inspector [section=arranger] .inspector-section-title", "collapsed"));
	click(".inspector [section=arranger] button.section-header");
	CHECK_FALSE(window.hasClass(".inspector [section=arranger]", "collapsed"));
}

TEST_CASE(
	"the inspector's Arranger Track section lists the chain and the events, plays them and sets the jump mode", "[ui]")
{
	// Reference manual pp. 405, 410-411: the Current Arranger Chain and Arranger Events lists, and Jump Mode.
	Window window;
	const Json a = window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}}).at("event").at("id");
	const Json b =
		window.app.call("arranger.add", {{"position", 8 * g_quarter}, {"end", 16 * g_quarter}}).at("event").at("id");
	window.app.call("arranger.setChain", {{"steps", {{{"event", b}, {"repeats", 2}}, {{"event", a}}}}});
	const std::string row = "listitem[type=arranger]";
	const auto click = [&window](const std::string& selector)
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto text = [&window](const std::string& selector) { return window.element(selector).value("text", ""); };
	// A name is a textdisplay, its text the value attribute.
	const auto name = [&window](const std::string& selector)
	{ return window.element(selector + " .name").value("attributes", Json::object()).value("value", std::string()); };
	const auto disabled = [&window](const std::string& selector)
	{ return window.element(selector).value("attributes", Json::object()).contains("disabled"); };
	REQUIRE(window.app.waitUntil([&] { return window.shown(row + " .track-name") == 1; }));
	window.app.appUi().docking().open("inspector");
	window.display.setTool(ui::Tool::ObjectSelection);
	click(row + " .track-name");
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [section=arranger]", "unavailable"); }));
	const std::string steps = ".inspector .inspector-arranger-steps ";
	const std::string events = ".inspector .inspector-arranger-events ";
	REQUIRE(window.shown(steps + ".arranger-row") == 2);
	CHECK(name(steps + "[step=\"0\"]") == "B");
	CHECK(window.element(steps + "[step=\"0\"] .repeats").at("attributes").value("value", std::string()) == "2");
	CHECK(name(steps + "[step=\"1\"]") == "A");
	REQUIRE(window.shown(events + ".arranger-row") == 2);
	CHECK(text(events + "[event=\"" + a.dump() + "\"] .name") == "A");

	// Outside arranger mode the arrows can't play anything: arranger.goToStep and arranger.trigger refuse.
	CHECK(disabled(steps + "[step=\"1\"] button.arranger-locate"));
	CHECK(disabled(events + "[event=\"" + a.dump() + "\"] button.arranger-trigger"));
	window.app.call("arranger.setMode", {{"mode", true}});
	REQUIRE(window.app.waitUntil([&] { return !disabled(steps + "[step=\"1\"] button.arranger-locate"); }));

	// A step's arrow plays from it, and its row shows it playing; an event's arrow triggers it.
	click(steps + "[step=\"1\"] button.arranger-locate");
	REQUIRE(window.app.waitUntil([&] { return window.app.call("transport.get").at("arranger").at("step") == 1; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass(steps + "[step=\"1\"]", "current"); }));
	CHECK_FALSE(window.hasClass(steps + "[step=\"0\"]", "current"));
	click(events + "[event=\"" + b.dump() + "\"] button.arranger-trigger");
	REQUIRE(window.app.waitUntil([&] { return window.app.call("transport.get").at("arranger").at("event") == b; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass(events + "[event=\"" + b.dump() + "\"]", "current"); }));

	// Jump Mode, and the list follows a change from elsewhere, which brings no project read.
	click(".inspector select[control=arrangerJumpMode]");
	window.app.ui().key(std::nullopt, "Down");
	click(".inspector select[control=arrangerJumpMode]");
	REQUIRE(window.app.waitUntil([&] { return window.app.call("arranger.get").at("jumpMode") == "4bars"; }));
	window.app.call("arranger.setMode", {{"jumpMode", "end"}});
	CHECK(window.app.waitUntil(
		[&]
		{
			return window.element(".inspector select[control=arrangerJumpMode]").value("value", std::string()) == "end";
		}));

	// Alt and a double click on a step's name rename its event (p. 405); a plain double click doesn't.
	window.app.ui().click(std::nullopt, {steps + "[step=\"1\"] .name", 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(window.hasClass(steps + "[step=\"1\"] .name", "editing"));
	window.app.ui().click(
		std::nullopt, {steps + "[step=\"1\"] .name", 0, std::nullopt}, ui::MouseButton::Left, 2, {"alt"});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(steps + "[step=\"1\"] .name", "editing"); }));
	// A refresh that changes nothing the lists show doesn't build them again, so the name being typed stays.
	window.app.call("arranger.setMode", {{"jumpMode", "1bar"}});
	window.app.call("transport.locate", {{"position", g_quarter}});
	CHECK(window.app.waitUntil(
		[&]
		{
			return window.element(".inspector select[control=arrangerJumpMode]").value("value", std::string()) ==
				"1bar";
		}));
	for (int i = 0; i < 5; ++i)
		window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.hasClass(steps + "[step=\"1\"] .name", "editing"));
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "Intro");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("name") == "Intro"; }));
	CHECK(window.app.waitUntil([&] { return text(events + "[event=\"" + a.dump() + "\"] .name") == "Intro"; }));
}

TEST_CASE("arranger events on the lane are selected with clicks and resized by their edges", "[ui]")
{
	// Reference manual p. 404: arranger events are selected and resized as events are.
	Window window;
	const Json a = window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}}).at("event").at("id");
	const Json b =
		window.app.call("arranger.add", {{"position", 8 * g_quarter}, {"end", 12 * g_quarter}}).at("event").at("id");
	const std::string eventA = ".arranger-lane-event[event=\"" + a.dump() + "\"]";
	const std::string eventB = ".arranger-lane-event[event=\"" + b.dump() + "\"]";
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 2; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::ObjectSelection);
	const auto click = [&window](ui::Point at, const std::vector<std::string>& modifiers)
	{
		window.app.ui().click(std::nullopt, {"", 0, at}, ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// A click selects one, Ctrl adds another, and a press beside them selects none.
	click(window.at(1.0), {});
	CHECK(window.hasClass(eventA, "selected"));
	CHECK_FALSE(window.hasClass(eventB, "selected"));
	click(window.at(5.0), {"ctrl"});
	CHECK(window.hasClass(eventA, "selected"));
	CHECK(window.hasClass(eventB, "selected"));
	// Ctrl on a selected one removes it.
	click(window.at(1.0), {"ctrl"});
	CHECK_FALSE(window.hasClass(eventA, "selected"));
	CHECK(window.hasClass(eventB, "selected"));
	// Shift removes or adds one as Ctrl does (clicking another each time: two on one are a double click).
	click(window.at(5.0), {"shift"});
	CHECK_FALSE(window.hasClass(eventA, "selected"));
	CHECK_FALSE(window.hasClass(eventB, "selected"));
	click(window.at(1.0), {"shift"});
	CHECK(window.hasClass(eventA, "selected"));
	CHECK_FALSE(window.hasClass(eventB, "selected"));
	click(window.at(5.0), {"shift"});
	CHECK(window.hasClass(eventA, "selected"));
	CHECK(window.hasClass(eventB, "selected"));
	click(window.at(7.0), {});
	CHECK_FALSE(window.hasClass(eventA, "selected"));
	CHECK_FALSE(window.hasClass(eventB, "selected"));

	// A drag of an edge resizes the event, snapped to bars: the end of A, then the start of B.
	const auto edge = [&window](const std::string& event, const char* which)
	{
		const Json box = window.box(event + " .arranger-lane-edge[edge=" + which + "]");
		return ui::Point{static_cast<int>(std::lround(box.at("x").get<double>() + (box.at("width").get<double>() / 2))),
			static_cast<int>(std::lround(box.at("y").get<double>() + (box.at("height").get<double>() / 2)))};
	};
	window.app.ui().drag(std::nullopt, {"", 0, edge(eventA, "end")}, {"", 0, window.at(2.3)});
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("end") == 4 * g_quarter; }));
	CHECK(window.events()[0].at("position") == 0);
	window.app.ui().drag(std::nullopt, {"", 0, edge(eventB, "start")}, {"", 0, window.at(2.2)});
	REQUIRE(window.app.waitUntil([&] { return window.events()[1].at("position") == 4 * g_quarter; }));
	CHECK(window.events()[1].at("end") == 12 * g_quarter);
	// The lane shows B from its new start.
	CHECK(window.app.waitUntil(
		[&] { return std::abs(window.box(eventB).at("x").get<double>() - window.at(2.0).x) < 2.0; }));
}

TEST_CASE("the inspector's Current Arranger Chain list edits repeats and modes and removes steps", "[ui]")
{
	// Reference manual p. 409: the Repeats and Mode columns, and Remove Touched on a step's context menu.
	Window window;
	const Json a = window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}}).at("event").at("id");
	const Json b =
		window.app.call("arranger.add", {{"position", 8 * g_quarter}, {"end", 16 * g_quarter}}).at("event").at("id");
	window.app.call("arranger.setChain", {{"steps", {{{"event", b}, {"repeats", 2}}, {{"event", a}}}}});
	const auto chain = [&window]
	{
		const Json arranger = window.app.call("arranger.get");
		for (const Json& each : arranger.at("chains"))
			if (each.at("id") == arranger.at("chain"))
				return each.at("steps");
		return Json::array();
	};
	const auto click = [&window](const std::string& selector, ui::MouseButton button, int count)
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, button, count, {});
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const std::string row = "listitem[type=arranger]";
	REQUIRE(window.app.waitUntil([&] { return window.shown(row + " .track-name") == 1; }));
	window.app.appUi().docking().open("inspector");
	window.display.setTool(ui::Tool::ObjectSelection);
	click(row + " .track-name", ui::MouseButton::Left, 1);
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [section=arranger]", "unavailable"); }));
	const std::string steps = ".inspector .inspector-arranger-steps ";
	REQUIRE(window.app.waitUntil([&] { return window.shown(steps + ".arranger-row") == 2; }));

	// A double click on the repeats types them, capped to 999.
	const auto type = [&](const std::string& text)
	{
		click(steps + "[step=\"0\"] .repeats", ui::MouseButton::Left, 2);
		REQUIRE(window.app.waitUntil([&] { return window.hasClass(steps + "[step=\"0\"] .repeats", "editing"); }));
		// The list isn't built again on later frames, which would replace the field being typed.
		for (int frame = 0; frame < 5; ++frame)
			window.app.shell().step(std::chrono::milliseconds(16));
		REQUIRE(window.hasClass(steps + "[step=\"0\"] .repeats", "editing"));
		window.app.ui().key(std::nullopt, "Mod+A");
		window.app.ui().text(std::nullopt, text);
		window.app.ui().key(std::nullopt, "Enter");
	};
	const auto shows = [&](const std::string& repeats)
	{
		return window.app.waitUntil(
			[&]
			{
				return window.element(steps + "[step=\"0\"] .repeats").at("attributes").value("value", std::string()) ==
					repeats;
			});
	};
	type("3");
	REQUIRE(window.app.waitUntil([&] { return chain()[0].at("repeats") == 3; }));
	CHECK(shows("3"));
	type("1200");
	REQUIRE(window.app.waitUntil([&] { return chain()[0].at("repeats") == 999; }));
	REQUIRE(shows("999"));
	// Text that isn't a number keeps the repeats.
	type("many");
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(shows("999"));
	CHECK(chain()[0].at("repeats") == 999);

	// The mode column picks the repeat mode.
	click(steps + "[step=\"1\"] select.mode", ui::MouseButton::Left, 1);
	window.app.ui().key(std::nullopt, "Down");
	click(steps + "[step=\"1\"] select.mode", ui::MouseButton::Left, 1);
	REQUIRE(window.app.waitUntil([&] { return chain()[1].at("mode") == "repeatForever"; }));
	CHECK(chain()[0].at("mode") == "normal");

	// Remove Touched removes the step right-clicked.
	click(steps + "[step=\"0\"] .name", ui::MouseButton::Right, 1);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#inspector-arranger-step-menu", "open"); }));
	click("#inspector-arranger-step-menu .arranger-remove-touched", ui::MouseButton::Left, 1);
	REQUIRE(window.app.waitUntil([&] { return chain().size() == 1; }));
	CHECK(chain()[0].at("event") == a);
	CHECK(chain()[0].at("mode") == "repeatForever");
	CHECK(window.app.waitUntil([&] { return window.shown(steps + ".arranger-row") == 1; }));
}

TEST_CASE("the info line shows the arranger events selected on the lane and renames them", "[ui]")
{
	// Reference manual p. 405: an arranger event selected is renamed on the info line.
	Window window;
	window.app.appUi().docking().setVisible("project", "infoLine", true);
	window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}});
	window.app.call("arranger.add", {{"position", 8 * g_quarter}, {"end", 12 * g_quarter}});
	const uint64_t track = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const uint64_t part = window.app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_quarter}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.shown(".arranger-lane-event") == 2; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::ObjectSelection);
	const auto click = [&window](ui::Point at, const std::vector<std::string>& modifiers)
	{
		window.app.ui().click(std::nullopt, {"", 0, at}, ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto field = [](const std::string& name) { return "#info-line textdisplay[field=" + name + "]"; };
	const auto shows = [&window, &field](const std::string& name, const std::string& value)
	{
		return window.app.waitUntil(
			[&] { return window.element(field(name)).at("attributes").value("value", "") == value; });
	};
	const auto type = [&window, &field](const std::string& name, const std::string& text)
	{
		window.app.ui().click(std::nullopt, {field(name), 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		window.app.ui().key(std::nullopt, "Mod+A");
		window.app.ui().text(std::nullopt, text);
		window.app.ui().key(std::nullopt, "Enter");
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto label = [&window](const std::string& name)
	{ return window.element("#info-line [info-field=" + name + "] .info-label").at("text").get<std::string>(); };

	// A click on A shows it: its name, start, end and length.
	click(window.at(1.0), {});
	REQUIRE(shows("name", "A"));
	CHECK(window.hasClass("#info-line", "arranger"));
	CHECK_FALSE(window.hasClass("#info-line", "markers"));
	CHECK(label("name") == "Name");
	CHECK(label("start") == "Start");
	CHECK(shows("start", "1.1.1.0"));
	CHECK(shows("end", "3.1.1.0"));
	CHECK(shows("length", "2.0.0.0"));
	CHECK(window.hasClass("#info-line [info-field=mute]", "unavailable"));
	CHECK_FALSE(window.hasClass("#info-line [info-field=end]", "unavailable"));

	// Each field edits it with arranger.set, a new start keeping its length.
	const size_t steps = window.app.call("history.get").at("steps").size();
	type("name", "Intro");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("name") == "Intro"; }));
	type("end", "2.1.1.0");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("end") == 4 * g_quarter; }));
	// The info line edits from the events it shows, which follow the engine's a step or more later (#929).
	REQUIRE(shows("end", "2.1.1.0"));
	type("start", "1.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("position") == 2 * g_quarter; }));
	CHECK(window.events()[0].at("end") == 6 * g_quarter);
	CHECK(window.app.call("history.get").at("steps").size() == steps + 3);

	// With B selected too, a name goes to both in one undo step.
	REQUIRE(shows("start", "1.3.1.0"));
	click(window.at(5.0), {"ctrl"});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#info-line", "multiple"); }));
	type("name", "Chorus");
	REQUIRE(window.app.waitUntil([&] { return window.events()[1].at("name") == "Chorus"; }));
	CHECK(window.events()[0].at("name") == "Chorus");
	CHECK(window.app.call("history.get").at("steps").size() == steps + 4);

	// An empty name leaves their names as they are: the next name is the only undo step more.
	window.app.ui().click(std::nullopt, {field("name"), 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().key(std::nullopt, "Backspace");
	window.app.ui().key(std::nullopt, "Enter");
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(shows("name", "Chorus"));
	type("name", "Bridge");
	REQUIRE(window.app.waitUntil([&] { return window.events()[1].at("name") == "Bridge"; }));
	CHECK(window.events()[0].at("name") == "Bridge");
	CHECK(window.app.call("history.get").at("steps").size() == steps + 5);

	// A marker selected deselects the arranger events: the info line shows the marker, with its description.
	window.app.call("track.add", {{"type", "marker"}});
	const uint64_t marker =
		window.app.call("marker.add", {{"position", 0}, {"name", "Verse"}}).at("marker").at("id").get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.shown(".marker-label") == 1; }));
	window.app.appUi().markerLines()->pick({marker});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#info-line", "markers"); }));
	CHECK_FALSE(window.hasClass("#info-line", "arranger"));
	CHECK(label("name") == "Description");
	CHECK(shows("name", "Verse"));
	CHECK(window.shown(".arranger-lane-event.selected") == 0);

	// Selecting a part shows the part and deselects the arranger events.
	click(window.at(1.0), {});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#info-line", "arranger"); }));
	window.display.select({part});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "arranger"); }));
	CHECK(label("name") == "Name");
	CHECK(window.shown(".arranger-lane-event.selected") == 0);
}

TEST_CASE("the inspector's Current Arranger Chain list selects steps and removes the selected ones", "[ui]")
{
	// Reference manual p. 410: Ctrl (Cmd) or Shift and a click select several steps, Remove Selected removes them.
	Window window;
	const Json a = window.app.call("arranger.add", {{"position", 0}, {"end", 8 * g_quarter}}).at("event").at("id");
	const Json b =
		window.app.call("arranger.add", {{"position", 8 * g_quarter}, {"end", 16 * g_quarter}}).at("event").at("id");
	window.app.call("arranger.setChain", {{"steps", {{{"event", a}}, {{"event", b}}, {{"event", a}}, {{"event", b}}}}});
	window.app.call("arranger.setMode", {{"mode", true}});
	const auto chain = [&window]
	{
		const Json arranger = window.app.call("arranger.get");
		for (const Json& each : arranger.at("chains"))
			if (each.at("id") == arranger.at("chain"))
				return each.at("steps");
		return Json::array();
	};
	const auto click =
		[&window](const std::string& selector, ui::MouseButton button, const std::vector<std::string>& modifiers = {})
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, button, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const std::string row = "listitem[type=arranger]";
	REQUIRE(window.app.waitUntil([&] { return window.shown(row + " .track-name") == 1; }));
	window.app.appUi().docking().open("inspector");
	window.display.setTool(ui::Tool::ObjectSelection);
	click(row + " .track-name", ui::MouseButton::Left);
	const std::string steps = ".inspector .inspector-arranger-steps ";
	REQUIRE(window.app.waitUntil([&] { return window.shown(steps + ".arranger-row") == 4; }));
	const auto selected = [&]
	{
		window.app.shell().step(std::chrono::milliseconds(0));
		std::string rows;
		for (int step = 0; step < 4; ++step)
			rows += window.hasClass(steps + "[step=\"" + std::to_string(step) + "\"]", "selected") ? "x" : "-";
		return rows;
	};
	const auto step = [&steps](int index) { return steps + "[step=\"" + std::to_string(index) + "\"] .name"; };

	// A click selects a step, also beside its name, Shift up to another one, Ctrl (Cmd) adds or removes one.
	const auto beside = [&window, &step](int index)
	{
		const Json name = window.box(step(index));
		return ui::Point{static_cast<int>(std::floor(name.at("x").get<double>())) - 1,
			static_cast<int>(std::lround(name.at("y").get<double>() + (name.at("height").get<double>() / 2.0)))};
	};
	window.app.ui().click(std::nullopt, {"", 0, beside(1)}, ui::MouseButton::Left, 1, {});
	CHECK(selected() == "-x--");
	click(step(3), ui::MouseButton::Left, {"shift"});
	CHECK(selected() == "-xxx");
	click(step(2), ui::MouseButton::Left, {"ctrl"});
	CHECK(selected() == "-x-x");
	click(step(0), ui::MouseButton::Left, {"ctrl"});
	CHECK(selected() == "xx-x");
	// The arrow plays from its step and leaves the selection as it is.
	click(steps + "[step=\"2\"] .arranger-locate", ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(steps + "[step=\"2\"]", "current"); }));
	CHECK(selected() == "xx-x");

	// Remove Selected removes them, and the steps left are selected none.
	const auto removeSelected = std::string("#inspector-arranger-step-menu .arranger-remove-selected");
	const auto disabled = [&window](const std::string& item)
	{ return window.element(item).value("attributes", Json::object()).contains("disabled"); };
	// A right click beside a step's name touches the step too, for Remove Touched.
	window.app.ui().click(std::nullopt, {"", 0, beside(2)}, ui::MouseButton::Right, 1, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#inspector-arranger-step-menu", "open"); }));
	CHECK_FALSE(disabled("#inspector-arranger-step-menu .arranger-remove-touched"));
	CHECK_FALSE(disabled(removeSelected));
	click(removeSelected, ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return chain().size() == 1; }));
	CHECK(chain()[0].at("event") == a);
	REQUIRE(window.app.waitUntil([&] { return window.shown(steps + ".arranger-row") == 1; }));
	CHECK_FALSE(window.hasClass(steps + "[step=\"0\"]", "selected"));
	// Without a selection Remove Selected is disabled.
	click(step(0), ui::MouseButton::Right);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#inspector-arranger-step-menu", "open"); }));
	CHECK(window.element(removeSelected).value("attributes", Json::object()).contains("disabled"));
}

TEST_CASE("arranger events selected on the lane deselect the rest and Delete and a drag act on all selected", "[ui]")
{
	// #2076, reference manual p. 404: arranger events are selected, moved and deleted as events are.
	Window window;
	const auto add = [&window](int64_t from, int64_t to)
	{ return window.app.call("arranger.add", {{"position", from}, {"end", to}}).at("event").at("id").get<uint64_t>(); };
	const uint64_t a = add(0, 4 * g_quarter);
	const uint64_t b = add(4 * g_quarter, 8 * g_quarter);
	const uint64_t c = add(8 * g_quarter, 12 * g_quarter);
	const auto selected = [&window](uint64_t event)
	{ return window.hasClass(".arranger-lane-event[event=\"" + std::to_string(event) + "\"]", "selected"); };
	const auto position = [&window](uint64_t event)
	{
		for (const Json& each : window.events())
			if (each.at("id") == event)
				return each.at("position").get<int64_t>();
		return int64_t{-1};
	};
	const uint64_t track = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const uint64_t part = window.app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_quarter}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	window.app.call("track.add", {{"type", "marker"}});
	const uint64_t marker = window.app.call("marker.add", {{"position", 0}}).at("marker").at("id").get<uint64_t>();
	window.app.call("track.add", {{"type", "signature"}});
	window.app.call("signature.add", {{"bar", 4}, {"numerator", 3}, {"denominator", 4}});
	ui::MarkerLines& markers = *window.app.appUi().markerLines();
	ui::SignatureRow& signatures = window.app.appUi().signatureLane()->row();
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return window.shown(".arranger-lane-event") == 3 && !window.hasClass(".arranger-lane-row", "hidden") &&
				window.shown(".signature-event") == 2 && !markers.loading();
		}));
	window.display.setTool(ui::Tool::ObjectSelection);
	// A click on an arranger event, another one each time, as two soon after each other on one open it.
	const auto click = [&window](ui::Point at, const std::vector<std::string>& modifiers)
	{
		window.app.ui().click(std::nullopt, {"", 0, at}, ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// An arranger event selected deselects the events, the markers, the time signature events and the range.
	window.display.select({part});
	window.app.shell().step(std::chrono::milliseconds(0));
	click(window.at(1.0), {});
	CHECK(selected(a));
	CHECK(window.display.selection().empty());
	markers.pick({marker});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(selected(a));
	click(window.at(3.0), {});
	CHECK(selected(b));
	CHECK(markers.selection().empty());
	signatures.setSelection({4});
	click(window.at(5.0), {});
	CHECK(selected(c));
	CHECK(signatures.selection().empty());
	window.display.setRange(ui::RangeSelection{0, 48000, {track}});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.app.waitUntil([&] { return !selected(c); })); // the range deselects them, as it does markers
	click(window.at(1.0), {});
	CHECK(selected(a));
	CHECK_FALSE(window.display.range().has_value());
	// And a time signature event selected deselects them.
	window.app.ui().click(std::nullopt, {".signature-event", 1, std::nullopt}, ui::MouseButton::Left, 1, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(signatures.selection().size() == 1);
	CHECK_FALSE(selected(a));

	// A drag of one of the selected moves all selected, in one undo step, and they follow the mouse.
	click(window.at(5.0), {});
	click(window.at(1.0), {"ctrl"});
	REQUIRE(signatures.selection().empty());
	const size_t steps = window.app.call("history.get").at("steps").size();
	window.app.ui().press(std::nullopt, {"", 0, window.at(5.0)}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, {"", 0, window.at(7.2)});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.app.waitUntil(
		[&]
		{
			return std::abs(
					   window.box(".arranger-lane-event[event=\"" + std::to_string(a) + "\"]").at("x").get<double>() -
					   window.at(2.0).x) < 2.0;
		}));
	window.app.ui().release(std::nullopt, {"", 0, window.at(7.2)}, ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return position(c) == 12 * g_quarter; }));
	CHECK(position(a) == 4 * g_quarter);
	CHECK(position(b) == 4 * g_quarter); // B wasn't selected
	CHECK(window.app.call("history.get").at("steps").size() == steps + 1);

	// Delete removes the selected arranger events, in one undo step, and nothing else.
	window.app.keys().run("edit.delete", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 1; }));
	CHECK(window.events()[0].at("id") == b);
	CHECK(window.app.call("history.get").at("steps").size() == steps + 2);
	CHECK(window.app.call("project.get").at("markers").size() == 1);
	CHECK(window.app.call("project.get").at("midiParts").size() == 1);
}

TEST_CASE("the inspector's Current Arranger Chain list moves and copies steps dragged and takes events dropped", "[ui]")
{
	// Reference manual pp. 409-410: steps dragged up or down move, with Alt copy, and events dropped go in there.
	Window window;
	const auto add = [&window](int64_t from, int64_t to)
	{ return window.app.call("arranger.add", {{"position", from}, {"end", to}}).at("event").at("id").get<uint64_t>(); };
	const uint64_t a = add(0, 4 * g_quarter);
	const uint64_t b = add(4 * g_quarter, 8 * g_quarter);
	const uint64_t c = add(8 * g_quarter, 12 * g_quarter);
	window.app.call("arranger.setChain", {{"steps", {{{"event", a}}, {{"event", b}}, {{"event", c}}}}});
	const auto order = [&window]
	{
		const Json arranger = window.app.call("arranger.get");
		std::vector<uint64_t> events;
		for (const Json& chain : arranger.at("chains"))
			if (chain.at("id") == arranger.at("chain"))
				for (const Json& step : chain.at("steps"))
					events.push_back(step.at("event").get<uint64_t>());
		return events;
	};
	const std::string track = "listitem[type=arranger] .track-name";
	REQUIRE(window.app.waitUntil([&] { return window.shown(track) == 1; }));
	window.app.appUi().docking().open("inspector");
	window.display.setTool(ui::Tool::ObjectSelection);
	window.app.ui().click(std::nullopt, {track, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	const std::string steps = ".inspector .inspector-arranger-steps ";
	const auto row = [&steps](int index) { return steps + "[step=\"" + std::to_string(index) + "\"]"; };
	// The list shows the chain's steps: their names in order.
	const auto shows = [&window, &steps, &row](const std::string& names)
	{
		return window.app.waitUntil(
			[&]
			{
				std::string shown;
				for (int index = 0; index < static_cast<int>(window.shown(steps + ".arranger-row")); ++index)
					shown += window.element(row(index) + " .name").at("attributes").value("value", "");
				return shown == names;
			});
	};
	REQUIRE(shows("ABC"));
	// A point in a row's name, a fraction of the way down.
	const auto at = [&window](const std::string& selector, double fraction)
	{
		const Json box = window.box(selector + " .name");
		return ui::Point{static_cast<int>(std::lround(box.at("x").get<double>() + (box.at("width").get<double>() / 2))),
			static_cast<int>(std::lround(box.at("y").get<double>() + (box.at("height").get<double>() * fraction)))};
	};
	const auto drag = [&window](ui::Point from, ui::Point to, const std::vector<std::string>& modifiers = {})
	{
		window.app.ui().drag(std::nullopt, {"", 0, from}, {"", 0, to}, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// On a step's lower half the step dragged goes after it, on its upper half before it.
	drag(at(row(0), 0.5), at(row(2), 0.75));
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{b, c, a}; }));
	REQUIRE(shows("BCA"));
	drag(at(row(1), 0.5), at(row(0), 0.25));
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{c, b, a}; }));
	REQUIRE(shows("CBA"));
	// With Alt it is copied.
	drag(at(row(2), 0.5), at(row(0), 0.25), {"alt"});
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{a, c, b, a}; }));
	REQUIRE(shows("ACBA"));
	// The selected steps move together when one of them is dragged.
	window.app.ui().click(std::nullopt, {"", 0, at(row(0), 0.5)}, ui::MouseButton::Left, 1, {});
	window.app.ui().click(std::nullopt, {"", 0, at(row(2), 0.5)}, ui::MouseButton::Left, 1, {"ctrl"});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.hasClass(row(2), "selected"));
	drag(at(row(2), 0.5), at(row(3), 0.75));
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{c, a, a, b}; }));
	REQUIRE(shows("CAAB"));

	// An event dropped from the Arranger Events list goes in where it is dropped.
	const size_t undoSteps = window.app.call("history.get").at("steps").size();
	drag(at(".inspector .inspector-arranger-events [event=\"" + std::to_string(b) + "\"]", 0.5), at(row(1), 0.25));
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{c, b, a, a, b}; }));
	REQUIRE(shows("CBAAB"));
	// A drag that ends on the step it started on, or outside the chain list, changes nothing.
	drag(at(row(1), 0.25), at(row(1), 0.75));
	drag(at(row(1), 0.5), at(".inspector .inspector-arranger-events [event=\"" + std::to_string(a) + "\"]", 0.5));
	CHECK(window.app.call("history.get").at("steps").size() == undoSteps + 1);
	CHECK(order() == std::vector<uint64_t>{c, b, a, a, b});
}

TEST_CASE("arranger events dragged from the lane drop into the inspector's Current Arranger Chain list", "[ui]")
{
	// Reference manual p. 409: an arranger event dragged from the project window goes into the chain where it is
	// dropped and stays where it is; one of the events selected brings them all, in timeline order.
	Window window;
	const auto add = [&window](int64_t from, int64_t to)
	{ return window.app.call("arranger.add", {{"position", from}, {"end", to}}).at("event").at("id").get<uint64_t>(); };
	const uint64_t a = add(0, 4 * g_quarter);
	const uint64_t b = add(4 * g_quarter, 8 * g_quarter);
	const uint64_t c = add(8 * g_quarter, 12 * g_quarter);
	window.app.call("arranger.setChain", {{"steps", {{{"event", a}}, {{"event", b}}}}});
	const auto order = [&window]
	{
		const Json arranger = window.app.call("arranger.get");
		std::vector<uint64_t> events;
		for (const Json& chain : arranger.at("chains"))
			if (chain.at("id") == arranger.at("chain"))
				for (const Json& step : chain.at("steps"))
					events.push_back(step.at("event").get<uint64_t>());
		return events;
	};
	const std::string track = "listitem[type=arranger] .track-name";
	REQUIRE(
		window.app.waitUntil([&] { return window.shown(track) == 1 && window.shown(".arranger-lane-event") == 3; }));
	window.app.appUi().docking().open("inspector");
	window.display.setTool(ui::Tool::ObjectSelection);
	window.app.ui().click(std::nullopt, {track, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	const auto row = [](int index)
	{ return ".inspector .inspector-arranger-steps [step=\"" + std::to_string(index) + "\"] .name"; };
	REQUIRE(window.app.waitUntil([&] { return window.shown(row(1)) == 1; }));
	const auto upper = [&window](const std::string& selector)
	{
		const Json box = window.box(selector);
		return ui::Point{static_cast<int>(std::lround(box.at("x").get<double>() + (box.at("width").get<double>() / 2))),
			static_cast<int>(std::lround(box.at("y").get<double>() + (box.at("height").get<double>() / 4)))};
	};
	const auto drag = [&window](ui::Point from, ui::Point to, const std::vector<std::string>& modifiers = {})
	{
		window.app.ui().drag(std::nullopt, {"", 0, from}, {"", 0, to}, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const Json before = window.events();

	// C dropped on A's upper half goes before it, and stays where it is on the lane.
	drag(window.at(5.0), upper(row(0)));
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{c, a, b}; }));
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.events() == before);

	// A and C selected on the lane, A dragged onto B's upper half: A and C go in before it, in timeline order. Each
	// press is on another event than the one before, as two soon after each other on one open it.
	window.app.ui().click(std::nullopt, {"", 0, window.at(1.0)}, ui::MouseButton::Left, 1, {});
	window.app.ui().click(std::nullopt, {"", 0, window.at(5.0)}, ui::MouseButton::Left, 1, {"ctrl"});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.shown(row(2)) == 1; }));
	drag(window.at(1.0), upper(row(2)));
	REQUIRE(window.app.waitUntil([&] { return order() == std::vector<uint64_t>{c, a, a, c, b}; }));
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.events() == before);
}

TEST_CASE("arranger events selected on the lane deselect the automation points and points deselect them", "[ui]")
{
	// #2076: automation points are among the selections that arranger events selected deselect, and back.
	Window window;
	window.app.call("arranger.add", {{"position", 0}, {"end", 4 * g_quarter}});
	const uint64_t track = window.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const auto lane = window.app.call("automationLane.add", {{"track", track}, {"target", {{"type", "volume"}}}})
						  .at("lane")
						  .at("id")
						  .get<uint64_t>();
	window.app.call("automationPoint.add", {{"lane", lane}, {"points", {{{"position", g_quarter}, {"value", -6.0}}}}});
	const uint64_t point = window.app.call("automationLane.list", {{"track", track}})
							   .at("lanes")
							   .at(0)
							   .at("points")
							   .at(0)
							   .at("id")
							   .get<uint64_t>();
	window.display.showAutomation(track, true);
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return window.shown(".arranger-lane-event") == 1 && !window.hasClass(".arranger-lane-row", "hidden") &&
				window.display.automationTargets(track) != nullptr &&
				!window.display.project().at("automation").empty();
		}));
	window.display.setTool(ui::Tool::ObjectSelection);
	const auto selected = [&window] { return window.hasClass(".arranger-lane-event", "selected"); };
	const auto selectPoint = [&]
	{
		window.display.selectPoints({{lane, {point}}});
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// Points selected, then a click on an arranger event: no points stay selected.
	selectPoint();
	REQUIRE_FALSE(window.display.selectedPoints().empty());
	window.app.ui().click(std::nullopt, {"", 0, window.at(1.0)}, ui::MouseButton::Left, 1, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(selected());
	CHECK(window.display.selectedPoints().empty());
	// The arranger event selected, then points: the arranger event goes.
	selectPoint();
	CHECK(window.app.waitUntil([&] { return !selected(); }));
	CHECK_FALSE(window.display.selectedPoints().empty());
}

TEST_CASE("a drag of selected arranger events, one at the project start, to the left changes nothing", "[ui]")
{
	// #2076: none of the arranger events dragged goes before the project start, so with one there they stay.
	Window window;
	const auto add = [&window](int64_t from, int64_t to)
	{ return window.app.call("arranger.add", {{"position", from}, {"end", to}}).at("event").at("id").get<uint64_t>(); };
	const uint64_t a = add(0, 4 * g_quarter);
	const uint64_t b = add(4 * g_quarter, 8 * g_quarter);
	REQUIRE(window.app.waitUntil(
		[&] { return window.shown(".arranger-lane-event") == 2 && !window.hasClass(".arranger-lane-row", "hidden"); }));
	window.display.setTool(ui::Tool::ObjectSelection);
	const auto x = [&window](uint64_t event)
	{ return window.box(".arranger-lane-event[event=\"" + std::to_string(event) + "\"]").at("x").get<double>(); };
	const auto showsWhereItIs = [&]
	{ return std::abs(x(a) - window.at(0.0).x) < 2.0 && std::abs(x(b) - window.at(2.0).x) < 2.0; };
	// B, then A with Ctrl: a press on B right after a click on it would be a double click.
	window.app.ui().click(std::nullopt, {"", 0, window.at(3.0)}, ui::MouseButton::Left, 1, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	window.app.ui().click(std::nullopt, {"", 0, window.at(1.0)}, ui::MouseButton::Left, 1, {"ctrl"});
	window.app.shell().step(std::chrono::milliseconds(0));
	const auto selected = [&window](uint64_t event)
	{ return window.hasClass(".arranger-lane-event[event=\"" + std::to_string(event) + "\"]", "selected"); };
	REQUIRE((selected(a) && selected(b)));
	const size_t steps = window.app.call("history.get").at("steps").size();

	// B dragged a bar to the left would take A before the project start: both show where they are.
	window.app.ui().press(std::nullopt, {"", 0, window.at(3.0)}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, {"", 0, window.at(1.0)});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(showsWhereItIs());
	window.app.ui().release(std::nullopt, {"", 0, window.at(1.0)}, ui::MouseButton::Left);
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.events()[0].at("position") == 0);
	CHECK(window.events()[1].at("position") == 4 * g_quarter);
	CHECK(window.app.call("history.get").at("steps").size() == steps);
	CHECK(showsWhereItIs());
}
