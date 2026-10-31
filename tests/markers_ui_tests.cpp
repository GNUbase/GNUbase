// SPDX-License-Identifier: GPL-3.0-or-later
// Markers in the UI (#91): marker lines and marker events in the event display on the fake window system, following a
// real engine, and the markers window and the marker key commands in the app's UI.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/markers.h"
#include "ui/shell.h"
#include "ui/timeformat.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
	// that a dp is a pixel, following an engine on the dummy device.
	struct Fixture
	{
		Fixture()
		{
			ui.shell.eachStep([this] { return binding.update(); });
			step();
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

		Rml::Element& element(const char* selector) const
		{
			Rml::Element* found = document.QuerySelector(selector);
			REQUIRE(found != nullptr);
			return *found;
		}

		const std::vector<ui::EventLanes::Lane>& lanes(const char* selector) const
		{
			auto* lanes = dynamic_cast<ui::EventLanes*>(&element(selector));
			REQUIRE(lanes != nullptr);
			return lanes->lanes();
		}

		// Where the marker lines are, in seconds.
		std::vector<double> lines() const
		{
			std::vector<double> starts;
			for (const ui::EventLanes::Lane& lane : lanes("event-lanes.marker-lines"))
				for (const ui::TimeSpan& line : lane.events)
					starts.push_back(line.start);
			return starts;
		}

		// A point over the event lanes, x dp from their left and y dp from their top.
		ui::Target lanesAt(double x, double y) const
		{
			Rml::Element& lanes = element("event-lanes.events");
			return point(lanes.GetAbsoluteLeft() + x, lanes.GetAbsoluteTop() + y);
		}

		// An audio track, then marker track 1 with a position marker at 1 s and a cycle marker "Verse" from 2 to 4 s,
		// then marker track 2, active, with a position marker at 5 s.
		void addMarkers()
		{
			call("track.add", {{"type", "audio"}});
			first = call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
			call("marker.add", {{"position", 2 * g_quarter}});
			call("marker.add",
				{{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});
			second = call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
			call("marker.add", {{"position", 10 * g_quarter}});
			REQUIRE(waitUntil([this] { return lines() == std::vector<double>{5.0}; }));
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::MarkerLines markers{display, binding};
		ui::Automation automation{ui.shell};
		uint64_t first = 0;
		uint64_t second = 0;
	};
}

TEST_CASE("marker lines show the active marker track's markers, every track's or none", "[unit]")
{
	Fixture fixture;
	fixture.addMarkers();

	// Behind the events of every track, on the event display's timeline.
	Rml::Element& area = fixture.element(".event-area");
	CHECK(area.GetChild(0)->IsClassSet("marker-lines"));
	CHECK(area.GetChild(1)->IsClassSet("events"));
	Rml::Element& lines = fixture.element("event-lanes.marker-lines");
	CHECK(lines.GetAttribute<double>("scale", -1.0) == fixture.display.view().scale);
	fixture.display.zoomIn();
	fixture.step();
	CHECK(lines.GetAttribute<double>("scale", -1.0) == fixture.display.view().scale);
	CHECK(lines.GetAttribute<double>("start", -1.0) == fixture.display.view().start);

	// The active track's: cycle markers at their start and end.
	fixture.call("marker.activateTrack", {{"track", fixture.first}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.lines() == std::vector<double>{1.0, 2.0, 4.0}; }));

	// The user setting shows every marker track's, or none.
	fixture.call("settings.set", {{"key", "ui.markerLines"}, {"value", "all"}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.lines() == std::vector<double>{1.0, 2.0, 4.0, 5.0}; }));
	CHECK(fixture.markers.mode() == ui::MarkerLineMode::All);
	fixture.call("settings.set", {{"key", "ui.markerLines"}, {"value", "off"}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.lines().empty(); }));
}

TEST_CASE("marker events show on the marker tracks' lanes, labelled with their numbers and names", "[unit]")
{
	Fixture fixture;
	fixture.addMarkers();

	// A lane per track, over the events; markers only on the marker tracks' lanes.
	const auto& lanes = fixture.lanes("event-lanes.marker-events");
	REQUIRE(lanes.size() == 3);
	CHECK(lanes[0].events.empty());
	REQUIRE(lanes[1].events.size() == 2);
	CHECK(lanes[1].events[0].start == Approx(1.0));
	CHECK(lanes[1].events[0].length == 0.0);
	CHECK(lanes[1].events[1].start == Approx(2.0));
	CHECK(lanes[1].events[1].length == Approx(2.0));
	REQUIRE(lanes[2].events.size() == 1);

	// Labels where the markers are: 0.01 seconds per dp, 40 dp a lane. Cycle markers' numbers are in brackets.
	Rml::ElementList labels;
	fixture.document.QuerySelectorAll(labels, ".marker-label");
	REQUIRE(labels.size() == 3);
	CHECK(labels[0]->GetInnerRML() == "1");
	CHECK(labels[1]->GetInnerRML() == "[1] Verse");
	CHECK(labels[1]->IsClassSet("cycle"));
	CHECK_FALSE(labels[1]->IsClassSet("active"));
	CHECK(labels[2]->IsClassSet("active"));
	Rml::Element& events = fixture.element("event-lanes.events");
	CHECK(labels[1]->GetAbsoluteLeft() - events.GetAbsoluteLeft() == Approx(200.0).margin(0.5));
	CHECK(labels[1]->GetAbsoluteTop() - events.GetAbsoluteTop() == Approx(40.0).margin(0.5));

	// They follow the vertical zoom.
	fixture.display.zoomInVertically();
	fixture.step();
	CHECK(fixture.lanes("event-lanes.marker-events")[1].height == Approx(50.0));
	CHECK(labels[1]->GetAbsoluteTop() - events.GetAbsoluteTop() == Approx(50.0).margin(0.5));
}

TEST_CASE("on a marker track, Alt-click adds a marker and a double click on a cycle marker sets the locators", "[unit]")
{
	Fixture fixture;
	fixture.addMarkers();
	const auto count = [&fixture](uint64_t track)
	{ return fixture.call("marker.list", {{"track", track}}).at("markers").size(); };

	// Alt-click adds a position marker there on the marker track under the mouse, nowhere else.
	fixture.automation.click(std::nullopt, fixture.lanesAt(600, 60), ui::MouseButton::Left, 1, {"alt"});
	REQUIRE(fixture.waitUntil([&] { return count(fixture.first) == 3; }));
	const Json added = fixture.call("marker.list", {{"track", fixture.first}}).at("markers").back();
	CHECK(added.at("position") == 12 * g_quarter); // 6 s
	fixture.automation.click(std::nullopt, fixture.lanesAt(700, 20), ui::MouseButton::Left, 1, {"alt"});
	fixture.automation.click(std::nullopt, fixture.lanesAt(700, 60), ui::MouseButton::Left, 1, {});
	fixture.step();
	CHECK(count(fixture.first) == 3);

	// A double click on a cycle marker sets the locators to it; with Alt it zooms to it, adding nothing.
	fixture.automation.click(std::nullopt, fixture.lanesAt(300, 60), ui::MouseButton::Left, 2, {});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.call("transport.get").at("left") == 4 * g_quarterSamples; }));
	CHECK(fixture.call("transport.get").at("right") == 8 * g_quarterSamples);
	fixture.automation.click(std::nullopt, fixture.lanesAt(300, 60), ui::MouseButton::Left, 2, {"alt"});
	fixture.step();
	CHECK(fixture.display.view().start == Approx(2.0));
	CHECK(fixture.display.visibleLength() == Approx(2.0));
	CHECK(count(fixture.first) == 3);
}

namespace
{
	ui::Target at(const std::string& selector, int index = 0)
	{
		return {selector, index, std::nullopt};
	}

	int64_t cursor(test::UiApp& app)
	{
		return app.call("transport.get").at("position").get<int64_t>();
	}

	Json markerList(test::UiApp& app)
	{
		return app.call("marker.list").at("markers");
	}

	// The markers window's document, which must be open.
	Rml::ElementDocument& markersDocument(test::UiApp& app)
	{
		ui::Window* window = app.shell().find(app.appUi().markersWindow());
		REQUIRE(window != nullptr);
		Rml::ElementDocument* document = window->context().GetDocument(0);
		REQUIRE(document != nullptr);
		return *document;
	}
}

TEST_CASE("the markers window lists the active marker track's markers to locate, edit, add and remove", "[ui]")
{
	test::UiApp app;
	const auto track = app.call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
	app.call("marker.add", {{"position", 2 * g_quarter}, {"name", "Intro"}});
	app.call("marker.add", {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});

	// Mod+M opens it in a window of its own.
	app.ui().key(std::nullopt, "Mod+M");
	REQUIRE(app.waitUntil(
		[&app] { return app.appUi().markers() != nullptr && app.appUi().markers()->markers().size() == 2; }));
	const ui::WindowId window = app.appUi().markersWindow();
	ui::MarkersWindow& markers = *app.appUi().markers();
	Rml::ElementDocument& document = markersDocument(app);
	Rml::ElementList rows;
	document.QuerySelectorAll(rows, "#marker-rows tr");
	REQUIRE(rows.size() == 2);
	const ui::TimeContext context;
	CHECK(rows[1]->QuerySelector(".type")->GetInnerRML() == "Cycle");
	CHECK(
		rows[1]->QuerySelector(".position")->GetInnerRML() == ui::formatTime(ui::TimeFormat::BarsBeats, 2.0, context));
	CHECK(rows[1]->QuerySelector(".end")->GetInnerRML() == ui::formatTime(ui::TimeFormat::BarsBeats, 4.0, context));
	auto* tracks = dynamic_cast<Rml::ElementFormControlSelect*>(document.GetElementById("marker-track"));
	REQUIRE(tracks != nullptr);
	CHECK(tracks->GetValue() == std::to_string(track));

	// A click in the locate column locates there, and the arrow shows at the marker at the cursor.
	app.ui().click(window, at("#marker-rows .locate", 1), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return cursor(app) == 4 * g_quarterSamples; }));
	REQUIRE(app.waitUntil([&document] { return document.QuerySelector("#marker-rows tr.at-cursor") != nullptr; }));
	CHECK(document.QuerySelector("#marker-rows tr.at-cursor")->GetAttribute<uint64_t>("value", 0) ==
		markers.markers()[1].id);

	// Descriptions and id numbers are edited in place, Enter taking them.
	app.ui().click(window, at("#marker-rows .description input", 0), ui::MouseButton::Left, 1, {});
	app.ui().key(window, "Mod+A");
	app.ui().text(window, "Start");
	app.ui().key(window, "Enter");
	REQUIRE(app.waitUntil([&markers] { return markers.markers()[0].name == "Start"; })); // the rows again
	app.ui().click(window, at("#marker-rows .number input", 1), ui::MouseButton::Left, 1, {});
	app.ui().key(window, "Mod+A");
	app.ui().text(window, "4");
	app.ui().key(window, "Enter");
	REQUIRE(app.waitUntil([&markers] { return markers.markers()[1].number == 4; }));

	// The functions: reassign the cycle markers' ids, move the selected markers to the cursor.
	app.ui().click(window, at("#marker-functions"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#reassign-cycle-ids"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return markers.markers()[1].number == 1; }));
	app.ui().click(window, at("#marker-rows .type", 0), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return markers.selection().size() == 1; }));
	app.call("transport.locate", {{"position", 12 * g_quarterSamples}});
	app.ui().click(window, at("#marker-functions"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#move-to-cursor"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return markers.markers()[1].start == Approx(6.0); }));
	CHECK(markerList(app)[1].at("position") == 12 * g_quarter);
	CHECK(markerList(app)[1].at("name") == "Start");

	// Enter in the list locates to the selected marker.
	app.ui().click(window, at("#marker-rows .type", 0), ui::MouseButton::Left, 1, {});
	app.ui().key(window, "Enter");
	REQUIRE(app.waitUntil([&app] { return cursor(app) == 4 * g_quarterSamples; }));

	// Add and remove: a position marker at the cursor, a cycle marker at the locators, the selected ones go.
	app.call("transport.set", {{"left", g_quarterSamples}, {"right", 2 * g_quarterSamples}});
	app.ui().click(window, at("#add-position-marker"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#add-cycle-marker"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return markers.markers().size() == 4; }));
	CHECK(markers.markers()[0].cycle);
	app.ui().click(window, at("#marker-rows .type", 0), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#remove-marker"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return markers.markers().size() == 3; }));
	CHECK_FALSE(markers.markers()[0].cycle);

	// The marker track list activates the marker track picked.
	const auto other = app.call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&markers] { return markers.markers().empty(); }));
	CHECK(tracks->GetNumOptions() == 2);
	CHECK(tracks->GetValue() == std::to_string(other));
	tracks->SetSelection(0);
	REQUIRE(app.waitUntil([&app, track] { return app.call("project.get").at("activeMarkerTrack") == track; }));
	REQUIRE(app.waitUntil([&markers] { return markers.markers().size() == 3; }));

	// Mod+M again closes it, also from its own window.
	app.ui().key(window, "Mod+M");
	REQUIRE(app.waitUntil([&app] { return app.appUi().markers() == nullptr; }));
	CHECK(app.shell().find(window) == nullptr);
}

TEST_CASE("the markers window's locate arrow follows the project's sample rate", "[ui]")
{
	// The window learns the cursor before the project; the arrow converts it at the project's rate, not at 48 kHz.
	test::UiApp app;
	app.call("track.add", {{"type", "marker"}});
	app.call("project.set", {{"sampleRate", 44100.0}});
	app.call("marker.add", {{"position", g_quarter}});
	app.call("marker.add", {{"position", 2 * g_quarter}}); // one second: sample 44100
	app.call("transport.locate", {{"position", 44100}});

	app.ui().key(std::nullopt, "Mod+M");
	REQUIRE(app.waitUntil(
		[&app] { return app.appUi().markers() != nullptr && app.appUi().markers()->markers().size() == 2; }));
	Rml::ElementDocument& document = markersDocument(app);
	REQUIRE(app.waitUntil([&document] { return document.QuerySelector("#marker-rows tr.at-cursor") != nullptr; }));
	CHECK(document.QuerySelector("#marker-rows tr.at-cursor")->GetAttribute<uint64_t>("value", 0) ==
		app.appUi().markers()->markers()[1].id);
}

TEST_CASE("key commands locate to markers, set them and recall cycle markers", "[ui]")
{
	test::UiApp app;
	app.call("track.add", {{"type", "marker"}});
	app.call("marker.add", {{"position", 2 * g_quarter}});
	app.call("marker.add", {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}});

	app.ui().key(std::nullopt, "Shift+N");
	REQUIRE(app.waitUntil([&app] { return cursor(app) == 2 * g_quarterSamples; }));
	app.ui().key(std::nullopt, "Shift+N");
	REQUIRE(app.waitUntil([&app] { return cursor(app) == 4 * g_quarterSamples; }));
	app.ui().key(std::nullopt, "Shift+B");
	REQUIRE(app.waitUntil([&app] { return cursor(app) == 2 * g_quarterSamples; }));

	// Set Marker 3 at the cursor, then To Marker 3 from elsewhere.
	app.call("transport.locate", {{"position", 10 * g_quarterSamples}});
	app.ui().key(std::nullopt, "Mod+3");
	REQUIRE(app.waitUntil([&app] { return markerList(app).size() == 3; }));
	app.call("transport.locate", {{"position", 0}});
	app.ui().key(std::nullopt, "Shift+3");
	REQUIRE(app.waitUntil([&app] { return cursor(app) == 10 * g_quarterSamples; }));

	// Recall Cycle Marker 1 sets the locators; Insert adds a position marker at the cursor.
	app.ui().key(std::nullopt, "Shift+Num1");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("right") == 8 * g_quarterSamples; }));
	CHECK(app.call("transport.get").at("left") == 4 * g_quarterSamples);
	app.ui().key(std::nullopt, "Insert");
	REQUIRE(app.waitUntil([&app] { return markerList(app).size() == 4; }));
}
