// SPDX-License-Identifier: GPL-3.0-or-later
// Editing markers on the marker track (#376): dragging, resizing, adding and renaming markers with the mouse, the
// erase tool and range selections by markers in the event display on the fake window system, following a real engine;
// and in the app's UI the marker track's controls in the track list, Delete and the Show Marker Lines menu.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/markers.h"
#include "ui/shell.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter; // half a second at the default 120 BPM
	constexpr int64_t g_quarterSamples = 24000;

	ui::Target point(double x, double y)
	{
		return {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	}

	// The event display with its markers, filling a 1000 by 400 window of the fake UI at a monitor scale of 1, so
	// that a dp is a pixel and, at the default zoom, 0.01 seconds; tracks are 40 dp high.
	struct Fixture
	{
		Fixture()
		{
			ui.shell.eachStep([this] { return binding.update(); });
			step();
			// An audio track, then marker track 1 with a position marker at 1 s and a cycle marker "Verse" from 2 to
			// 4 s, then marker track 2, active, with a position marker at 5 s.
			audio = call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
			first = call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
			position = call("marker.add", {{"position", 2 * g_quarter}}).at("marker").at("id").get<uint64_t>();
			cycle = call("marker.add",
				{{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}})
						.at("marker")
						.at("id")
						.get<uint64_t>();
			second = call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
			call("marker.add", {{"position", 10 * g_quarter}});
			REQUIRE(waitUntil([this] { return markers.markers().markers.size() == 3; }));
		}

		static ui::WindowSpec spec()
		{
			ui::Fonts(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "fonts").load();
			ui::WindowSpec spec;
			spec.bounds = ui::Rect{0, 0, 1000, 400};
			return spec;
		}

		Rml::ElementDocument& load() const
		{
			const std::string base = std::string(GNUBASE_RESOURCES_DIR) + "/styles/base.rcss";
			Rml::ElementDocument* document =
				window.context().LoadDocumentFromMemory(R"(<rml><head><link type="text/rcss" href=")" + base +
					R"("/><style>body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; font-family: Inter; }
				#display { position: absolute; left: 0; top: 0; right: 0; bottom: 0; }</style></head>
				<body><div id="display" class="event-display"/></body></rml>)");
			REQUIRE(document != nullptr);
			document->Show();
			return *document;
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = engine::Client(engine).call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		void step() { ui.shell.step(milliseconds(0)); }

		bool waitUntil(const std::function<bool()>& done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!done())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				ui.shell.step(milliseconds(5));
			}
			step();
			return true;
		}

		// A point over the event lanes, x dp from their left and y dp from their top.
		ui::Target lanesAt(double x, double y) const
		{
			Rml::Element* lanes = document.QuerySelector("event-lanes.events");
			REQUIRE(lanes != nullptr);
			return point(lanes->GetAbsoluteLeft() + x, lanes->GetAbsoluteTop() + y);
		}

		void drag(double fromX, double fromY, double toX, double toY, const std::vector<std::string>& keys = {})
		{
			automation.drag(std::nullopt, lanesAt(fromX, fromY), lanesAt(toX, toY), keys);
			step();
		}

		Json list(uint64_t track) { return call("marker.list", {{"track", track}}).at("markers"); }

		// A marker as marker.list has it, or null if it is gone.
		Json marker(uint64_t id)
		{
			for (const uint64_t track : {first, second})
				for (const Json& each : list(track))
					if (each.at("id") == id)
						return each;
			return nullptr;
		}

		// Whether the markers the event display shows are the project's, as the next press finds them.
		bool synced()
		{
			const std::vector<ui::MarkerItem> project = ui::readMarkers(call("project.get")).markers;
			const std::vector<ui::MarkerItem>& shown = markers.markers().markers;
			return !markers.loading() && project.size() == shown.size() &&
				std::equal(project.begin(), project.end(), shown.begin(),
					[](const ui::MarkerItem& a, const ui::MarkerItem& b)
					{ return a.id == b.id && a.track == b.track && a.start == b.start && a.end == b.end; });
		}

		// Waits until the marker's field is the value, in the project and in the event display.
		bool waitFor(uint64_t id, const std::string& field, const Json& value)
		{
			return waitUntil(
				[&]
				{
					const Json found = marker(id);
					return found.is_object() && found.at(field) == value && synced();
				});
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::MarkerLines markers{display, binding};
		ui::Automation automation{ui.shell};
		uint64_t audio = 0;
		uint64_t first = 0;
		uint64_t second = 0;
		uint64_t position = 0;
		uint64_t cycle = 0;
	};
}

TEST_CASE(
	"on the marker track, markers drag, cycle markers move by their upper border and resize by their handles", "[unit]")
{
	Fixture fixture;

	// A position marker by its line, from 1 s to 1.5 s, and selected.
	fixture.drag(100, 70, 150, 70);
	REQUIRE(fixture.waitFor(fixture.position, "position", 3 * g_quarter));
	CHECK(fixture.markers.selection() == std::vector<uint64_t>{fixture.position});

	// A cycle marker by its upper border, half a second on, keeping its length.
	fixture.drag(300, 42, 350, 42);
	REQUIRE(fixture.waitFor(fixture.cycle, "position", 5 * g_quarter));
	CHECK(fixture.marker(fixture.cycle).at("end") == 9 * g_quarter);

	// Its start handle and its end handle resize it.
	fixture.drag(250, 70, 200, 70);
	REQUIRE(fixture.waitFor(fixture.cycle, "position", 4 * g_quarter));
	CHECK(fixture.marker(fixture.cycle).at("end") == 9 * g_quarter);
	fixture.drag(450, 70, 500, 70);
	REQUIRE(fixture.waitFor(fixture.cycle, "end", 10 * g_quarter));
	CHECK(fixture.marker(fixture.cycle).at("position") == 4 * g_quarter);

	// A press inside a cycle marker, below its border, only selects it.
	fixture.drag(300, 70, 350, 70);
	CHECK(fixture.markers.selection() == std::vector<uint64_t>{fixture.cycle});
	CHECK(fixture.marker(fixture.cycle).at("position") == 4 * g_quarter);

	// Several selected markers move together, in one undo step.
	// (On its label: a press where a click just was would be a double click.)
	fixture.automation.click(std::nullopt, fixture.lanesAt(152, 47), ui::MouseButton::Left, 1, {"ctrl"});
	fixture.step();
	REQUIRE(fixture.markers.selection().size() == 2);
	fixture.drag(150, 70, 250, 70);
	REQUIRE(fixture.waitFor(fixture.position, "position", 5 * g_quarter));
	REQUIRE(fixture.waitFor(fixture.cycle, "position", 6 * g_quarter));
	CHECK(fixture.marker(fixture.cycle).at("end") == 12 * g_quarter);
	fixture.call("history.undo");
	CHECK(fixture.marker(fixture.position).at("position") == 3 * g_quarter);
	CHECK(fixture.marker(fixture.cycle).at("position") == 4 * g_quarter);
	REQUIRE(fixture.waitUntil([&] { return fixture.synced(); }));

	// Snapping to the grid: bars, two seconds at 120 BPM.
	fixture.call("snap.set", {{"on", true}});
	REQUIRE(fixture.waitUntil([&] { return fixture.display.snap().on; }));
	fixture.automation.click(std::nullopt, fixture.lanesAt(700, 70), ui::MouseButton::Left, 1, {}); // selects none
	fixture.step();
	CHECK(fixture.markers.selection().empty());
	fixture.drag(150, 70, 230, 70);
	REQUIRE(fixture.waitFor(fixture.position, "position", 4 * g_quarter));
	fixture.call("snap.set", {{"on", false}});
	REQUIRE(fixture.waitUntil([&] { return !fixture.display.snap().on; }));

	// Dropped on another marker track, a marker goes there with the first free number.
	fixture.drag(200, 70, 300, 110);
	REQUIRE(fixture.waitFor(fixture.position, "track", fixture.second));
	CHECK(fixture.marker(fixture.position).at("position") == 6 * g_quarter);
	CHECK(fixture.marker(fixture.position).at("number") == 2);
}

TEST_CASE("on the marker track, Ctrl-drag adds a cycle marker and Shift adds and renames at once", "[unit]")
{
	Fixture fixture;
	const auto count = [&fixture] { return fixture.list(fixture.first).size(); };

	// Ctrl and a drag on an empty spot adds a cycle marker over the stretch dragged.
	fixture.drag(600, 60, 700, 60, {"ctrl"});
	REQUIRE(fixture.waitUntil([&] { return count() == 3; }));
	const Json added = fixture.list(fixture.first).back();
	CHECK(added.at("type") == "cycle");
	CHECK(added.at("position") == 12 * g_quarter);
	CHECK(added.at("end") == 14 * g_quarter);
	CHECK_FALSE(fixture.document.QuerySelector(".marker-name-field")->IsClassSet("shown"));

	// Shift+Alt and a click adds a position marker and opens its name to type.
	fixture.automation.click(std::nullopt, fixture.lanesAt(800, 60), ui::MouseButton::Left, 1, {"shift", "alt"});
	REQUIRE(fixture.waitUntil([&] { return count() == 4; }));
	REQUIRE(fixture.waitUntil([&] { return fixture.document.QuerySelector(".marker-name-field.shown") != nullptr; }));
	fixture.automation.text(std::nullopt, "Bridge");
	fixture.automation.key(std::nullopt, "Enter");
	REQUIRE(fixture.waitUntil([&] { return fixture.list(fixture.first).back().at("name") == "Bridge"; }));
	CHECK(fixture.list(fixture.first).back().at("position") == 16 * g_quarter);
	CHECK_FALSE(fixture.document.QuerySelector(".marker-name-field")->IsClassSet("shown"));

	// Shift+Ctrl and a drag adds a cycle marker to rename.
	fixture.drag(850, 60, 950, 60, {"shift", "ctrl"});
	REQUIRE(fixture.waitUntil([&] { return count() == 5; }));
	REQUIRE(fixture.waitUntil([&] { return fixture.document.QuerySelector(".marker-name-field.shown") != nullptr; }));
	fixture.automation.text(std::nullopt, "Outro");
	fixture.automation.key(std::nullopt, "Enter");
	REQUIRE(fixture.waitUntil([&] { return fixture.list(fixture.first).back().at("name") == "Outro"; }));
	CHECK(fixture.list(fixture.first).back().at("type") == "cycle");
}

TEST_CASE("the erase tool deletes a marker, with Alt every later one on its track", "[unit]")
{
	Fixture fixture;
	fixture.call("marker.add", {{"track", fixture.first}, {"position", 12 * g_quarter}});
	fixture.display.setTool(ui::Tool::Erase);

	REQUIRE(fixture.waitUntil([&] { return fixture.synced(); }));
	fixture.automation.click(std::nullopt, fixture.lanesAt(100, 70), ui::MouseButton::Left, 1, {});
	REQUIRE(fixture.waitUntil([&] { return fixture.marker(fixture.position).is_null() && fixture.synced(); }));
	CHECK(fixture.list(fixture.first).size() == 2);

	// Alt-click on the cycle marker: it and the marker after it go, the other track's stay.
	fixture.automation.click(std::nullopt, fixture.lanesAt(300, 70), ui::MouseButton::Left, 1, {"alt"});
	INFO(fixture.list(fixture.first).dump());
	REQUIRE(fixture.waitUntil([&] { return fixture.list(fixture.first).empty(); }));
	CHECK(fixture.list(fixture.second).size() == 1);
}

TEST_CASE("with the range tool, a double click on the marker track selects the range between markers", "[unit]")
{
	Fixture fixture;
	fixture.display.setTool(ui::Tool::RangeSelection);

	// Between the position marker at 1 s and the cycle marker's start at 2 s, across every track.
	fixture.automation.click(std::nullopt, fixture.lanesAt(150, 70), ui::MouseButton::Left, 2, {});
	REQUIRE(fixture.waitUntil(
		[&] { return fixture.display.range() && fixture.display.range()->start == 2 * g_quarterSamples; }));
	const ui::RangeSelection range =
		fixture.display.range().value_or(ui::RangeSelection{0, 0, std::vector<uint64_t>()});
	CHECK(range.end == 4 * g_quarterSamples);
	CHECK(range.tracks == std::vector<uint64_t>{fixture.audio, fixture.first, fixture.second});
	CHECK(fixture.markers.selection().empty());
}

namespace
{
	ui::Target at(const std::string& selector, int index = 0)
	{
		return {selector, index, std::nullopt};
	}

	// The marker track's control in the track list.
	std::string control(uint64_t track, const std::string& name)
	{
		return "#track-list listitem[value=\"" + std::to_string(track) + "\"] [control=" + name + "]";
	}

	void click(test::UiApp& app, const std::string& selector, const std::vector<std::string>& keys = {})
	{
		app.ui().click(std::nullopt, at(selector), ui::MouseButton::Left, 1, keys);
		app.shell().step(milliseconds(0));
	}

	// Chooses the drop-down list's index-th item after the first, which only names it.
	void choose(test::UiApp& app, const std::string& selector, int index)
	{
		click(app, selector);
		for (int i = 0; i < index; ++i)
			app.ui().key(std::nullopt, "Down");
		click(app, selector);
	}

	bool exists(test::UiApp& app, const std::string& selector)
	{
		return !app.ui().dom(std::nullopt, selector, 0).at("elements").empty();
	}
}

TEST_CASE("the marker track's controls add markers, activate it and locate, cycle and zoom to markers", "[ui]")
{
	test::UiApp app({900, 560});
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	const auto first = app.call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
	const auto second = app.call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
	const auto list = [&app](uint64_t track) { return app.call("marker.list", {{"track", track}}).at("markers"); };
	REQUIRE(app.waitUntil([&] { return exists(app, control(first, "markerZoom")); }));

	// Add Position Marker at the cursor, Add Cycle Marker at the locators.
	app.call("transport.locate", {{"position", 2 * g_quarterSamples}});
	click(app, control(first, "addPositionMarker"));
	REQUIRE(app.waitUntil([&] { return list(first).size() == 1; }));
	CHECK(list(first)[0].at("position") == 2 * g_quarter);
	app.call("transport.set", {{"left", 4 * g_quarterSamples}, {"right", 8 * g_quarterSamples}});
	click(app, control(first, "addCycleMarker"));
	REQUIRE(app.waitUntil([&] { return list(first).size() == 2; }));
	CHECK(list(first)[1].at("type") == "cycle");
	CHECK(list(first)[1].at("end") == 8 * g_quarter);

	// Shift-click adds one and opens its name.
	app.call("transport.set", {{"left", 12 * g_quarterSamples}, {"right", 16 * g_quarterSamples}});
	click(app, control(first, "addCycleMarker"), {"shift"});
	REQUIRE(app.waitUntil([&] { return list(first).size() == 3; }));
	REQUIRE(app.waitUntil([&] { return exists(app, ".marker-name-field.shown"); }));
	app.ui().text(std::nullopt, "Chorus");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return list(first)[2].at("name") == "Chorus"; }));

	// Activate Track: the second one is active, as the newest marker track.
	CHECK(app.call("project.get").at("activeMarkerTrack") == second);
	REQUIRE(app.waitUntil(
		[&]
		{
			return app.ui()
					   .dom(std::nullopt, control(second, "activateMarkerTrack"), 0)
					   .at("elements")
					   .at(0)
					   .at("classes")
					   .dump()
					   .find("checked") != std::string::npos;
		}));
	click(app, control(first, "activateMarkerTrack"));
	REQUIRE(app.waitUntil([&] { return app.call("project.get").at("activeMarkerTrack") == first; }));

	// Locate: the cursor goes to the marker, which is selected.
	choose(app, control(first, "markerLocate"), 2); // the cycle marker from 2 s
	REQUIRE(app.waitUntil([&] { return app.call("transport.get").at("position") == 4 * g_quarterSamples; }));
	const uint64_t verse = list(first)[1].at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return app.appUi().markerLines()->selection() == std::vector<uint64_t>{verse}; }));

	// Cycle: the locators go to the cycle marker; Zoom: the view shows it.
	app.call("transport.set", {{"left", 0}, {"right", 0}});
	choose(app, control(first, "markerCycle"), 1);
	REQUIRE(app.waitUntil([&] { return app.call("transport.get").at("right") == 8 * g_quarterSamples; }));
	CHECK(app.call("transport.get").at("left") == 4 * g_quarterSamples);
	choose(app, control(first, "markerZoom"), 2);
	REQUIRE(app.waitUntil([&] { return display.view().start == Approx(6.0); }));
	CHECK(display.visibleLength() == Approx(2.0));

	// Delete removes the selected markers.
	app.appUi().markerLines()->setSelection({verse});
	app.ui().key(std::nullopt, "Delete");
	REQUIRE(app.waitUntil([&] { return list(first).size() == 2; }));
}

TEST_CASE("the track list's context menu shows the marker lines of the active marker track, all or none", "[ui]")
{
	test::UiApp app({900, 560});
	app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return exists(app, control(track, "addPositionMarker")); }));
	const auto setting = [&app] { return app.call("settings.get", {{"key", "ui.markerLines"}}).at("value"); };
	for (const char* mode : {"all", "off", "active"})
	{
		app.ui().click(std::nullopt, at("#track-list listitem[value=\"" + std::to_string(track) + "\"] .track-name"),
			ui::MouseButton::Right, 1, {});
		app.shell().step(milliseconds(0));
		app.ui().move(std::nullopt, at("#marker-lines-submenu"));
		app.shell().step(milliseconds(0));
		click(app, std::string("#marker-lines-") + mode);
		REQUIRE(app.waitUntil([&] { return setting() == mode; }));
	}
}
