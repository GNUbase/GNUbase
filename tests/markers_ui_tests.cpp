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
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
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
			audio = call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
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
		uint64_t audio = 0;
		uint64_t first = 0;
		uint64_t second = 0;
	};
}

TEST_CASE("marker lines show the active marker track's markers, every track's or none", "[unit]")
{
	Fixture fixture;
	fixture.addMarkers();

	// Behind the events of every track and the folder tracks' data, on the event display's timeline.
	const Rml::Element& area = fixture.element(".event-area");
	CHECK(area.GetChild(0)->IsClassSet("marker-lines"));
	CHECK(area.GetChild(1)->IsClassSet("folder-data"));
	CHECK(area.GetChild(2)->IsClassSet("events"));
	const Rml::Element& lines = fixture.element("event-lanes.marker-lines");
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

TEST_CASE("marker events and Alt-click follow each track's own height", "[unit]")
{
	Fixture fixture;
	fixture.addMarkers();
	const auto count = [&fixture](uint64_t track)
	{ return fixture.call("marker.list", {{"track", track}}).at("markers").size(); };

	// The audio track twice as high moves the marker tracks' lanes, labels and mouse targets down.
	fixture.display.setTrackHeight(fixture.audio, 80);
	fixture.step();
	CHECK(fixture.lanes("event-lanes.marker-events")[0].height == Approx(80.0));
	Rml::ElementList labels;
	fixture.document.QuerySelectorAll(labels, ".marker-label");
	REQUIRE(labels.size() == 3);
	Rml::Element& events = fixture.element("event-lanes.events");
	CHECK(labels[1]->GetAbsoluteTop() - events.GetAbsoluteTop() == Approx(80.0).margin(0.5));
	fixture.automation.click(std::nullopt, fixture.lanesAt(600, 100), ui::MouseButton::Left, 1, {"alt"});
	REQUIRE(fixture.waitUntil([&] { return count(fixture.first) == 3; }));
	CHECK(count(fixture.second) == 1);
}

TEST_CASE("marker events and Alt-click skip hidden tracks", "[unit]")
{
	Fixture fixture;
	fixture.addMarkers();
	const auto count = [&fixture](uint64_t track)
	{ return fixture.call("marker.list", {{"track", track}}).at("markers").size(); };
	// The labels again each time, since a new marker replaces them.
	const auto labels = [&fixture]
	{
		Rml::ElementList found;
		fixture.document.QuerySelectorAll(found, ".marker-label");
		return found;
	};
	Rml::Element& events = fixture.element("event-lanes.events");
	const auto top = [&events](Rml::Element* label) { return label->GetAbsoluteTop() - events.GetAbsoluteTop(); };

	// Marker track 1 hidden: its labels hide, marker track 2 moves up into its lane and takes the click.
	fixture.display.setHiddenTracks({fixture.first});
	fixture.step();
	Rml::ElementList hidden = labels();
	REQUIRE(hidden.size() == 3);
	CHECK(hidden[0]->IsClassSet("hidden"));
	CHECK(hidden[1]->IsClassSet("hidden"));
	CHECK_FALSE(hidden[2]->IsClassSet("hidden"));
	CHECK(top(hidden[2]) == Approx(40.0).margin(0.5));
	fixture.automation.click(std::nullopt, fixture.lanesAt(600, 60), ui::MouseButton::Left, 1, {"alt"});
	REQUIRE(fixture.waitUntil([&] { return count(fixture.second) == 2; }));
	CHECK(count(fixture.first) == 2);

	// Shown again, the labels go back to their lanes.
	fixture.display.setHiddenTracks({});
	fixture.step();
	Rml::ElementList shown = labels();
	REQUIRE(shown.size() == 4);
	for (const Rml::Element* label : shown)
		CHECK_FALSE(label->IsClassSet("hidden"));
	CHECK(top(shown[0]) == Approx(40.0).margin(0.5));
	CHECK(top(shown[1]) == Approx(40.0).margin(0.5));
	CHECK(top(shown[2]) == Approx(80.0).margin(0.5));
	CHECK(top(shown[3]) == Approx(80.0).margin(0.5));
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

	// Lets the UI take a few steps, so that what it would do by now is done.
	void settle(test::UiApp& app)
	{
		for (int i = 0; i < 5; ++i)
			app.shell().step(milliseconds(10));
	}

	Json setting(test::UiApp& app, const std::string& key)
	{
		return app.call("settings.get", {{"key", key}}).at("value");
	}

	// Opens the markers window with Mod+M and waits for its rows.
	ui::MarkersWindow& openMarkers(test::UiApp& app, size_t rows)
	{
		app.ui().key(std::nullopt, "Mod+M");
		REQUIRE(app.waitUntil([&app, rows]
			{ return app.appUi().markers() != nullptr && app.appUi().markers()->markers().size() == rows; }));
		return *app.appUi().markers();
	}

	Rml::ElementList markerRows(Rml::ElementDocument& document)
	{
		Rml::ElementList rows;
		document.QuerySelectorAll(rows, "#marker-rows tr");
		return rows;
	}

	// The rows' markers, in the order the window lists them.
	std::vector<uint64_t> rowOrder(Rml::ElementDocument& document)
	{
		std::vector<uint64_t> ids;
		for (const Rml::Element* row : markerRows(document))
			ids.push_back(row->GetAttribute<uint64_t>("value", 0));
		return ids;
	}

	// The columns of the header after the locate column, in order.
	std::vector<std::string> headerOrder(Rml::ElementDocument& document)
	{
		Rml::ElementList cells;
		document.QuerySelectorAll(cells, "#marker-head td");
		std::vector<std::string> columns;
		for (const Rml::Element* cell : cells)
			if (!cell->IsClassSet("locate"))
				columns.push_back(cell->GetAttribute<Rml::String>("column", ""));
		return columns;
	}

	// The text field's value in the column of a row, or nothing if the cell has none.
	std::optional<std::string> field(Rml::ElementDocument& document, size_t row, const std::string& column)
	{
		const Rml::ElementList rows = markerRows(document);
		if (row >= rows.size())
			return std::nullopt;
		auto* input = dynamic_cast<Rml::ElementFormControl*>(rows[row]->QuerySelector("." + column + " input"));
		if (input == nullptr)
			return std::nullopt;
		return input->GetValue();
	}

	// Clicks the text field in the column of the index-th row that has one, types the text over its value and
	// presses Enter.
	void edit(test::UiApp& app, int index, const std::string& column, const std::string& text)
	{
		const ui::WindowId window = app.appUi().markersWindow();
		app.ui().click(window, at("#marker-rows ." + column + " input", index), ui::MouseButton::Left, 1, {});
		app.ui().key(window, "Ctrl+A");
		app.ui().text(window, text);
		app.ui().key(window, "Enter");
	}

	// A point x pixels into the element, halfway down.
	ui::Target pointIn(Rml::Element& element, float x)
	{
		return point(element.GetAbsoluteLeft() + x, element.GetAbsoluteTop() + (element.GetOffsetHeight() / 2.0F));
	}

	int64_t markerField(test::UiApp& app, size_t index, const std::string& name)
	{
		return markerList(app)[index].at(name).get<int64_t>();
	}

	uint64_t addMarker(test::UiApp& app, const Json& parameters)
	{
		return app.call("marker.add", parameters).at("marker").at("id").get<uint64_t>();
	}

	TEST_CASE("the markers window lists the active marker track's markers to locate, edit, add and remove", "[ui]")
	{
		test::UiApp app;
		const auto track = app.call("track.add", {{"type", "marker"}}).at("track").at("id").get<uint64_t>();
		app.call("marker.add", {{"position", 2 * g_quarter}, {"name", "Intro"}});
		app.call(
			"marker.add", {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});

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
		CHECK(field(document, 1, "position") == ui::formatTime(ui::TimeFormat::BarsBeats, 2.0, context));
		CHECK(field(document, 1, "end") == ui::formatTime(ui::TimeFormat::BarsBeats, 4.0, context));
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
		app.ui().key(window, "Ctrl+A");
		app.ui().text(window, "Start");
		app.ui().key(window, "Enter");
		REQUIRE(app.waitUntil([&markers] { return markers.markers()[0].name == "Start"; })); // the rows again
		app.ui().click(window, at("#marker-rows .number input", 1), ui::MouseButton::Left, 1, {});
		app.ui().key(window, "Ctrl+A");
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

	TEST_CASE("the markers window's lengths don't count from the project's start time", "[ui]")
	{
		test::UiApp app;
		app.call("track.add", {{"type", "marker"}});
		app.call("project.set", {{"startTime", 3600.0}});
		app.call("marker.add", {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}}); // 2 seconds
		app.call("settings.set", {{"key", "ui.timeFormats"}, {"value", {{"primary", "seconds"}}}});

		openMarkers(app, 1);
		Rml::ElementDocument& document = markersDocument(app);
		REQUIRE(app.waitUntil([&document] { return field(document, 0, "length") == std::string("0:00:02.000"); }));
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

}

TEST_CASE("the markers window edits positions, ends and lengths in the time format", "[ui]")
{
	test::UiApp app;
	app.call("track.add", {{"type", "marker"}});
	app.call("marker.add", {{"position", 2 * g_quarter}});
	app.call("marker.add", {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 6 * g_quarter}});
	const ui::MarkersWindow& markers = openMarkers(app, 2);
	Rml::ElementDocument& document = markersDocument(app);
	const ui::TimeContext context;

	// Positions, ends and lengths in the transport's primary time format, bars and beats by default; a position
	// marker has no end or length.
	CHECK(field(document, 0, "position") == ui::formatTime(ui::TimeFormat::BarsBeats, 1.0, context));
	CHECK(field(document, 0, "end") == std::nullopt);
	CHECK(field(document, 0, "length") == std::nullopt);
	CHECK(field(document, 1, "end") == ui::formatTime(ui::TimeFormat::BarsBeats, 3.0, context));
	CHECK(field(document, 1, "length") == "0.2.0.0");

	// The end, the length from the position, and the position, which moves a cycle marker as a whole.
	edit(app, 0, "end", "3");
	REQUIRE(app.waitUntil([&app] { return markerField(app, 1, "end") == 8 * g_quarter; }));
	edit(app, 0, "length", "0.3.0.0");
	REQUIRE(app.waitUntil([&app] { return markerField(app, 1, "end") == 7 * g_quarter; }));
	edit(app, 1, "position", "5");
	REQUIRE(app.waitUntil([&app] { return markerField(app, 1, "position") == 16 * g_quarter; }));
	CHECK(markerField(app, 1, "end") == 19 * g_quarter);
	edit(app, 0, "position", "2");
	REQUIRE(app.waitUntil([&app] { return markerField(app, 0, "position") == 4 * g_quarter; }));

	// What isn't a time, or an end before the position, shows the marker as it is again.
	edit(app, 0, "position", "later");
	REQUIRE(app.waitUntil([&document, &context]
		{ return field(document, 0, "position") == ui::formatTime(ui::TimeFormat::BarsBeats, 2.0, context); }));
	edit(app, 0, "end", "1");
	REQUIRE(app.waitUntil([&document, &context]
		{ return field(document, 1, "end") == ui::formatTime(ui::TimeFormat::BarsBeats, 9.5, context); }));
	CHECK(markerField(app, 1, "end") == 19 * g_quarter);
	CHECK(markers.markers().size() == 2);

	// The transport's primary time format changes them all.
	app.call("settings.set", {{"key", "ui.timeFormats"}, {"value", {{"primary", "seconds"}}}});
	REQUIRE(app.waitUntil([&document, &context]
		{ return field(document, 0, "position") == ui::formatTime(ui::TimeFormat::Seconds, 2.0, context); }));
	CHECK(field(document, 1, "length") == ui::formatLength(ui::TimeFormat::Seconds, 8.0, 9.5, context));
}

TEST_CASE("the markers window sorts by a click on a column header, and its columns move and resize", "[ui]")
{
	test::UiApp app;
	app.call("track.add", {{"type", "marker"}});
	const uint64_t first = addMarker(app, {{"position", 2 * g_quarter}, {"name", "B"}});
	const uint64_t cycle =
		addMarker(app, {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 6 * g_quarter}, {"name", "A"}});
	const uint64_t second = addMarker(app, {{"position", 8 * g_quarter}, {"name", "a"}});
	openMarkers(app, 3);
	const ui::WindowId window = app.appUi().markersWindow();
	Rml::ElementDocument& document = markersDocument(app);
	const auto sorted = [&document](const char* column)
	{
		const Rml::Element* cell = document.QuerySelector(std::string("#marker-head td.") + column);
		return cell != nullptr && cell->IsClassSet("sorted");
	};
	CHECK(rowOrder(document) == std::vector<uint64_t>{first, cycle, second});
	CHECK(headerOrder(document) ==
		std::vector<std::string>{"number", "type", "position", "end", "length", "description"});
	CHECK(sorted("position"));

	// A click on a header sorts by its column, the position second; another click turns the order around, the
	// position still second.
	app.ui().click(window, at("#marker-head td.description"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&document, first, cycle, second]
		{ return rowOrder(document) == std::vector<uint64_t>{cycle, second, first}; }));
	CHECK(sorted("description"));
	CHECK(!sorted("position"));
	app.ui().click(window, at("#marker-head td.description"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&document, first, cycle, second]
		{ return rowOrder(document) == std::vector<uint64_t>{first, cycle, second}; }));
	CHECK(document.QuerySelector("#marker-head td.description")->IsClassSet("descending"));
	app.ui().click(window, at("#marker-head td.type"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&document, first, cycle, second]
		{ return rowOrder(document) == std::vector<uint64_t>{first, second, cycle}; }));
	REQUIRE(app.waitUntil([&app] { return setting(app, "ui.markerColumns").value("sort", "") == "type"; }));
	CHECK(setting(app, "ui.markerColumns").at("descending") == false);

	// Dragging a header moves its column, in the header and the rows; the locate column stays first. (The window is
	// hidden, so its layout waits for the next input or an update.)
	document.UpdateDocument();
	Rml::Element* number = document.QuerySelector("#marker-head td.number");
	REQUIRE(number != nullptr);
	app.ui().drag(window, at("#marker-head td.description"), pointIn(*number, 2.0F));
	REQUIRE(app.waitUntil(
		[&document]
		{
			return headerOrder(document) ==
				std::vector<std::string>{"description", "number", "type", "position", "end", "length"};
		}));
	const Rml::Element* row = markerRows(document)[0];
	CHECK(row->GetChild(0)->IsClassSet("locate"));
	CHECK(row->GetChild(1)->IsClassSet("description"));
	CHECK(row->GetChild(2)->IsClassSet("number"));
	REQUIRE(app.waitUntil(
		[&app] { return setting(app, "ui.markerColumns").value("order", Json::array()).front() == "description"; }));
	CHECK(setting(app, "ui.markerColumns").at("order").size() == 6);
	CHECK(rowOrder(document) == std::vector<uint64_t>{first, second, cycle}); // a drag doesn't sort

	// Dragging the line between two headers resizes the column on its left, in the rows too.
	document.UpdateDocument();
	Rml::Element* type = document.QuerySelector("#marker-head td.type");
	REQUIRE(type != nullptr);
	const float width = type->GetOffsetWidth();
	app.ui().drag(window, pointIn(*type, width - 1.0F), pointIn(*type, width + 39.0F));
	REQUIRE(app.waitUntil(
		[&document, width]
		{
			document.UpdateDocument(); // a hidden window lays out only when it draws
			return document.QuerySelector("#marker-head td.type")->GetOffsetWidth() ==
				Approx(width + 40.0F).margin(2.0);
		}));
	CHECK(markerRows(document)[0]->QuerySelector(".type")->GetOffsetWidth() == Approx(width + 40.0F).margin(2.0));
	REQUIRE(app.waitUntil(
		[&app] { return setting(app, "ui.markerColumns").value("widths", Json::object()).contains("type"); }));
	CHECK(rowOrder(document) == std::vector<uint64_t>{first, second, cycle});

	// The setting from MCP or Lua applies at once; a column named twice counts once, those it leaves out follow in
	// their usual order.
	app.call("settings.set", {{"key", "ui.markerColumns"}, {"value", {{"order", {"position", "number", "position"}}}}});
	REQUIRE(app.waitUntil(
		[&document]
		{
			return headerOrder(document) ==
				std::vector<std::string>{"position", "number", "type", "end", "length", "description"};
		}));
	CHECK(rowOrder(document) == std::vector<uint64_t>{first, cycle, second});

	// Opened again, the window shows the columns as they were.
	app.ui().key(window, "Mod+M");
	REQUIRE(app.waitUntil([&app] { return app.appUi().markers() == nullptr; }));
	openMarkers(app, 3);
	Rml::ElementDocument& reopened = markersDocument(app);
	REQUIRE(app.waitUntil([&reopened] { return headerOrder(reopened).front() == "position"; }));
}

TEST_CASE("the markers window scrolls to the locate arrow, and Page Up and Page Down move through it", "[ui]")
{
	test::UiApp app;
	app.call("track.add", {{"type", "marker"}});
	for (int i = 1; i <= 60; ++i)
		app.call("marker.add", {{"position", i * g_quarter}});
	ui::MarkersWindow& markers = openMarkers(app, 60);
	const ui::WindowId window = app.appUi().markersWindow();
	Rml::ElementDocument& document = markersDocument(app);
	Rml::Element* list = document.GetElementById("marker-list");
	REQUIRE(list != nullptr);
	const auto inView = [list](Rml::Element* row)
	{
		return row != nullptr && row->GetAbsoluteTop() >= list->GetAbsoluteTop() - 1.0F &&
			row->GetAbsoluteTop() + row->GetOffsetHeight() <= list->GetAbsoluteTop() + list->GetClientHeight() + 1.0F;
	};
	const auto atCursor = [&document, &markers](size_t index)
	{
		const Rml::Element* row = document.QuerySelector("#marker-rows tr.at-cursor");
		return row != nullptr && row->GetAttribute<uint64_t>("value", 0) == markers.markers()[index].id;
	};
	CHECK(list->GetScrollTop() == 0.0F);
	CHECK(setting(app, "ui.markersAutoScroll") == true);
	REQUIRE(
		app.waitUntil([&document] { return document.GetElementById("marker-auto-scroll")->HasAttribute("checked"); }));

	// Auto-Scroll with Project Cursor, on by default, keeps the row with the arrow in view.
	app.call("marker.locate", {{"number", 60}});
	REQUIRE(app.waitUntil([&atCursor] { return atCursor(59); }));
	REQUIRE(
		app.waitUntil([&document, &inView] { return inView(document.QuerySelector("#marker-rows tr.at-cursor")); }));
	CHECK(list->GetScrollTop() > 0.0F);

	// Turned off, the list stays where it is.
	app.ui().click(window, at("#marker-auto-scroll"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return setting(app, "ui.markersAutoScroll") == false; }));
	const float scrolled = list->GetScrollTop();
	app.call("marker.locate", {{"number", 1}});
	REQUIRE(app.waitUntil([&atCursor] { return atCursor(0); }));
	settle(app);
	CHECK(list->GetScrollTop() == scrolled);

	// Page Down and Page Up move the selection by the rows the list shows; Home and End go to the first and last.
	const auto selected = [&markers]
	{
		const std::vector<uint64_t> ids = markers.selection();
		const std::vector<ui::MarkerItem>& all = markers.markers();
		for (size_t i = 0; ids.size() == 1 && i < all.size(); ++i)
			if (all[i].id == ids.front())
				return static_cast<int>(i);
		return -1;
	};
	app.ui().click(window, at("#marker-rows .type", 59), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&selected] { return selected() == 59; }));
	app.ui().key(window, "Home");
	REQUIRE(app.waitUntil([&selected] { return selected() == 0; }));
	app.ui().key(window, "PageDown");
	REQUIRE(app.waitUntil([&selected] { return selected() > 1; }));
	const int page = selected();
	CHECK(page < 30);
	CHECK(inView(markerRows(document)[static_cast<size_t>(page)]));
	app.ui().key(window, "PageDown");
	REQUIRE(app.waitUntil([&selected, page] { return selected() == 2 * page; }));
	CHECK(inView(markerRows(document)[static_cast<size_t>(2 * page)]));
	app.ui().key(window, "PageUp");
	REQUIRE(app.waitUntil([&selected, page] { return selected() == page; }));
	app.ui().key(window, "End");
	app.ui().key(window, "PageDown");
	REQUIRE(app.waitUntil([&selected] { return selected() == 59; }));
	app.ui().key(window, "PageUp");
	REQUIRE(app.waitUntil([&selected, page] { return selected() == 59 - page; }));
}

TEST_CASE("the markers window changes the description or length of all selected markers at once", "[ui]")
{
	test::UiApp app;
	app.call("track.add", {{"type", "marker"}});
	app.call("marker.add", {{"type", "cycle"}, {"position", 0}, {"end", 2 * g_quarter}, {"name", "One"}});
	app.call("marker.add", {{"position", 4 * g_quarter}, {"name", "Two"}});
	app.call("marker.add", {{"type", "cycle"}, {"position", 8 * g_quarter}, {"end", 9 * g_quarter}, {"name", "Three"}});
	app.call("marker.add", {{"position", 12 * g_quarter}, {"name", "Four"}});
	ui::MarkersWindow& markers = openMarkers(app, 4);
	const ui::WindowId window = app.appUi().markersWindow();
	const size_t steps = app.call("history.get").at("steps").size();

	app.ui().click(window, at("#marker-rows .type", 0), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#marker-rows .type", 2), ui::MouseButton::Left, 1, {"shift"});
	REQUIRE(app.waitUntil([&markers] { return markers.selection().size() == 3; }));

	// A field that loses the focus without an edit changes nothing.
	app.ui().click(window, at("#marker-rows .description input", 0), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#marker-rows .type", 2), ui::MouseButton::Left, 1, {"shift"});
	settle(app);
	CHECK(markerList(app)[1].at("name") == "Two");
	CHECK(markerList(app)[2].at("name") == "Three");
	CHECK(app.call("history.get").at("steps").size() == steps);
	REQUIRE(markers.selection().size() == 3);

	// A description typed into a selected row names them all, in one undo step; the row not selected keeps its own.
	edit(app, 1, "description", "Verse");
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json list = markerList(app);
			return list[0].at("name") == "Verse" && list[1].at("name") == "Verse" && list[2].at("name") == "Verse";
		}));
	CHECK(markerList(app)[3].at("name") == "Four");
	CHECK(app.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(markers.selection().size() == 3);

	// A length gives every selected cycle marker that length; position markers have none.
	edit(app, 0, "length", "1.0.0.0");
	REQUIRE(app.waitUntil([&app] { return markerField(app, 2, "end") == 12 * g_quarter; }));
	CHECK(markerField(app, 0, "end") == 4 * g_quarter);
	CHECK(markerList(app)[1].at("type") == "position");

	// Undo takes back the step as a whole.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return markerField(app, 2, "end") == 9 * g_quarter; }));
	CHECK(markerField(app, 0, "end") == 2 * g_quarter);
	CHECK(markerList(app)[1].at("name") == "Verse");

	// With one row selected, only its marker changes.
	app.ui().click(window, at("#marker-rows .type", 3), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return markers.selection().size() == 1; }));
	edit(app, 1, "description", "Coda");
	REQUIRE(app.waitUntil([&app] { return markerList(app)[1].at("name") == "Coda"; }));
	CHECK(markerList(app)[0].at("name") == "Verse");

	// A change from elsewhere rebuilds the rows under a focused field, which doesn't write its old text back.
	app.ui().click(window, at("#marker-rows .description input", 3), ui::MouseButton::Left, 1, {});
	app.call("marker.set", {{"marker", markerList(app)[3].at("id")}, {"name", "Outro"}});
	REQUIRE(app.waitUntil([&markers] { return markers.markers()[3].name == "Outro"; }));
	settle(app);
	CHECK(markerList(app)[3].at("name") == "Outro");
}

TEST_CASE("the marker settings: marker ids on the marker track, cycle follows and sync selection", "[ui]")
{
	test::UiApp app;
	app.appUi().showProject();
	app.call("track.add", {{"type", "marker"}});
	// Close to the start, so that their labels show in the project window.
	const uint64_t intro = addMarker(app, {{"position", g_quarter}, {"name", "Intro"}});
	const uint64_t verse =
		addMarker(app, {{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});
	ui::MarkersWindow& markers = openMarkers(app, 2);
	const ui::WindowId window = app.appUi().markersWindow();
	Rml::ElementDocument& document = markersDocument(app);
	REQUIRE(app.appUi().markerLines() != nullptr);
	const ui::MarkerLines& lines = *app.appUi().markerLines();
	const auto label = [&app](size_t index) -> Rml::Element*
	{
		Rml::Context& context = app.main().context();
		Rml::ElementList labels;
		for (int i = 0; i < context.GetNumDocuments() && labels.empty(); ++i)
			context.GetDocument(i)->QuerySelectorAll(labels, ".marker-label");
		return index < labels.size() ? labels[index] : nullptr;
	};
	const auto text = [&label](size_t index)
	{
		const Rml::Element* found = label(index);
		return found == nullptr ? std::string() : found->GetInnerRML();
	};
	const auto checked = [&document](const char* id) { return document.GetElementById(id)->HasAttribute("checked"); };
	REQUIRE(app.waitUntil([&text] { return text(1) == "[1] Verse"; }));
	REQUIRE(app.waitUntil([&checked] { return checked("show-marker-ids") && checked("sync-selection"); }));
	CHECK(!checked("cycle-follows"));

	// Show Marker IDs on Marker Track, on by default.
	app.ui().click(window, at("#marker-settings"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#show-marker-ids"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return setting(app, "ui.markerIds") == false; }));
	REQUIRE(app.waitUntil([&text] { return text(1) == "Verse"; }));
	CHECK(text(0) == "Intro");

	// Cycle Follows When Locating to Markers: locating from the list sets the locators to the cycle marker.
	app.ui().click(window, at("#marker-settings"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#cycle-follows"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return setting(app, "transport.cycleFollowsMarkers") == true; }));
	REQUIRE(app.waitUntil([&checked] { return checked("cycle-follows"); }));
	app.ui().click(window, at("#marker-rows .locate", 1), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("left") == 4 * g_quarterSamples; }));
	CHECK(app.call("transport.get").at("right") == 8 * g_quarterSamples);

	// Sync Selection, on by default: the markers window and the project window select the same markers.
	app.ui().click(window, at("#marker-rows .type", 1), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&lines, verse] { return lines.selection() == std::vector<uint64_t>{verse}; }));
	REQUIRE(app.waitUntil([&label] { return label(1) != nullptr && label(1)->IsClassSet("selected"); }));
	CHECK(!label(0)->IsClassSet("selected"));
	app.ui().click(std::nullopt, at(".marker-label", 0), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers, intro] { return markers.selection() == std::vector<uint64_t>{intro}; }));
	CHECK(lines.selection() == std::vector<uint64_t>{intro});
	app.ui().click(std::nullopt, at(".marker-label", 1), ui::MouseButton::Left, 1, {"ctrl"});
	REQUIRE(app.waitUntil([&markers] { return markers.selection().size() == 2; }));
	CHECK(lines.selection().size() == 2);

	// It pauses while recording.
	app.call("transport.record");
	REQUIRE(app.waitUntil([&markers] { return !markers.syncsSelection(); }));
	app.ui().click(window, at("#marker-rows .type", 0), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers, intro] { return markers.selection() == std::vector<uint64_t>{intro}; }));
	settle(app);
	CHECK(lines.selection().size() == 2);
	app.call("transport.stop");
	REQUIRE(app.waitUntil([&markers] { return markers.syncsSelection(); }));
	// After recording the window takes the project window's selection.
	REQUIRE(app.waitUntil([&markers] { return markers.selection().size() == 2; }));
	app.ui().click(window, at("#marker-rows .type", 1), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&lines, verse] { return lines.selection() == std::vector<uint64_t>{verse}; }));

	// Turned off, each window keeps its own selection.
	app.ui().click(window, at("#marker-settings"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#sync-selection"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers] { return !markers.syncsSelection(); }));
	CHECK(setting(app, "ui.markersSyncSelection") == false);
	app.ui().click(window, at("#marker-rows .type", 0), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers, intro] { return markers.selection() == std::vector<uint64_t>{intro}; }));
	settle(app);
	CHECK(lines.selection() == std::vector<uint64_t>{verse});

	// Turned on again, and when the window opens, the window takes the project window's selection.
	app.ui().click(window, at("#marker-settings"), ui::MouseButton::Left, 1, {});
	app.ui().click(window, at("#sync-selection"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&markers, verse] { return markers.selection() == std::vector<uint64_t>{verse}; }));
	app.ui().key(std::nullopt, "Mod+M");
	REQUIRE(app.waitUntil([&app] { return app.appUi().markers() == nullptr; }));
	app.ui().click(std::nullopt, at(".marker-label", 0), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&lines, intro] { return lines.selection() == std::vector<uint64_t>{intro}; }));
	const ui::MarkersWindow& reopened = openMarkers(app, 2);
	REQUIRE(app.waitUntil([&reopened, intro] { return reopened.selection() == std::vector<uint64_t>{intro}; }));
}

TEST_CASE("File > Export and File > Import > Markers write and read the marker file chosen", "[ui]")
{
	test::UiApp app{{1000, 700}};
	const test::TempDir dir;
	app.appUi().showProject();
	for (const char* command : {"file.exportMarkers", "file.importMarkers"})
	{
		CHECK(app.ui()
				  .dom(std::nullopt, std::string("#file-menu menuitem[command=\"") + command + "\"]", 0)
				  .at("elements")
				  .size() == 1);
		CHECK(app.keys().canRun(command, app.main(), nullptr));
	}
	app.call("marker.add", {{"position", 2 * g_quarter}, {"name", "Intro"}});

	// The file chosen gets the extension .csv if it has none.
	app.keys().run("file.exportMarkers", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
	CHECK(app.shell().fileDialogs().begin()->second.kind == ui::FileDialogKind::Save);
	CHECK(app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {dir.path() / "song"}));
	app.shell().step(milliseconds(0));
	app.call("project.revision"); // after the export, which the command thread runs first
	REQUIRE(std::filesystem::exists(dir.path() / "song.csv"));

	app.call("project.new");
	app.keys().run("file.importMarkers", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
	CHECK(app.shell().fileDialogs().begin()->second.kind == ui::FileDialogKind::Open);
	CHECK(app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {dir.path() / "song.csv"}));
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("markers").size() == 1; }));
	const Json marker = app.call("project.get").at("markers")[0];
	CHECK(marker.at("name") == "Intro");
	CHECK(marker.at("position") == 2 * g_quarter);
}
