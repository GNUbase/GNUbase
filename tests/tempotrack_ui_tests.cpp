// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo track in the UI (#155): the tempo curve on the tempo track's lane in the event display on the fake window
// system, following a real engine, and the tempo track editor, Activate Tempo Track and Tap Tempo in the app's UI.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/rmlui.h"
#include "ui/shell.h"
#include "ui/tempotrack.h"
#include "ui/transport.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

	ui::Target point(double x, double y)
	{
		return {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	}

	// The centre of an element.
	ui::Target centre(Rml::Element& element)
	{
		return point(element.GetAbsoluteLeft() + (element.GetOffsetWidth() / 2.0F),
			element.GetAbsoluteTop() + (element.GetOffsetHeight() / 2.0F));
	}

	// The event display with its tempo lane, filling a 1000 by 400 window of the fake UI at a monitor scale of 1, so
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

		Json tempoTrack() { return call("project.get").at("tempoTrack"); }

		Rml::ElementList points() const
		{
			Rml::ElementList found;
			document.QuerySelectorAll(found, ".tempo-curve .tempo-point");
			return found;
		}

		Rml::Element& curve() const
		{
			Rml::Element* found = document.QuerySelector(".tempo-curve");
			REQUIRE(found != nullptr);
			return *found;
		}

		// A point on the tempo curve, at a time in seconds and y dp from its top.
		ui::Target at(double seconds, double y) const
		{
			const ui::TimelineView& view = display.view();
			Rml::Element& element = curve();
			return point(
				element.GetAbsoluteLeft() + ((seconds - view.start) / view.scale), element.GetAbsoluteTop() + y);
		}

		void click(const ui::Target& target)
		{
			automation.click(std::nullopt, target, ui::MouseButton::Left, 1, {});
			step();
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::TempoLane lane{display, binding};
		ui::Automation automation{ui.shell};
	};

	// The project window with its transport bar shown, at a dp ratio at which all its sections fit 1024 pixels.
	void showBar(test::UiApp& app)
	{
		app.setDpRatio(0.8F);
		app.appUi().showProject();
		app.appUi().docking().setVisible("project", "transportBar", true);
		app.shell().step(milliseconds(0));
	}

	Rml::ElementDocument& editorDocument(test::UiApp& app)
	{
		ui::Window* window = app.shell().find(app.appUi().tempoWindow());
		REQUIRE(window != nullptr);
		Rml::ElementDocument* document = window->context().GetDocument(0);
		REQUIRE(document != nullptr);
		return *document;
	}
}

TEST_CASE("the visible tempos reach from below the slowest tempo event to above the fastest", "[unit]")
{
	ui::ProjectTempo tempo;
	CHECK(ui::visibleTempo(tempo) == std::pair<double, double>{100.0, 140.0}); // fixed at 120 BPM
	tempo.active = true;
	tempo.tempoTrack = time::TempoMap({{0, 95.0, false}, {g_quarter, 181.0, true}});
	CHECK(ui::visibleTempo(tempo) == std::pair<double, double>{70.0, 210.0});
}

TEST_CASE("taps in time give the tempo of the time between them", "[unit]")
{
	CHECK(!ui::tappedTempo({1.0}));
	CHECK(ui::tappedTempo({1.0, 1.5, 2.0, 2.5}).value_or(0.0) == Approx(120.0));
	CHECK(ui::tappedTempo({0.0, 0.6, 1.2}).value_or(0.0) == Approx(100.0));
	CHECK(ui::tappedTempo({0.0, 0.001}).value_or(0.0) == Approx(999.0)); // as fast as a tempo goes
}

TEST_CASE("the tempo curve shows the tempo track's events on its lane, where clicks add, move and erase them", "[unit]")
{
	Fixture fixture;
	fixture.call("track.add", {{"type", "audio"}});
	fixture.call("track.add", {{"type", "tempo"}});
	fixture.call("project.set", {{"tempoTrackActive", true}});
	fixture.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.points().size() == 2; }));

	// Over the tempo track's lane, the second one, at the events' times and tempos.
	Rml::Element& curve = fixture.curve();
	CHECK(!curve.IsClassSet("hidden"));
	CHECK(!curve.IsClassSet("inactive"));
	const std::vector<double>& tops = fixture.display.laneTops();
	CHECK(curve.GetAbsoluteTop() - fixture.document.QuerySelector("event-lanes.events")->GetAbsoluteTop() ==
		Approx(tops[1]));
	CHECK(curve.GetOffsetHeight() == Approx(tops[2] - tops[1]));
	const ui::TimelineView& view = fixture.display.view();
	Rml::Element& second = *fixture.points()[1];
	CHECK(second.GetAbsoluteLeft() + (second.GetOffsetWidth() / 2.0F) - curve.GetAbsoluteLeft() ==
		Approx((2.0 - view.start) / view.scale).margin(1.0));
	// 100 BPM in a curve from 80 to 140 BPM.
	CHECK(second.GetAbsoluteTop() + (second.GetOffsetHeight() / 2.0F) - curve.GetAbsoluteTop() ==
		Approx(curve.GetOffsetHeight() * (140.0 - 100.0) / 60.0).margin(1.0));

	// A click elsewhere adds an event there, at the tempo the height says.
	fixture.click(fixture.at(3.0, curve.GetOffsetHeight() / 2.0));
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.tempoTrack().size() == 3; }));
	const Json added = fixture.tempoTrack()[2];
	// Two seconds at 120 BPM, then one at 100 BPM: five thirds of a quarter.
	CHECK(added.at("position").get<int64_t>() == Approx((4 * g_quarter) + (5 * g_quarter / 3)).margin(g_quarter / 10));
	CHECK(added.at("bpm").get<double>() == Approx(110.0).margin(2.0));
	CHECK(fixture.call("project.get").at("tracks").size() == 2); // and nothing else

	// A drag moves the event: later, and slower.
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.points().size() == 3; }));
	fixture.automation.drag(std::nullopt, centre(*fixture.points()[1]), fixture.at(2.5, curve.GetOffsetHeight()));
	fixture.step();
	REQUIRE(fixture.waitUntil(
		[&fixture] { return std::abs(fixture.tempoTrack()[1].at("bpm").get<double>() - 80.0) <= 2.0; }));
	CHECK(fixture.tempoTrack()[1].at("position").get<int64_t>() ==
		Approx((4 * g_quarter) + (5 * g_quarter / 6)).margin(g_quarter / 10));
	CHECK(fixture.lane.curve().selection().size() == 1);

	// The erase tool removes events, but not the first.
	fixture.display.setTool(ui::Tool::Erase);
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.points().size() == 3; }));
	fixture.click(centre(*fixture.points()[0]));
	fixture.click(centre(*fixture.points()[2]));
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.tempoTrack().size() == 2; }));
	CHECK(fixture.tempoTrack()[0].at("position") == 0);

	// In fixed tempo mode the curve is inactive; without a tempo track it is hidden.
	fixture.call("project.set", {{"tempoTrackActive", false}});
	REQUIRE(fixture.waitUntil([&curve] { return curve.IsClassSet("inactive"); }));
	const uint64_t tempoTrack = fixture.call("project.get").at("tracks")[1].at("id").get<uint64_t>();
	fixture.call("track.remove", {{"track", tempoTrack}});
	REQUIRE(fixture.waitUntil([&curve] { return curve.IsClassSet("hidden"); }));
}

TEST_CASE("the tempo track editor switches the tempo mode and edits the tempo events", "[ui]")
{
	test::UiApp app{{1000, 600}};
	app.appUi().showProject();
	app.call("tempo.add", {{"position", 8 * g_quarter}, {"bpm", 90.0}, {"ramp", true}});

	// Mod+T opens it in a window of its own.
	app.ui().key(std::nullopt, "Mod+T");
	REQUIRE(app.waitUntil([&app] { return app.appUi().tempoEditor() != nullptr; }));
	const ui::WindowId window = app.appUi().tempoWindow();
	Rml::ElementDocument& document = editorDocument(app);
	REQUIRE(app.waitUntil(
		[&document]
		{
			Rml::ElementList points;
			document.QuerySelectorAll(points, ".tempo-point");
			return points.size() == 2;
		}));
	Rml::Element* curve = document.QuerySelector(".tempo-curve");
	REQUIRE(curve != nullptr);
	CHECK(curve->IsClassSet("inactive"));
	auto* current = dynamic_cast<Rml::ElementFormControl*>(document.GetElementById("current-tempo"));
	REQUIRE(current != nullptr);
	CHECK(current->GetValue() == "120.000");

	// Activate Tempo Track.
	app.ui().click(window, {"#tempo-track-active", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempoTrackActive") == true; }));
	REQUIRE(app.waitUntil([curve] { return !curve->IsClassSet("inactive"); }));
	CHECK(document.GetElementById("tempo-track-active")->IsClassSet("checked"));

	// The current tempo in tempo track mode is the tempo at the cursor, and changes the event before it.
	app.ui().click(window, {"#current-tempo", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.ui().key(window, "Ctrl+A");
	app.ui().text(window, "130");
	app.ui().key(window, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[0].at("bpm") == 130.0; }));

	// A click on an event selects it and shows it on the info line, where its tempo and type change.
	Rml::ElementList points;
	document.QuerySelectorAll(points, ".tempo-point");
	REQUIRE(points.size() == 2);
	app.ui().click(window, centre(*points[1]), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&document] { return !document.GetElementById("tempo-info")->IsClassSet("hidden"); }));
	CHECK(document.GetElementById("tempo-position")->GetInnerRML() == "3.1.1.0");
	app.ui().click(window, {"#tempo-value", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.ui().key(window, "Ctrl+A");
	app.ui().text(window, "95.5");
	app.ui().key(window, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[1].at("bpm") == 95.5; }));
	auto* type = dynamic_cast<Rml::ElementFormControlSelect*>(document.GetElementById("tempo-ramp"));
	REQUIRE(type != nullptr);
	CHECK(type->GetValue() == "ramp");
	type->SetValue("jump");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[1].at("ramp") == false; }));

	// The type of new events is the user setting.
	auto* newType = dynamic_cast<Rml::ElementFormControlSelect*>(document.GetElementById("tempo-type"));
	REQUIRE(newType != nullptr);
	newType->SetValue("ramp");
	REQUIRE(app.waitUntil(
		[&app] { return app.call("settings.get", {{"key", "editing.tempoType"}}).at("value") == "ramp"; }));

	// Backspace removes the selected events, outside a text field.
	document.GetElementById("tempo-value")->Blur();
	app.ui().key(window, "Backspace");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo").size() == 1; }));

	// The draw tool adds an event where it clicks.
	app.ui().click(window, {"#tempo-tool-draw", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	CHECK(app.appUi().tempoEditor()->tool() == ui::TempoTool::Draw);
	app.ui().click(window, {"#tempo-curve-area", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo").size() == 2; }));
	CHECK(app.call("project.get").at("tempo")[1].at("ramp") == true);

	// Mod+T closes it.
	app.ui().key(window, "Mod+T");
	REQUIRE(app.waitUntil([&app] { return app.appUi().tempoEditor() == nullptr; }));
}

TEST_CASE("the tempo track's Activate Tempo Track and the transport bar's switch the tempo mode", "[ui]")
{
	test::UiApp app{{1024, 500}};
	showBar(app);
	const auto tempo = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	const std::string button = "#track-list listitem[value=\"" + std::to_string(tempo) + "\"] button.tempoTrack";
	REQUIRE(app.waitUntil([&app, &button] { return !app.ui().dom(std::nullopt, button, 0).at("elements").empty(); }));
	CHECK(app.ui().dom(std::nullopt, "#track-list listitem button[control]", 0).at("elements").size() == 1);

	app.ui().click(std::nullopt, {button, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempoTrackActive") == true; }));
	const auto checked = [&app](const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		const Json classes = found.empty() ? Json::array() : found[0].value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), "checked") != classes.end();
	};
	REQUIRE(app.waitUntil([&] { return checked(button) && checked("#transport-bar #tempo-track"); }));

	app.ui().click(std::nullopt, {"#transport-bar #tempo-track", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempoTrackActive") == false; }));
	REQUIRE(app.waitUntil([&] { return !checked(button) && !checked("#transport-bar #tempo-track"); }));
}

TEST_CASE("Tap Tempo sets the tempo the taps beat once they stop", "[ui]")
{
	test::UiApp app{{1024, 500}};
	showBar(app);
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true); // RmlUi's time stands still but for advance()
	for (int tap = 0; tap < 4; ++tap)
	{
		if (tap > 0)
			clock->advance(0.6);
		app.ui().click(std::nullopt, {"#transport-bar #tap-tempo", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(milliseconds(0));
	}
	// Meanwhile the tempo field shows the tapped tempo, and the project's is unchanged.
	CHECK(
		app.ui().dom(std::nullopt, "#transport-bar #tempo", 0).at("elements")[0].at("attributes").at("value") == "100");
	CHECK(app.call("project.get").at("fixedTempo") == 120.0);
	clock->advance(2.5);
	const bool set = app.waitUntil([&app] { return app.call("project.get").at("fixedTempo") == 100.0; });
	clock->freeze(false);
	CHECK(set);
}
