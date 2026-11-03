// SPDX-License-Identifier: GPL-3.0-or-later
// Automation tracks (#121, docs/features/automation-tracks.md) in the app's project window, driven with ui::Automation
// as MCP's UI tools drive it: showing and hiding them, the parameter selector, the drawing and line tools, selecting,
// moving and scaling points, and the static value line.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Element.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

		// Shows the track's automation and waits for its first automation track, a volume lane.
		Json show()
		{
			click(row(track) + " .automation-toggle");
			REQUIRE(app.waitUntil([&] { return elements(app, ".automation-row .automation-parameter").size() == 1; }));
			REQUIRE(app.waitUntil(
				[&] { return elements(app, ".automation-parameter").at(0).value("text", "") == "Volume"; }));
			const Json all = lanes();
			REQUIRE(all.size() == 1);
			return all[0].at("id");
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
	// Show/Hide Automation opens an automation track for the first parameter (reference manual p. 880).
	const Json volume = f.show();
	CHECK(f.lane(volume).at("target").at("type") == "volume");
	CHECK(f.lane(volume).at("read") == false);
	CHECK(hasClass(elements(f.app, row(f.track) + " .automation-toggle").at(0), "checked"));
	REQUIRE(f.display.laneAutomation().size() == 2);
	CHECK(f.display.laneAutomation()[1] == volume.get<uint64_t>());
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

	// The parameter selector lists the track's parameters, which the search narrows; picking one shows it instead,
	// and the unused lane it showed goes.
	f.app.ui().click(std::nullopt, {".automation-parameter", 1, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(hasClass(elements(f.app, "#automation-parameter-menu").at(0), "open"));
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.automation-target").size() == 5);
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.checked").at(0).value("text", "") == "Pan");
	f.click("#automation-parameter-menu .automation-search");
	f.app.ui().text(std::nullopt, "LEFT");
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(f.app, "#automation-parameter-menu menuitem.automation-target:not(.hidden)").size() == 1);
	f.click("#automation-parameter-menu menuitem.automation-target:not(.hidden)");
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return elements(f.app, ".automation-parameter").size() == 2 &&
				elements(f.app, ".automation-parameter").at(1).value("text", "") == "Pan left";
		}));
	REQUIRE(f.lanes().size() == 2);
	CHECK(f.lanes()[1].at("target").at("type") == "panLeft");
	// One undo step brings the pan lane back.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == 2 && f.lanes()[1].at("target").at("type") == "pan"; }));
	f.app.call("history.redo");
	REQUIRE(f.app.waitUntil([&] { return f.lanes()[1].at("target").at("type") == "panLeft"; }));

	// Remove Parameter removes the lane with its points and closes its automation track.
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 2; }));
	f.app.ui().click(std::nullopt, {".automation-parameter", 1, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.click("#automation-parameter-menu menuitem[automation-function=\"remove\"]");
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").size() == 1; }));
	CHECK(f.lanes().size() == 1);

	// Hiding closes the automation tracks; Show All Used Automation opens those with points, on every track.
	f.click(row(f.track) + " .automation-toggle");
	REQUIRE(f.app.waitUntil([&] { return elements(f.app, ".automation-row").empty(); }));
	CHECK(f.display.laneAutomation() == std::vector<uint64_t>{0});
	f.app.call("automationPoint.add", {{"lane", volume}, {"points", {{{"position", 0}, {"value", -6.0}}}}});
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
	// Remove Unused Parameters removes the track's lanes without points.
	f.app.ui().click(
		std::nullopt, {row(other) + " .automation-parameter", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.click("#automation-parameter-menu menuitem[automation-function=\"removeUnused\"]");
	REQUIRE(f.app.waitUntil([&] { return f.app.call("automationLane.list", {{"track", other}}).at("lanes").empty(); }));
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
