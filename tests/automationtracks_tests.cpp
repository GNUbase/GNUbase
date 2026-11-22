// SPDX-License-Identifier: GPL-3.0-or-later
// Automation tracks (#121, docs/features/automation-tracks.md) in the app's project window, driven with ui::Automation
// as MCP's UI tools drive it: showing and hiding them, the parameter selector, the drawing and line tools, selecting,
// moving and scaling points, and the static value line.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/markers.h"
#include "ui/signaturetrack.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Element.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool hasClass(const Json& element, const std::string& name)
	{
		const Json classes = element.value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
	}

	// Whether the first element that the selector finds has the focus.
	bool focused(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		if (found.empty())
			return false;
		const Json pseudoClasses = found.at(0).value("pseudoClasses", Json::array());
		return std::find(pseudoClasses.begin(), pseudoClasses.end(), "focus") != pseudoClasses.end();
	}

	std::string row(uint64_t track)
	{
		return "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	}

	// The project window, at a dp ratio of 1, with an audio track on a linear time base, so that points are in
	// samples, and snapping off.
	struct Automation
	{
		Automation() : display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			app.shell().step(std::chrono::milliseconds(0));
			track = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
			app.call("track.set", {{"track", track}, {"timeBase", "linear"}});
			app.call("snap.set", {{"on", false}});
			REQUIRE(app.waitUntil([&] { return elements(app, row(track) + " .automation-toggle").size() == 1; }));
		}

		void click(const std::string& selector, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void clickAt(int x, int y, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(std::nullopt, {"", 0, ui::Point{x, y}}, ui::MouseButton::Left, 1, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void drag(int x0, int y0, int x1, int y1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().drag(std::nullopt, {"", 0, ui::Point{x0, y0}}, {"", 0, ui::Point{x1, y1}}, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json lanes() { return app.call("automationLane.list", {{"track", track}}).at("lanes"); }

		Json lane(const Json& id)
		{
			const Json all = lanes();
			const auto found =
				std::find_if(all.begin(), all.end(), [&id](const Json& lane) { return lane.at("id") == id; });
			REQUIRE(found != all.end());
			return *found;
		}

		// Shows the track's automation and waits for its first automation track, volume; returns the lane it shows.
		Json showVolume()
		{
			click(row(track) + " .automation-toggle");
			REQUIRE(app.waitUntil([&] { return elements(app, ".automation-row .automation-parameter").size() == 1; }));
			REQUIRE(app.waitUntil(
				[&] { return elements(app, ".automation-parameter").at(0).value("text", "") == "Volume"; }));
			REQUIRE(display.laneAutomation().size() == 2);
			return display.laneAutomation()[1];
		}

		// The same with a volume lane without points that is already in the project and doesn't read (#1918).
		Json show()
		{
			const Json id =
				app.call("automationLane.add", {{"track", track}, {"target", {{"type", "volume"}}}, {"read", false}})
					.at("lane")
					.at("id");
			REQUIRE(app.waitUntil([&] { return display.project().at("automation").size() == 1; }));
			REQUIRE(showVolume() == id);
			return id;
		}

		// The automation lane's box in the event display, in the window's pixels.
		Json box(const Json& id)
		{
			const Json found = elements(app, ".automation-lane[lane=\"" + id.dump() + "\"]");
			REQUIRE(found.size() == 1);
			return found[0].at("box");
		}

		// The x of a time in seconds, and the y of a display value from 0 (bottom) to 1, in the lane.
		int x(double seconds)
		{
			const Json lanes = elements(app, "event-lanes.events").at(0).at("box");
			return lanes.at("x").get<int>() + static_cast<int>(std::lround(seconds / display.view().scale));
		}

		int y(const Json& id, double value)
		{
			const Json lane = box(id);
			return lane.at("y").get<int>() +
				static_cast<int>(std::lround((1.0 - value) * lane.at("height").get<double>()));
		}

		std::vector<ui::CurvePoint> curve(const char* which, const Json& id) const
		{
			const std::string selector = ".automation-lane[lane=\"" + id.dump() + "\"] curve." + which;
			const auto* found = dynamic_cast<const ui::Curve*>(display.eventArea().QuerySelector(selector));
			REQUIRE(found != nullptr);
			return found->points();
		}

		// Waits until the display has read the lane as the engine has it, so that the next gesture edits what is there
		// (its curve shows a gesture's result at once).
		void settle(const Json& id)
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json& project = display.project();
					const Json expected = lane(id);
					return project.is_object() &&
						std::any_of(project.at("automation").begin(), project.at("automation").end(),
							[&expected](const Json& shown) { return shown == expected; });
				}));
		}

		test::UiApp app{{1000, 500}};
		ui::EventDisplay& display;
		uint64_t track = 0;
	};
}

TEST_CASE("automation tracks show and hide below their track, and the parameter selector picks their parameter", "[ui]")
{
	Automation f;
	// Show/Hide Automation opens an automation track for the first parameter (reference manual p. 880), without a
	// lane in the project until a tool draws on it, and so without an undo step (#1918).
	const Json history = f.app.call("history.get");
	const Json volume = f.showVolume();
	CHECK(f.lanes().empty());
	REQUIRE(f.display.unaddedLane(volume.get<uint64_t>()) != nullptr);
	CHECK(f.display.unaddedLane(volume.get<uint64_t>())->at("target").at("type") == "volume");
	CHECK(hasClass(elements(f.app, row(f.track) + " .automation-toggle").at(0), "checked"));
	// The track's row reaches over its automation track's lane, which is level with the automation row.
	const Json trackRow = elements(f.app, row(f.track)).at(0).at("box");
	CHECK(trackRow.at("height").get<int>() == static_cast<int>(2 * ui::g_trackHeight));
	CHECK(elements(f.app, ".automation-row").at(0).at("box").at("y") == f.box(volume).at("y"));
	// The static value line shows the channel's volume while the lane has no points.
	REQUIRE(f.app.waitUntil([&] { return f.curve("static-value", volume).size() == 1; }));
	CHECK(f.curve("static-value", volume)[0].value == Approx(60.0 / 66.0)); // 0 dB from -60 to 6

	// Append Automation Track adds the next parameter's.
	f.click(".automation-row .automation-append");
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-parameter").at(1).value("text", "") == "Pan"; }));

	// The parameter selector lists the track's parameters, which the search narrows; picking one shows it instead.
	f.app.ui().click(std::nullopt, {".automation-parameter", 1, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(hasClass(elements(f.app, "#automation-parameter-menu").at(0), "open"));
	// The fader's five and the 41 of the channel strip's sections.
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.automation-target").size() == 5 + 41);
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.checked").at(0).value("text", "") == "Pan");
	// The selector is taller than the window and opens at its top, where it stays while the search narrows it and
	// when the search text goes again (#1665): it doesn't jump under its button.
	const auto top = [&f]
	{
		f.app.shell().step(std::chrono::milliseconds(0));
		f.app.shell().step(std::chrono::milliseconds(0)); // the update in which a resized menu moves
		return elements(f.app, "#automation-parameter-menu").at(0).at("box").at("y").get<int>();
	};
	const int opened = top();
	f.click("#automation-parameter-menu .automation-search");
	f.app.ui().text(std::nullopt, "LEFT");
	CHECK(top() == opened);
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.automation-target:not(.hidden)").size() == 1);
	for (int i = 0; i < 4; ++i)
		f.app.ui().key(std::nullopt, "Backspace");
	CHECK(top() == opened);
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.automation-target:not(.hidden)").size() == 5 + 41);
	f.app.ui().text(std::nullopt, "LEFT");
	CHECK(top() == opened);
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.automation-target:not(.hidden)").size() == 1);
	// The keys skip the parameters that the search hides: Shift+Tab from the search field and Up from the first item
	// go round to the last shown one, and Tab from there round to the search field (#524).
	const std::string search = "#automation-parameter-menu .automation-search";
	const std::string first = "#automation-parameter-menu menuitem[automation-function=remove]";
	const std::string shown = "#automation-parameter-menu menuitem.automation-target:not(.hidden)";
	REQUIRE(focused(f.app, search));
	f.app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused(f.app, shown));
	f.app.ui().key(std::nullopt, "Tab");
	CHECK(focused(f.app, search));
	f.app.ui().key(std::nullopt, "Tab");
	CHECK(focused(f.app, first));
	f.app.ui().key(std::nullopt, "Up");
	CHECK(focused(f.app, shown));
	f.app.ui().key(std::nullopt, "Down");
	CHECK(focused(f.app, first));
	REQUIRE(hasClass(elements(f.app, "#automation-parameter-menu").at(0), "open"));
	f.click("#automation-parameter-menu menuitem.automation-target:not(.hidden)");
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return elements(f.app, ".automation-parameter").size() == 2 &&
				elements(f.app, ".automation-parameter").at(1).value("text", "") == "Pan left";
		}));
	CHECK(f.lanes().empty());

	// Remove Parameter closes its automation track.
	f.app.ui().click(std::nullopt, {".automation-parameter", 1, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.click("#automation-parameter-menu menuitem[automation-function=\"remove\"]");
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 1; }));

	// Hiding closes the automation tracks; Show All Used Automation opens those with points, on every track. None of
	// this changed the project.
	f.click(row(f.track) + " .automation-toggle");
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").empty(); }));
	CHECK(f.display.laneAutomation() == std::vector<uint64_t>{0});
	CHECK(f.lanes().empty());
	CHECK(f.app.call("history.get") == history);
	const Json used =
		f.app.call("automationLane.add", {{"track", f.track}, {"target", {{"type", "volume"}}}}).at("lane").at("id");
	f.app.call("automationPoint.add", {{"lane", used}, {"points", {{{"position", 0}, {"value", -6.0}}}}});
	const uint64_t other = f.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	f.app.call("automationLane.add", {{"track", other}, {"target", {{"type", "pan"}}}}); // unused
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("automation").size() == 2; }));
	f.app.keys().run("track.showUsedAutomation", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 1; }));
	CHECK(f.display.automationShown(f.track));
	CHECK(!f.display.automationShown(other));
	f.app.keys().run("track.hideAllAutomation", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").empty(); }));
	// The context menu's command shows the selected tracks' automation.
	f.app.appUi().trackList()->select({other});
	f.app.keys().run("track.toggleAutomationSelected", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(other) + " .automation-row").size() == 1; }));
	// Remove Unused Parameters removes the track's lanes without points and closes their automation tracks.
	f.app.ui().click(
		std::nullopt, {row(other) + " .automation-parameter", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.click("#automation-parameter-menu menuitem[automation-function=\"removeUnused\"]");
	REQUIRE(f.app.waitUntil([&] { return f.app.call("automationLane.list", {{"track", other}}).at("lanes").empty(); }));
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(other) + " .automation-row").empty(); }));

	// Remove Parameter on a lane in the project with points removes it, and its automation track closes and stays
	// closed after the project is read again.
	f.app.keys().run("track.showUsedAutomation", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(f.track) + " .automation-row").size() == 1; }));
	f.app.ui().click(
		std::nullopt, {row(f.track) + " .automation-parameter", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.click("#automation-parameter-menu menuitem[automation-function=\"remove\"]");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().empty(); }));
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(f.track) + " .automation-row").empty(); }));
	f.app.call("track.add", {{"type", "audio"}});
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("tracks").size() == 3; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(f.app, ".automation-row").empty());
}

TEST_CASE("showing automation records no undo step, and the first points add their lane in their undo step", "[ui]")
{
	Automation f;
	const Json history = f.app.call("history.get");
	const Json shown = f.showVolume();
	CHECK(f.app.call("history.get") == history);

	// The draw tool's first click adds the lane, which reads, with its point: one undo step (#1918).
	f.display.setTool(ui::Tool::Draw);
	f.clickAt(f.x(1.0), f.y(shown, 0.5));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json all = f.lanes();
			return all.size() == 1 && all[0].at("points").size() == 1;
		}));
	const Json volume = f.lanes()[0].at("id");
	CHECK(f.lane(volume).at("target").at("type") == "volume");
	CHECK(f.lane(volume).at("read") == true);
	const Json steps = f.app.call("history.get").at("steps");
	REQUIRE(steps.size() == history.at("steps").size() + 1);
	CHECK(steps.back().at("action") == "Draw Automation");
	REQUIRE(f.app.waitUntil(
		[&] { return f.display.laneAutomation() == std::vector<uint64_t>{0, volume.get<uint64_t>()}; }));

	// Undo takes the lane with its point and leaves the automation track shown and empty; redo brings them back.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const std::vector<uint64_t>& lanes = f.display.laneAutomation();
			return f.lanes().empty() && lanes.size() == 2 && f.display.unaddedLane(lanes[1]) != nullptr;
		}));
	CHECK(f.display.automationShown(f.track));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json parameters = elements(f.app, ".automation-parameter");
			return parameters.size() == 1 && parameters.at(0).value("text", "") == "Volume";
		}));
	f.app.call("history.redo");
	REQUIRE(f.app.waitUntil(
		[&] { return f.display.laneAutomation() == std::vector<uint64_t>{0, volume.get<uint64_t>()}; }));
	CHECK(f.lane(volume).at("points").size() == 1);
}

TEST_CASE("a volume lane's static value line follows the channel without a project read", "[ui]")
{
	// A step that changed only channels, such as each step of a fader drag, reads no project (#1881): the display
	// asks again for the lanes' targets, whose values the line shows.
	Automation f;
	const Json volume = f.show();
	REQUIRE(f.app.waitUntil([&] { return f.curve("static-value", volume).size() == 1; }));
	CHECK(f.curve("static-value", volume)[0].value == Approx(60.0 / 66.0)); // 0 dB from -60 to 6
	Json channel;
	const Json project = f.app.call("project.get");
	for (const Json& each : project.at("tracks"))
		if (each.at("id") == f.track)
			channel = each.at("channel");
	ui::EngineBinding& binding = f.app.appUi().binding();
	REQUIRE(f.app.waitUntil([&binding] { return binding.waiting("project.get") == 0; }));
	size_t reads = 0;
	const ui::Subscription counted = binding.onResult("project.get", [&reads](const engine::Result&) { ++reads; });
	f.app.call("channel.set", {{"channel", channel}, {"volume", -6.0}});
	CHECK(f.app.waitUntil(
		[&]
		{
			const auto line = f.curve("static-value", volume);
			return line.size() == 1 && line[0].value == Approx(54.0 / 66.0);
		}));
	CHECK(reads == 0);
}

TEST_CASE("Show/Hide Automation lies in its track's lower left corner, inside the row, at every track height", "[ui]")
{
	// #1773: the button ended on the row's last pixel, over the selection frame and the separator to the next row, and
	// was 24 dp wide although its rule gives it 16. It stays left of the name, clear of the row's lower edge, and above
	// the 4 dp at that edge that size the track wherever the row has room for both.
	Automation f;
	const auto box = [&f](const std::string& selector)
	{
		const Json found = elements(f.app, selector);
		REQUIRE(found.size() == 1);
		return found[0].at("box");
	};
	const auto right = [](const Json& box) { return box.at("x").get<int>() + box.at("width").get<int>(); };
	const auto bottom = [](const Json& box) { return box.at("y").get<int>() + box.at("height").get<int>(); };
	const auto meet = [&](const Json& a, const Json& b)
	{
		return a.at("x").get<int>() < right(b) && b.at("x").get<int>() < right(a) && a.at("y").get<int>() < bottom(b) &&
			b.at("y").get<int>() < bottom(a);
	};
	const std::string toggle = row(f.track) + " .automation-toggle";
	const auto place = [&](double height)
	{
		f.display.setTrackHeight(f.track, height);
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	for (const double height : {ui::g_minTrackHeight, 30.0, ui::g_trackHeight, 64.0, 120.0})
	{
		INFO("track height " << height);
		place(height);
		REQUIRE(f.app.waitUntil([&] { return box(row(f.track)).at("height").get<double>() == height; }));
		const Json own = box(row(f.track));
		const Json button = box(toggle);
		INFO("row " << own.dump() << ", button " << button.dump());
		CHECK(button.at("width").get<int>() == 16);
		CHECK(button.at("height").get<int>() == 16);
		CHECK(button.at("x").get<int>() >= own.at("x").get<int>() + 4); // the row's padding
		CHECK(button.at("y").get<int>() >= own.at("y").get<int>() + 2);
		CHECK(bottom(button) <= bottom(own) - 2); // the frame and the separator stay whole
		if (height >= 22.0)                       // room for the button above the 4 dp that size the track
			CHECK(bottom(button) <= bottom(own) - 4);
		CHECK(!meet(button, box(row(f.track) + " .track-name")));
		CHECK(!meet(button, box(row(f.track) + " .track-controls")));
	}
	// With the automation tracks shown, it stays in the track's own part, above the first automation row.
	f.show();
	for (const double height : {ui::g_minTrackHeight, ui::g_trackHeight, 64.0})
	{
		INFO("track height " << height);
		place(height);
		REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(f.track) + " .automation-row").size() == 1; }));
		REQUIRE(f.app.waitUntil(
			[&]
			{
				return box(row(f.track) + " .automation-row").at("y").get<int>() ==
					box(row(f.track)).at("y").get<int>() + static_cast<int>(height);
			}));
		CHECK(bottom(box(toggle)) <= box(row(f.track) + " .automation-row").at("y").get<int>() - 2);
	}
}

TEST_CASE("the draw, line and erase tools draw automation, and the static value line shows without it", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	const int top = f.y(volume, 0.9);
	const int bottom = f.y(volume, 0.1);

	// The draw tool: a click adds a point there and turns reading on (reference manual pp. 872-873).
	f.display.setTool(ui::Tool::Draw);
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() == 1; }));
	CHECK(f.lane(volume).at("read") == true);
	const Json first = f.lane(volume).at("points")[0];
	CHECK(first.at("position").get<double>() == Approx(48000.0).margin(480.0));
	CHECK(first.at("value").get<double>() == Approx(-27.0).margin(1.0)); // halfway from -60 to 6 dB
	REQUIRE(f.app.waitUntil([&] { return f.curve("static-value", volume).empty(); }));
	f.settle(volume);

	// A drag draws a curve; a straight one keeps only its ends.
	f.drag(f.x(2.0), top, f.x(3.0), bottom);
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() == 3; }));
	f.settle(volume);
	Json points = f.lane(volume).at("points");
	CHECK(points[1].at("value").get<double>() > points[2].at("value").get<double>());
	CHECK(points[2].at("position").get<double>() == Approx(3.0 * 48000.0).margin(960.0));

	// The line tool in line mode: two points from the press to the release, which replace those between.
	f.display.setTool(ui::Tool::Line);
	f.display.setLineMode(ui::LineMode::Line);
	f.drag(f.x(1.5), bottom, f.x(3.5), top);
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() == 3; }));
	f.settle(volume);
	points = f.lane(volume).at("points");
	CHECK(points[0].at("position").get<double>() == Approx(48000.0).margin(480.0)); // outside it, kept
	CHECK(points[1].at("position").get<double>() == Approx(1.5 * 48000.0).margin(960.0));
	CHECK(points[2].at("value").get<double>() > points[1].at("value").get<double>());

	// Square mode: steps between the press's value and the release's, a grid period apart.
	f.display.setLineMode(ui::LineMode::Square);
	f.drag(f.x(4.0), bottom, f.x(6.0), top);
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() > 6; }));
	f.settle(volume);
	points = f.lane(volume).at("points");
	CHECK(points[3].at("shape") == "step");
	CHECK(points[3].at("value").get<double>() < points[4].at("value").get<double>());
	CHECK(points[5].at("value").get<double>() == Approx(points[3].at("value").get<double>()));

	// Sine and triangle draw their waves; parabola one bent segment.
	for (const ui::LineMode mode : {ui::LineMode::Sine, ui::LineMode::Triangle, ui::LineMode::Parabola})
	{
		const size_t before = f.lane(volume).at("points").size();
		f.display.setLineMode(mode);
		f.drag(f.x(6.2), bottom, f.x(6.9), top);
		REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() != before; }));
		f.settle(volume);
	}
	points = f.lane(volume).at("points");
	const auto bent =
		std::find_if(points.begin(), points.end(), [](const Json& p) { return p.at("shape") == "curve"; });
	CHECK(bent != points.end());

	// The erase tool removes the point clicked.
	const size_t count = f.lane(volume).at("points").size();
	f.display.setTool(ui::Tool::Erase);
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() == count - 1; }));

	// Reading off grays the curve out and shows the static value line again (reference manual p. 870).
	f.click(".automation-row .automation-read");
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("read") == false; }));
	REQUIRE(f.app.waitUntil([&] { return hasClass(elements(f.app, ".automation-lane").at(0), "off"); }));
	CHECK(f.curve("static-value", volume).size() == 1);

	// The line tool does nothing on a step curve, such as mute's (reference manual p. 874).
	const Json mute =
		f.app.call("automationLane.add", {{"track", f.track}, {"target", {{"type", "mute"}}}}).at("lane").at("id");
	f.settle(mute);
	f.display.setAutomationTarget(volume.get<uint64_t>(), {{"type", "mute"}});
	REQUIRE(
		f.app.waitUntil([&] { return elements(f.app, ".automation-lane[lane=\"" + mute.dump() + "\"]").size() == 1; }));
	f.display.setTool(ui::Tool::Line);
	f.drag(f.x(1.0), bottom, f.x(2.0), top);
	f.app.shell().step(std::chrono::milliseconds(50));
	CHECK(f.lane(mute).at("points").empty());
}

TEST_CASE("automation points are selected, moved, scaled and deleted with the object selection tool", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add",
		{{"lane", volume},
			{"points", {{{"position", 48000}, {"value", -27.0}}, {{"position", 96000}, {"value", -10.5}}}}});
	f.settle(volume);
	const Json ids = {f.lane(volume).at("points")[0].at("id"), f.lane(volume).at("points")[1].at("id")};
	const auto selection = [&] { return f.display.selectedPoints(); };

	// A click selects a point, Ctrl-click adds one (reference manual p. 875).
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(selection().size() == 1);
	CHECK(selection().at(volume.get<uint64_t>()) == std::set<uint64_t>{ids[0].get<uint64_t>()});
	CHECK(elements(f.app, ".event-display").at(0).at("attributes").value("automation-selection", "") ==
		volume.dump() + ":" + ids[0].dump());
	f.clickAt(f.x(2.0), f.y(volume, 0.75), {"ctrl"});
	CHECK(selection().at(volume.get<uint64_t>()).size() == 2);

	// Two selected points of a ramp curve show the automation event editor; dragging the middle of its top scales
	// them vertically from its bottom (reference manual p. 876).
	REQUIRE(f.app.waitUntil([&] { return hasClass(elements(f.app, ".automation-editor").at(0), "shown"); }));
	const Json editor = elements(f.app, ".automation-editor").at(0).at("box");
	const int middle = editor.at("x").get<int>() + (editor.at("width").get<int>() / 2);
	const int editorTop = editor.at("y").get<int>();
	f.drag(middle, editorTop, middle, editorTop - 5);
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points")[1].at("value").get<double>() > -10.0; }));
	CHECK(f.lane(volume).at("points")[0].at("value").get<double>() == Approx(-27.0).margin(0.01));
	f.settle(volume);

	// Its top border elsewhere moves them up or down.
	const double low = f.lane(volume).at("points")[0].at("value").get<double>();
	const Json moved = elements(f.app, ".automation-editor").at(0).at("box");
	const int quarter = moved.at("x").get<int>() + (moved.at("width").get<int>() / 3);
	f.drag(quarter, moved.at("y").get<int>(), quarter, moved.at("y").get<int>() + 4);
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points")[0].at("value").get<double>() < low - 1.0; }));
	f.settle(volume);

	// Dragging a selected point moves the selection in time and value.
	const Json before = f.lane(volume).at("points");
	const double v0 = (before[0].at("value").get<double>() + 60.0) / 66.0;
	f.drag(f.x(1.0), f.y(volume, v0), f.x(1.5), f.y(volume, v0));
	REQUIRE(
		f.app.waitUntil([&] { return f.lane(volume).at("points")[0].at("position").get<double>() > 1.4 * 48000.0; }));
	CHECK(f.lane(volume).at("points")[1].at("position").get<double>() == Approx(2.5 * 48000.0).margin(960.0));
	CHECK(f.lane(volume).at("points")[0].at("value").get<double>() ==
		Approx(before[0].at("value").get<double>()).margin(0.5));
	f.settle(volume);

	// A rectangle from an empty spot selects the points in it.
	f.drag(f.x(1.2), f.y(volume, 0.98), f.x(1.8), f.y(volume, 0.02));
	REQUIRE(selection().size() == 1);
	CHECK(selection().at(volume.get<uint64_t>()) == std::set<uint64_t>{ids[0].get<uint64_t>()});

	// Delete removes the selected points; selecting an event deselects them.
	f.app.keys().run("edit.delete", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.lane(volume).at("points").size() == 1; }));
	CHECK(selection().empty());
	f.settle(volume);
	f.clickAt(f.x(2.5), f.y(volume, (f.lane(volume).at("points")[0].at("value").get<double>() + 60.0) / 66.0));
	CHECK(selection().size() == 1);
	f.clickAt(f.x(0.5), elements(f.app, "event-lanes.events").at(0).at("box").at("y").get<int>() + 10);
	CHECK(selection().empty());
}

TEST_CASE(
	"with the selection tools combined, an automation track's pointer is the tool chosen, as a press uses it", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	f.display.setCombineSelectionTools(true);
	const auto pointer = [&f] { return elements(f.app, ".event-area").at(0).at("attributes").value("pointer", ""); };
	// In the upper half, where an event lane's pointer is the range selection tool's.
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{f.x(1.0), f.y(volume, 0.9)}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.app.waitUntil([&] { return pointer() == "objectSelection"; }));
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{f.x(1.0) + 1, f.y(volume, 0.9)}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(pointer() == "objectSelection");
}

TEST_CASE("an automation track's curve leaves gaps after terminators and in virgin territory", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add",
		{{"lane", volume},
			{"points",
				{{{"position", 48000}, {"value", -6.0}, {"terminator", true}}, {{"position", 96000}, {"value", 0.0}},
					{{"position", 144000}, {"value", -12.0}}}}});
	f.settle(volume);
	const auto terminators = [&f, &volume]()
	{
		std::vector<bool> result;
		for (const ui::CurvePoint& point : f.curve("automation-curve", volume))
			result.push_back(point.terminator);
		return result;
	};
	CHECK(terminators() == std::vector<bool>{true, false, false});
	const auto virgin = [&f, &volume]()
	{
		const std::string selector = ".automation-lane[lane=\"" + volume.dump() + "\"] curve.automation-curve";
		const auto* curve = dynamic_cast<const ui::Curve*>(f.display.eventArea().QuerySelector(selector));
		return curve != nullptr && curve->virginTerritory();
	};
	CHECK(!virgin());

	// Virgin territory is no undo step, but the curve follows it.
	f.app.call("automation.set", {{"virginTerritory", true}});
	REQUIRE(f.app.waitUntil(virgin));
	f.app.call("automation.set", {{"virginTerritory", false}});
	REQUIRE(f.app.waitUntil([&] { return !virgin(); }));
	CHECK(terminators() == std::vector<bool>{true, false, false});
}

TEST_CASE("an automation move preview keeps a terminator before a point at the same position", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	Json points = Json::array();
	points.push_back({{"position", 48000}, {"value", -6.0}});
	for (int i = 2; i < 25; ++i)
		points.push_back({{"position", i * 48000}, {"value", -6.0}});
	points.push_back({{"position", 25 * 48000}, {"value", -12.0}, {"terminator", true}});
	points.push_back({{"position", 25 * 48000}, {"value", 0.0}});
	for (int i = 26; i < 50; ++i)
		points.push_back({{"position", i * 48000}, {"value", -6.0}});
	f.app.call("automationPoint.add", {{"lane", volume}, {"points", points}});
	f.settle(volume);
	const ui::Target from{"", 0, ui::Point{f.x(1.0), f.y(volume, 54.0 / 66.0)}};
	const ui::Target to{"", 0, ui::Point{f.x(1.25), f.y(volume, 54.0 / 66.0)}};
	f.app.ui().press(std::nullopt, from, ui::MouseButton::Left);
	f.app.ui().move(std::nullopt, to);
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto shown = f.curve("automation-curve", volume);
	REQUIRE(shown.size() == points.size());
	const auto gap = std::find_if(
		shown.begin(), shown.end(), [](const ui::CurvePoint& point) { return std::abs(point.position - 25.0) < 1e-6; });
	REQUIRE(gap != shown.end());
	REQUIRE(std::next(gap) != shown.end());
	CHECK(gap->terminator);
	CHECK_FALSE(std::next(gap)->terminator);
	f.app.ui().release(std::nullopt, to, ui::MouseButton::Left);
}

TEST_CASE("a Ctrl-drag moves the selected automation points in one direction, and the editor tilts and stretches them",
	"[ui]")
{
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add",
		{{"lane", volume},
			{"points", {{{"position", 48000}, {"value", -27.0}}, {{"position", 96000}, {"value", -10.5}}}}});
	f.settle(volume);
	const Json ids = {f.lane(volume).at("points")[0].at("id"), f.lane(volume).at("points")[1].at("id")};
	const std::set<uint64_t> both = {ids[0].get<uint64_t>(), ids[1].get<uint64_t>()};
	const auto selected = [&]
	{
		const auto selection = f.display.selectedPoints();
		const auto found = selection.find(volume.get<uint64_t>());
		return found == selection.end() ? std::set<uint64_t>() : found->second;
	};
	const auto value = [&](size_t i) { return f.lane(volume).at("points")[i].at("value").get<double>(); };
	const auto position = [&](size_t i) { return f.lane(volume).at("points")[i].at("position").get<double>(); };
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	f.clickAt(f.x(2.0), f.y(volume, 0.75), {"ctrl"});
	REQUIRE(selected() == both);

	// Ctrl-dragging one of the selected points moves both, across only (reference manual p. 877), and keeps them
	// selected.
	f.drag(f.x(2.0), f.y(volume, 0.75), f.x(2.5), f.y(volume, 0.75) + 4, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return position(1) > 2.4 * 48000.0; }));
	CHECK(position(0) == Approx(1.5 * 48000.0).margin(960.0));
	CHECK(value(0) == Approx(-27.0).margin(0.01));
	CHECK(value(1) == Approx(-10.5).margin(0.01));
	CHECK(selected() == both);
	f.settle(volume);

	// A Ctrl-click on a selected point without moving takes it out of the selection.
	f.clickAt(f.x(2.5), f.y(volume, 0.75), {"ctrl"});
	CHECK(selected() == std::set<uint64_t>{ids[0].get<uint64_t>()});
	f.clickAt(f.x(2.5), f.y(volume, 0.75), {"ctrl"});
	REQUIRE(selected() == both);

	// The automation event editor's top left corner tilts the curve there and leaves the right point (reference manual
	// p. 876).
	const auto editor = [&]
	{
		REQUIRE(f.app.waitUntil([&] { return hasClass(elements(f.app, ".automation-editor").at(0), "shown"); }));
		return elements(f.app, ".automation-editor").at(0).at("box");
	};
	Json box = editor();
	double left = value(0);
	double right = value(1);
	f.drag(
		box.at("x").get<int>() + 2, box.at("y").get<int>() + 1, box.at("x").get<int>() + 2, box.at("y").get<int>() + 5);
	REQUIRE(f.app.waitUntil([&] { return value(0) < left - 1.0; }));
	CHECK(value(1) == Approx(right).margin(0.01));
	f.settle(volume);

	// Its top right corner tilts the right side and leaves the left point.
	box = editor();
	left = value(0);
	right = value(1);
	const int boxRight = box.at("x").get<int>() + box.at("width").get<int>() - 2;
	f.drag(boxRight, box.at("y").get<int>() + 1, boxRight, box.at("y").get<int>() + 5);
	REQUIRE(f.app.waitUntil([&] { return value(1) < right - 1.0; }));
	CHECK(value(0) == Approx(left).margin(0.01));
	f.settle(volume);

	// Its bottom border stretches the points' times from the box's left.
	box = editor();
	const int middle = box.at("x").get<int>() + (box.at("width").get<int>() / 2);
	const int bottom = box.at("y").get<int>() + box.at("height").get<int>() - 1;
	f.drag(middle, bottom, middle + (f.x(2.0) - f.x(1.5)), bottom);
	REQUIRE(f.app.waitUntil([&] { return position(1) > 2.9 * 48000.0; }));
	CHECK(position(0) == Approx(1.5 * 48000.0).margin(960.0));
	CHECK(position(1) == Approx(3.0 * 48000.0).margin(960.0));
	CHECK(selected() == both);
}

TEST_CASE("a move of automation points snaps, and Ctrl with another modifier turns snapping off", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add", {{"lane", volume}, {"points", {{{"position", 96000}, {"value", -6.0}}}}});
	f.settle(volume);
	f.app.call("snap.set", {{"on", true}, {"type", "grid"}, {"grid", "bar"}});
	REQUIRE(f.app.waitUntil([&f] { return f.display.snap().on; }));
	const auto position = [&] { return f.lane(volume).at("points")[0].at("position").get<double>(); };
	const int y = f.y(volume, 54.0 / 66.0);

	// The pressed point lands on a bar, 2 s at 120 BPM (reference manual p. 877).
	f.drag(f.x(2.0), y, f.x(3.3), y);
	REQUIRE(f.app.waitUntil([&] { return position() > 96000.0; }));
	CHECK(position() == 192000.0);
	f.settle(volume);

	// Ctrl and Shift, or Ctrl and Alt, move it where the mouse goes.
	f.drag(f.x(4.0), y, f.x(4.7), y, {"ctrl", "shift"});
	REQUIRE(f.app.waitUntil([&] { return position() > 192000.0; }));
	CHECK(position() == Approx(4.7 * 48000.0).margin(960.0));
	f.settle(volume);
	f.drag(f.x(4.7), y, f.x(5.2), y, {"ctrl", "alt"});
	REQUIRE(f.app.waitUntil([&] { return position() > 4.8 * 48000.0; }));
	CHECK(position() == Approx(5.2 * 48000.0).margin(960.0));
}

TEST_CASE("moved automation points replace those where they land, and clicks select as they do with events", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	Json points = Json::array();
	for (int i = 1; i <= 4; ++i)
		points.push_back({{"position", i * 48000}, {"value", -6.0}});
	f.app.call("automationPoint.add", {{"lane", volume}, {"points", points}});
	f.settle(volume);
	const auto positions = [&]
	{
		std::vector<double> found;
		const Json lane = f.lane(volume);
		for (const Json& point : lane.at("points"))
			found.push_back(point.at("position").get<double>() / 48000.0);
		return found;
	};
	const auto selected = [&]
	{
		const auto selection = f.display.selectedPoints();
		const auto found = selection.find(volume.get<uint64_t>());
		return found == selection.end() ? size_t{0} : found->second.size();
	};
	const int y = f.y(volume, 54.0 / 66.0);
	f.clickAt(f.x(1.0), y);
	f.clickAt(f.x(2.0), y, {"ctrl"});
	REQUIRE(selected() == 2);

	// A click on one point of the selection selects only it (reference manual p. 875).
	f.clickAt(f.x(2.0), y);
	CHECK(f.display.selectedPoints().at(volume.get<uint64_t>()) ==
		std::set<uint64_t>{f.lane(volume).at("points")[1].at("id").get<uint64_t>()});
	f.clickAt(f.x(1.0), y, {"ctrl"});
	REQUIRE(selected() == 2);

	// The selection moved over the third point replaces it, in the preview and in one undo step (pp. 877-878).
	const ui::Target from{"", 0, ui::Point{f.x(2.0), y}};
	const ui::Target to{"", 0, ui::Point{f.x(3.5), y}};
	f.app.ui().press(std::nullopt, from, ui::MouseButton::Left);
	f.app.ui().move(std::nullopt, to);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.curve("automation-curve", volume).size() == 3);
	f.app.ui().release(std::nullopt, to, ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return positions().size() == 3; }));
	CHECK(positions()[0] == Approx(2.5).margin(0.02));
	CHECK(positions()[1] == Approx(3.5).margin(0.02));
	CHECK(positions()[2] == 4.0);
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return positions() == std::vector<double>{1.0, 2.0, 3.0, 4.0}; }));
	f.settle(volume);

	// A click on an empty spot adds a point and ends the selection.
	REQUIRE(selected() == 2);
	f.clickAt(f.x(5.0), f.y(volume, 0.25));
	REQUIRE(f.app.waitUntil([&] { return positions().size() == 5; }));
	CHECK(selected() == 0);
	f.settle(volume);

	// A move restricted with Ctrl at the press stays so when Ctrl goes up before the button.
	f.clickAt(f.x(1.0), y);
	const ui::Target down{"", 0, ui::Point{f.x(1.5), y + 10}};
	f.app.ui().press(std::nullopt, {"", 0, ui::Point{f.x(1.0), y}}, ui::MouseButton::Left, {"ctrl"});
	f.app.ui().move(std::nullopt, down, {"ctrl"});
	f.app.ui().release(std::nullopt, down, ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return positions()[0] > 1.4; }));
	CHECK(f.lane(volume).at("points")[0].at("value").get<double>() == Approx(-6.0).margin(0.01));
	f.settle(volume);

	// Selecting a range deselects the points, so that Delete deletes the range.
	f.clickAt(f.x(2.0), y);
	REQUIRE(selected() == 1);
	f.display.setRange(ui::RangeSelection{0, 48000, {f.track}});
	CHECK(selected() == 0);
}

TEST_CASE("a move of one of two automation points at one position that keeps the times keeps the other", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	// A jump at 2 s, from -20 dB to 0 dB, as pastes leave at their edges.
	f.app.call("automationPoint.add",
		{{"lane", volume},
			{"points",
				{{{"position", 48000}, {"value", -20.0}}, {{"position", 96000}, {"value", -20.0}},
					{{"position", 96000}, {"value", 0.0}}}}});
	f.settle(volume);
	const auto values = [&]
	{
		std::vector<double> found;
		const Json lane = f.lane(volume);
		for (const Json& point : lane.at("points"))
			found.push_back(point.at("value").get<double>());
		return found;
	};
	REQUIRE(values().size() == 3);

	// Up and down only with Ctrl: the point at -20 dB goes up and the jump's other point stays, in the preview too.
	const int y = f.y(volume, 40.0 / 66.0);
	const ui::Target down{"", 0, ui::Point{f.x(2.0), y}};
	const ui::Target up{"", 0, ui::Point{f.x(2.0) + 1, y - 8}};
	f.app.ui().press(std::nullopt, down, ui::MouseButton::Left, {"ctrl"});
	f.app.ui().move(std::nullopt, up, {"ctrl"});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.curve("automation-curve", volume).size() == 3);
	f.app.ui().release(std::nullopt, up, ui::MouseButton::Left, {"ctrl"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return values().size() == 3 && values()[1] > -19.0; }));
	CHECK(values()[0] == Approx(-20.0).margin(0.01));
	CHECK(values()[2] == Approx(0.0).margin(0.01));
}

TEST_CASE("the zoom and play tools work on automation tracks, Show All Used Automation keeps those that show, and "
		  "selecting a track reveals its automation tracks",
	"[ui]")
{
	Automation f;
	const Json volume = f.show(); // no points
	f.display.setTool(ui::Tool::Zoom);
	const double scale = f.display.view().scale;
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.display.view().scale < scale; }));

	// The play tool auditions the automation track's audio track, and scrubs it (reference manual pp. 252-253).
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	std::vector<uint64_t> auditioned;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&](const engine::Event& event)
		{
			modes.push_back(event.data.at("mode").get<std::string>());
			auditioned.push_back(event.data.at("track").get<uint64_t>());
		});
	const auto audition = [&](const std::vector<std::string>& expected)
	{
		return f.app.waitUntil(
			[&]
			{
				binding.update();
				return modes == expected;
			});
	};
	f.display.setTool(ui::Tool::Play);
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(audition({"play", "off"}));
	CHECK(auditioned[0] == f.track);
	f.display.setPlayMode(ui::PlayMode::Scrub);
	f.drag(f.x(1.0), f.y(volume, 0.5), f.x(1.5), f.y(volume, 0.5));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes.size() >= 4 && modes.back() == "off";
		}));
	CHECK(modes[2] == "scrub");
	CHECK(auditioned[2] == f.track);
	f.display.setTool(ui::Tool::ObjectSelection);

	// Show All Used Automation leaves the unused automation track open.
	std::vector<uint64_t> tracks(10);
	for (uint64_t& track : tracks)
		track = f.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const Json used = f.app.call("automationLane.add", {{"track", tracks.back()}, {"target", {{"type", "volume"}}}})
						  .at("lane")
						  .at("id");
	f.app.call("automationPoint.add", {{"lane", used}, {"points", {{{"position", 0}, {"value", -6.0}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("automation").size() == 2; }));
	f.app.keys().run("track.showUsedAutomation", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 2; }));
	CHECK(f.display.automationShown(f.track));

	// Selecting the last track with the keys scrolls its automation track into view too.
	f.app.appUi().trackList()->select({f.track});
	for (size_t i = 0; i < tracks.size(); ++i)
		f.app.keys().run("track.selectNext", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.appUi().trackList()->selection() == std::vector<uint64_t>{tracks.back()});
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json lanes = elements(f.app, "event-lanes.events").at(0).at("box");
	const Json shown = f.box(used);
	CHECK(shown.at("y").get<int>() + shown.at("height").get<int>() <=
		lanes.at("y").get<int>() + lanes.at("height").get<int>());
}

TEST_CASE("Show Only Automation Tracks of a plugin shows its lanes and hides the track's others", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	const Json project = f.app.call("project.get");
	uint64_t channel = 0;
	for (const Json& track : project.at("tracks"))
		if (track.at("id").get<uint64_t>() == f.track)
			channel = track.at("channel").get<uint64_t>();
	REQUIRE(channel != 0);
	const Json slot =
		f.app.call("plugin.insert", {{"channel", channel}, {"plugin", "builtin:gainPan"}}).at("slot").at("id");
	const auto addPluginLane = [&](uint32_t parameter)
	{
		return f.app
			.call("automationLane.add",
				{{"track", f.track}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", parameter}}}})
			.at("lane")
			.at("id");
	};
	const Json gain = addPluginLane(0);
	const Json pan = addPluginLane(1);
	for (const Json& lane : {volume, gain, pan})
		f.app.call("automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 0.0}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("automation").size() == 3; }));
	f.app.keys().run("track.showUsedAutomation", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 3; }));
	// The selector names the plugin once the track's targets include its parameters.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json* targets = f.display.automationTargets(f.track);
			return targets != nullptr &&
				std::any_of(targets->begin(), targets->end(),
					[](const Json& target) { return target.at("target").at("type") == "plugin"; });
		}));

	// The parameter selector of a plugin's automation track offers Show Only Automation Tracks of the plugin (reference
	// manual p. 880).
	const std::vector<uint64_t> shown = f.display.laneAutomation();
	const auto at = std::find(shown.begin(), shown.end(), gain.get<uint64_t>());
	REQUIRE(at != shown.end());
	const auto index = static_cast<int>(at - shown.begin()) - 1; // lane 0 is the track's own
	f.app.ui().click(std::nullopt, {".automation-parameter", index, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click("#automation-parameter-menu menuitem[automation-function=\"onlyPlugin\"]");
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 2; }));
	std::vector<uint64_t> only = f.display.laneAutomation();
	std::sort(only.begin(), only.end());
	std::vector<uint64_t> expected = {0, gain.get<uint64_t>(), pan.get<uint64_t>()};
	std::sort(expected.begin(), expected.end());
	CHECK(only == expected);
}

TEST_CASE("automation tracks are selected, and the track list copies their automation to other tracks", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add",
		{{"lane", volume}, {"points", {{{"position", 0}, {"value", -6.0}}, {{"position", 48000}, {"value", 0.0}}}}});
	const uint64_t other = f.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	f.app.call("track.set", {{"track", other}, {"timeBase", "linear"}});
	const Json otherVolume =
		f.app.call("automationLane.add", {{"track", other}, {"target", {{"type", "volume"}}}}).at("lane").at("id");
	f.app.call("automationPoint.add",
		{{"lane", otherVolume}, {"points", {{{"position", 0}, {"value", -20.0}, {"shape", "step"}}}}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json& lanes = f.display.project().at("automation");
			return lanes.size() == 2 && !lanes[1].at("points").empty();
		}));
	f.app.keys().run("track.showUsedAutomation", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 2; }));
	f.app.call("transport.set", {{"left", 0}, {"right", 48000}});

	// A click beside an automation track's buttons selects it, Ctrl adds another, a click on a track deselects them.
	const auto clickRow = [&](const std::string& selector, const std::vector<std::string>& modifiers = {})
	{
		const Json box = elements(f.app, selector).at(0).at("box");
		f.clickAt(box.at("x").get<int>() + 10, box.at("y").get<int>() + (box.at("height").get<int>() / 2), modifiers);
	};
	const std::string first = row(f.track) + " .automation-row";
	const std::string second = row(other) + " .automation-row";
	// The commands for selected parameters wait for selected automation tracks, the paste commands for copied
	// automation.
	const auto canRun = [&f](const char* command) { return f.app.keys().canRun(command, f.app.main(), nullptr); };
	f.app.appUi().trackList()->select({f.track});
	CHECK(!canRun("track.copyAutomationSelectedParameters"));
	CHECK(canRun("track.copyAutomationSelected"));
	CHECK(!canRun("track.pasteAutomationAll"));
	CHECK(!canRun("track.pasteAutomationSelected"));
	clickRow(first);
	CHECK(hasClass(elements(f.app, first).at(0), "selected"));
	CHECK(canRun("track.copyAutomationSelectedParameters"));
	CHECK(!canRun("track.pasteAutomationSelectedParameters"));
	CHECK(f.app.appUi().trackList()->selectedLanes() == std::vector<uint64_t>{volume.get<uint64_t>()});
	clickRow(second, {"ctrl"});
	CHECK(f.app.appUi().trackList()->selectedLanes().size() == 2);
	clickRow(row(f.track) + " .track-name");
	CHECK(f.app.appUi().trackList()->selectedLanes().empty());
	CHECK(!hasClass(elements(f.app, first).at(0), "selected"));

	// Copy Automation of Selected Parameters, then Paste onto the other track's selected automation track, between
	// the locators (reference manual pp. 878-879).
	clickRow(first);
	f.app.keys().run("track.copyAutomationSelectedParameters", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return canRun("track.pasteAutomationSelectedParameters"); }));
	CHECK(canRun("track.pasteAutomationAll"));
	clickRow(second);
	f.app.call("transport.set", {{"left", 96000}, {"right", 144000}});
	f.app.keys().run("track.pasteAutomationSelectedParameters", Json::object(), f.app.main(), nullptr);
	const auto positions = [&]()
	{
		std::vector<int64_t> found;
		const Json lanes = f.app.call("automationLane.list", {{"track", other}}).at("lanes");
		for (const Json& point : lanes.at(0).at("points"))
			found.push_back(point.at("position").get<int64_t>());
		return found;
	};
	REQUIRE(f.app.waitUntil([&] { return positions().size() == 5; }));
	CHECK(positions() == std::vector<int64_t>{0, 96000, 96000, 144000, 144000});

	// Copy and Paste Automation of Selected Tracks.
	f.app.appUi().trackList()->select({f.track});
	f.app.call("transport.set", {{"left", 0}, {"right", 48000}});
	f.app.keys().run("track.copyAutomationSelected", Json::object(), f.app.main(), nullptr);
	f.app.appUi().trackList()->select({other});
	f.app.call("transport.set", {{"left", 192000}, {"right", 240000}});
	f.app.keys().run("track.pasteAutomationSelected", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return positions().size() == 9; }));
	CHECK(positions().at(5) == 192000);

	// A new project has nothing copied, no selected automation tracks or points and no open automation tracks, whose
	// ids its tracks, lanes and points get again.
	clickRow(second);
	const Json point = f.lane(volume).at("points").at(0).at("id");
	f.display.selectPoints({{volume.get<uint64_t>(), {point.get<uint64_t>()}}});
	REQUIRE(!f.display.selectedPoints().empty());
	REQUIRE(f.display.automationShown(f.track));
	f.app.call("project.new");
	REQUIRE(f.app.waitUntil([&] { return !canRun("track.pasteAutomationAll"); }));
	CHECK(f.app.appUi().trackList()->selectedLanes().empty());
	CHECK(f.display.selectedPoints().empty());
	CHECK(!f.display.automationShown(f.track));
	CHECK(!f.display.automationShown(other));
	CHECK(f.display.automationTargets(f.track) == nullptr);
}

TEST_CASE("pasting onto an unadded automation track adds its lane in the paste's undo step", "[ui]")
{
	// The workflow of the reference manual, pp. 878-879: open the target track's automation, select it, paste (#1918).
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add",
		{{"lane", volume}, {"points", {{{"position", 0}, {"value", -6.0}}, {{"position", 48000}, {"value", 0.0}}}}});
	f.app.call("transport.set", {{"left", 0}, {"right", 48000}});
	REQUIRE(f.app.call("automation.copy", {{"lanes", {volume}}}).at("lanes") == 1);
	const uint64_t other = f.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	f.app.call("track.set", {{"track", other}, {"timeBase", "linear"}});
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(other) + " .automation-toggle").size() == 1; }));
	f.click(row(other) + " .automation-toggle");
	const std::string automationRow = row(other) + " .automation-row";
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, automationRow).size() == 1; }));
	const auto unadded = [&]
	{
		const std::vector<uint64_t>& lanes = f.display.laneAutomation();
		return lanes.size() == 4 && f.display.unaddedLane(lanes[3]) != nullptr &&
			f.app.call("automationLane.list", {{"track", other}}).at("lanes").empty();
	};
	REQUIRE(f.app.waitUntil(unadded));

	// Selecting the unadded automation track enables the paste, not the copy: it has no automation.
	const Json box = elements(f.app, automationRow).at(0).at("box");
	f.clickAt(box.at("x").get<int>() + 10, box.at("y").get<int>() + (box.at("height").get<int>() / 2));
	const auto canRun = [&f](const char* command) { return f.app.keys().canRun(command, f.app.main(), nullptr); };
	REQUIRE(f.app.waitUntil([&] { return canRun("track.pasteAutomationSelectedParameters"); }));
	CHECK(!canRun("track.copyAutomationSelectedParameters"));

	const Json history = f.app.call("history.get");
	f.app.call("transport.set", {{"left", 96000}, {"right", 144000}});
	f.app.keys().run("track.pasteAutomationSelectedParameters", Json::object(), f.app.main(), nullptr);
	const auto pasted = [&]
	{
		const Json lanes = f.app.call("automationLane.list", {{"track", other}}).at("lanes");
		return lanes.size() == 1 && lanes[0].at("target").at("type") == "volume" && !lanes[0].at("points").empty();
	};
	REQUIRE(f.app.waitUntil(pasted));
	// The copy's points land between the locators, as onto a lane that was there (the curve before keeps its value).
	std::set<int64_t> positions;
	const Json added = f.app.call("automationLane.list", {{"track", other}}).at("lanes").at(0);
	for (const Json& point : added.at("points"))
		positions.insert(point.at("position").get<int64_t>());
	CHECK(positions.count(96000) == 1);
	CHECK(positions.count(144000) == 1);
	CHECK(*positions.rbegin() == 144000);
	// One undo step, after which the lane shows in the automation track.
	const Json steps = f.app.call("history.get").at("steps");
	REQUIRE(steps.size() == history.at("steps").size() + 1);
	CHECK(steps.back().at("action") == "Paste Automation");
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const std::vector<uint64_t>& lanes = f.display.laneAutomation();
			return lanes.size() == 4 && f.display.unaddedLane(lanes[3]) == nullptr;
		}));

	// Undo leaves the automation track shown and unadded.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil(unadded));
	CHECK(f.display.automationShown(other));
	CHECK(f.app.call("history.get").at("position") == history.at("position"));
}

TEST_CASE(
	"a paste that pastes nothing onto an unadded automation track leaves it unadded, without an undo step", "[ui]")
{
	// automation.paste adds the lane of an unadded automation track only where it pastes (#2018).
	Automation f;
	const Json volume = f.show();
	const Json pan =
		f.app.call("automationLane.add", {{"track", f.track}, {"target", {{"type", "pan"}}}}).at("lane").at("id");
	for (const Json& lane : {volume, pan})
		f.app.call("automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 0.0}}}}});
	f.app.call("transport.set", {{"left", 0}, {"right", 48000}});
	const uint64_t other = f.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	f.app.call("track.set", {{"track", other}, {"timeBase", "linear"}});
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(other) + " .automation-toggle").size() == 1; }));
	f.click(row(other) + " .automation-toggle"); // its volume, unadded
	const std::string automationRow = row(other) + " .automation-row";
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, automationRow).size() == 1; }));
	const auto unadded = [&]
	{
		const std::vector<uint64_t>& lanes = f.display.laneAutomation();
		return !lanes.empty() && f.display.unaddedLane(lanes.back()) != nullptr &&
			f.app.call("automationLane.list", {{"track", other}}).at("lanes").empty();
	};
	REQUIRE(f.app.waitUntil(unadded));
	const Json box = elements(f.app, automationRow).at(0).at("box");
	f.clickAt(box.at("x").get<int>() + 10, box.at("y").get<int>() + (box.at("height").get<int>() / 2));
	const auto canRun = [&f](const char* command) { return f.app.keys().canRun(command, f.app.main(), nullptr); };
	const ui::EngineBinding& binding = f.app.appUi().binding();
	const auto paste = [&]
	{
		REQUIRE(f.app.waitUntil([&] { return canRun("track.pasteAutomationSelectedParameters"); }));
		const Json history = f.app.call("history.get");
		const Json project = f.app.call("project.get");
		f.app.keys().run("track.pasteAutomationSelectedParameters", Json::object(), f.app.main(), nullptr);
		REQUIRE(f.app.waitUntil([&] { return binding.waiting("automation.paste") == 0; }));
		f.app.shell().step(std::chrono::milliseconds(0));
		CHECK(f.app.call("history.get") == history);
		CHECK(f.app.call("project.get") == project);
		CHECK(unadded());
		CHECK(f.display.automationShown(other));
	};

	// The copy has no lane of the parameter: a pan lane onto the volume automation track.
	REQUIRE(f.app.call("automation.copy", {{"lanes", {pan}}}).at("lanes") == 1);
	paste();
	// The paste fails, here for an empty locator range.
	REQUIRE(f.app.call("automation.copy", {{"lanes", {volume}}}).at("lanes") == 1);
	f.app.call("transport.set", {{"left", 96000}, {"right", 96000}});
	paste();
}

TEST_CASE("two pastes that add lanes with the same automation track selected are two undo steps", "[ui]")
{
	// Each paste is one automation.paste, so one undo step of its own, also with a project lane selected that repeats
	// (#1918, #2018).
	Automation f;
	const Json volume = f.show();
	f.app.call("automationPoint.add",
		{{"lane", volume}, {"points", {{{"position", 0}, {"value", -6.0}}, {{"position", 48000}, {"value", 0.0}}}}});
	f.app.call("transport.set", {{"left", 0}, {"right", 48000}});
	REQUIRE(f.app.call("automation.copy", {{"lanes", {volume}}}).at("lanes") == 1);
	const auto openUnadded = [&]
	{
		const uint64_t track = f.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
		f.app.call("track.set", {{"track", track}, {"timeBase", "linear"}});
		REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(track) + " .automation-toggle").size() == 1; }));
		f.click(row(track) + " .automation-toggle");
		REQUIRE(f.app.waitUntil([&] { return elements(f.app, row(track) + " .automation-row").size() == 1; }));
		return track;
	};
	const uint64_t second = openUnadded();
	const uint64_t third = openUnadded();
	const auto clickLane = [&](uint64_t track, const std::vector<std::string>& modifiers)
	{
		const Json box = elements(f.app, row(track) + " .automation-row").at(0).at("box");
		f.clickAt(box.at("x").get<int>() + 10, box.at("y").get<int>() + (box.at("height").get<int>() / 2), modifiers);
	};
	const auto lanes = [&](uint64_t track)
	{ return f.app.call("automationLane.list", {{"track", track}}).at("lanes").size(); };
	const auto paste = [&](uint64_t track)
	{
		f.app.keys().run("track.pasteAutomationSelectedParameters", Json::object(), f.app.main(), nullptr);
		REQUIRE(f.app.waitUntil([&] { return lanes(track) == 1; }));
		// The automation track shows the added lane.
		REQUIRE(f.app.waitUntil(
			[&]
			{
				const std::vector<uint64_t> selected = f.app.appUi().trackList()->selectedLanes(true);
				return std::none_of(selected.begin(), selected.end(),
					[&](uint64_t lane)
					{
						const Json* unadded = f.display.unaddedLane(lane);
						return unadded != nullptr && unadded->at("track") == track;
					});
			}));
	};
	const size_t steps = f.app.call("history.get").at("steps").size();
	f.app.call("transport.set", {{"left", 96000}, {"right", 144000}});
	clickLane(f.track, {});
	clickLane(second, {"ctrl"});
	paste(second);
	// The volume automation track stays selected, the next paste adds the third track's lane.
	clickLane(third, {"ctrl"});
	paste(third);
	CHECK(f.app.call("history.get").at("steps").size() == steps + 2);
	// One Undo takes back the second paste alone.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return lanes(third) == 0; }));
	CHECK(lanes(second) == 1);
}

TEST_CASE("a track's lanes come between its own lane and its automation tracks, under its row", "[ui]")
{
	Automation f;
	const Json volume = f.show();
	const Json media =
		f.app.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const Json clip = f.app.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const Json event =
		f.app.call("audioEvent.add", {{"track", f.track}, {"clip", clip.at("id")}, {"position", 0}}).at("event");
	f.app.call("event.set", {{"events", {event.at("id")}}, {"lane", 1}});
	// Show Lanes from the track list's context menu command: lanes 0 and 1, then the automation track.
	f.app.appUi().trackList()->select({f.track});
	f.app.keys().run("track.toggleLanesSelected", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.display.laneTakes() == std::vector<int>{-1, 0, 1, -1}; }));
	CHECK(f.display.laneAutomation()[3] == volume.get<uint64_t>());
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json trackRow = elements(f.app, row(f.track)).at(0).at("box");
	CHECK(trackRow.at("height").get<int>() == static_cast<int>(4 * ui::g_trackHeight));
	CHECK(elements(f.app, ".automation-row").size() == 1);
	CHECK(elements(f.app, ".automation-row").at(0).at("box").at("y") == f.box(volume).at("y"));
	// The command again hides them.
	f.app.keys().run("track.toggleLanesSelected", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.display.laneTakes() == std::vector<int>{-1, -1}; }));
}

TEST_CASE(
	"automation points and markers or time signature events deselect each other, and Delete deletes the last", "[ui]")
{
	// The project window selects events, automation points, markers or time signature events (#2011).
	Automation f;
	const Json volume = f.show();
	f.app.call("track.add", {{"type", "signature"}});
	for (const int bar : {2, 3})
		f.app.call("signature.add", {{"bar", bar}, {"numerator", 3}, {"denominator", 4}});
	const uint64_t marker = f.app.call("marker.add", {{"position", "cursor"}}).at("marker").at("id").get<uint64_t>();
	ui::MarkerLines& markers = *f.app.appUi().markerLines();
	ui::SignatureRow& signatures = f.app.appUi().signatureLane()->row();
	REQUIRE(f.app.waitUntil([&] { return !markers.loading() && elements(f.app, ".signature-event").size() == 3; }));
	// Clicks on a time signature event, on another one each time, as two soon after each other open it.
	const auto selectSignature = [&](int event)
	{
		f.app.ui().click(std::nullopt, {".signature-event", event, std::nullopt}, ui::MouseButton::Left, 1, {});
		f.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(signatures.selection().size() == 1);
	};
	const auto selectPoint = [&]
	{
		f.app.call("automationPoint.add", {{"lane", volume}, {"points", {{{"position", 48000}, {"value", -27.0}}}}});
		f.settle(volume);
		f.clickAt(f.x(1.0), f.y(volume, 0.5));
		REQUIRE(f.display.selectedPoints().size() == 1);
	};
	const auto points = [&] { return f.lane(volume).at("points").size(); };

	// A marker, then points: Delete removes the points, which were selected last.
	markers.pick({marker});
	selectPoint();
	CHECK(f.app.waitUntil([&] { return markers.selection().empty(); }));
	f.app.keys().run("edit.delete", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return points() == 0; }));
	CHECK(f.app.call("project.get").at("markers").size() == 1);
	// A time signature event, then points: the same.
	selectSignature(1);
	selectPoint();
	CHECK(f.app.waitUntil([&] { return signatures.selection().empty(); }));
	f.app.keys().run("edit.delete", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return points() == 0; }));
	CHECK(f.app.call("project.get").at("signatures").size() == 3);

	// Points, then a marker or a time signature event: the points go.
	selectPoint();
	markers.pick({marker});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.display.selectedPoints().empty());
	CHECK(markers.selection() == std::vector<uint64_t>{marker});
	f.settle(volume);
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(f.display.selectedPoints().size() == 1);
	selectSignature(2);
	CHECK(f.display.selectedPoints().empty());
	f.app.keys().run("edit.delete", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("signatures").size() == 2; }));
	CHECK(points() == 1);

	// Points, then a marker in the markers window, whose Sync Selection selects it in the project window: it stays
	// selected there and the points go.
	f.clickAt(f.x(1.0), f.y(volume, 0.5));
	REQUIRE(f.display.selectedPoints().size() == 1);
	f.app.ui().key(std::nullopt, "Mod+M");
	REQUIRE(f.app.waitUntil(
		[&] { return f.app.appUi().markers() != nullptr && f.app.appUi().markers()->markers().size() == 1; }));
	f.app.ui().click(
		f.app.appUi().markersWindow(), {"#marker-rows .type", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(f.app.waitUntil([&] { return markers.selection() == std::vector<uint64_t>{marker}; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.display.selectedPoints().empty());
	CHECK(markers.selection() == std::vector<uint64_t>{marker});
}
