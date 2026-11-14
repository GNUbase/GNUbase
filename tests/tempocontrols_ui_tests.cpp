// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo track's controls (#680, #831): Current Tempo at the project cursor in the transport bar, on the tempo track
// and in the tempo track editor, also during playback, Type of New Tempo Points on the tempo track, the fixed tempo
// line, selecting tempo events with Shift and a rectangle, the Tap Tempo Mode menu and its message, also in the
// transport panel, Insert Tempo Event at Cursor with its message and Select All Events.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/keycommands.h"
#include "ui/rmlui.h"
#include "ui/shell.h"
#include "ui/tempotrack.h"
#include "ui/timeformat.h"
#include "ui/tracklist.h"
#include "ui/transport.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
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
#include <tuple>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter; // half a second at the default 120 BPM
	constexpr int64_t g_second = 48000;                    // samples, at the default sample rate

	ui::Target point(double x, double y)
	{
		return {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	}

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

		std::vector<int64_t> selection() { return lane.curve().selection(); }

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::TempoLane lane{display, binding};
		ui::Automation automation{ui.shell};
	};

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	// An attribute of the first element the selector finds, or "" without one.
	std::string attributeOf(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json found = elements(app, selector);
		return found.empty() ? std::string() : found[0].value("attributes", Json::object()).value(name, std::string());
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json found = elements(app, selector);
		const Json classes = found.empty() ? Json::array() : found[0].value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(milliseconds(0));
	}

	// Double-clicks a field, types into it and presses Enter.
	void type(test::UiApp& app, const std::string& selector, const std::string& text, int clicks = 2)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, clicks, {});
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
		app.ui().key(std::nullopt, "Enter");
	}

	// The project window with its transport bar shown, at a dp ratio at which all its sections fit 1024 pixels.
	void showBar(test::UiApp& app)
	{
		app.setDpRatio(0.8F);
		app.appUi().showProject();
		app.appUi().docking().setVisible("project", "transportBar", true);
		app.shell().step(milliseconds(0));
	}

	Json tempo(test::UiApp& app)
	{
		return app.call("project.get").at("tempo");
	}

	// An element in the main window's documents.
	Rml::Element* find(test::UiApp& app, const std::string& selector)
	{
		Rml::Context& context = app.main().context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* found = context.GetDocument(i)->QuerySelector(selector))
				return found;
		return nullptr;
	}
}

TEST_CASE("Current Tempo is the tempo at the cursor and changes the tempo event at or before it", "[unit]")
{
	ui::TimeContext context; // 48 kHz
	context.tempo = time::TempoMap({{0, 120.0, false}, {4 * g_quarter, 100.0, false}, {8 * g_quarter, 140.0, true}});
	const auto at = [&context](time::Ticks ticks)
	{ return static_cast<int64_t>(std::llround(context.tempo.toSeconds(ticks) * context.sampleRate)); };
	CHECK(ui::currentTempo(context, 0) == 120.0);
	CHECK(ui::currentTempo(context, -1) == 120.0);
	CHECK(ui::currentTempo(context, 2 * g_second) == 100.0);                           // four quarters at 120 BPM
	CHECK(ui::currentTempo(context, at(6 * g_quarter)) == Approx(120.0).margin(0.01)); // halfway up the ramp
	CHECK(ui::currentTempo(context, at(10 * g_quarter)) == 140.0);

	// In fixed tempo mode it is the project's tempo.
	auto [command, parameters] = ui::currentTempoCommand(context, false, at(6 * g_quarter), 99.12345);
	CHECK(command == "project.set");
	CHECK(parameters == Json{{"tempo", 99.123}});
	// In tempo track mode the tempo event at or before the cursor: the first before the second.
	std::tie(command, parameters) = ui::currentTempoCommand(context, true, at(6 * g_quarter), 90.0);
	CHECK(command == "tempo.set");
	CHECK(parameters == Json{{"events", {{{"position", 4 * g_quarter}, {"bpm", 90.0}}}}});
	std::tie(command, parameters) = ui::currentTempoCommand(context, true, 2 * g_second, 90.0);
	CHECK(parameters.at("events")[0].at("position") == 4 * g_quarter);
	std::tie(command, parameters) = ui::currentTempoCommand(context, true, g_second, 1000.0);
	CHECK(parameters == Json{{"events", {{{"position", 0}, {"bpm", 999.0}}}}});
	CHECK(ui::tempoText(95.5) == "95.500");
}

TEST_CASE("the Tap Tempo message says where the tapped tempo goes", "[unit]")
{
	// Automatic (reference manual p. 1458): the project tempo in fixed tempo mode, else the selection, the playback
	// start while playing or the cursor.
	CHECK(ui::tapTempoMessage("auto", false, true, true) == "Tap Tempo: sets the project tempo");
	CHECK(ui::tapTempoMessage("auto", true, true, true) == "Tap Tempo: inserts a tempo event at the selection start");
	CHECK(ui::tapTempoMessage("auto", true, false, true) ==
		"Tap Tempo: inserts a tempo event at the last playback start position");
	CHECK(ui::tapTempoMessage("auto", true, false, false) == "Tap Tempo: inserts a tempo event at the cursor position");
	CHECK(ui::tapTempoMessage("selection", false, true, false) ==
		"Tap Tempo: inserts a tempo event at the selection start");
	CHECK(
		ui::tapTempoMessage("selection", true, false, true) == "Tap Tempo: inserts no tempo event without a selection");
	CHECK(ui::tapTempoMessage("playbackStart", true, true, false) ==
		"Tap Tempo: inserts a tempo event at the last playback start position");
	CHECK(
		ui::tapTempoMessage("cursor", false, true, true) == "Tap Tempo: inserts a tempo event at the cursor position");
	CHECK(ui::tapTempoMessage("project", true, true, true) == "Tap Tempo: changes the first tempo event");
	CHECK(ui::tapTempoMessage("project", false, false, false) == "Tap Tempo: sets the project tempo");
	CHECK(ui::tapTempoMessage("display", true, true, true) == "Tap Tempo: displays the tempo only");
}

TEST_CASE("the tempo curve shows the fixed tempo as a line over the inactive tempo track", "[unit]")
{
	Fixture fixture;
	fixture.call("track.add", {{"type", "tempo"}});
	fixture.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.points().size() == 2; }));
	auto* fixed = dynamic_cast<ui::Curve*>(fixture.document.QuerySelector(".tempo-curve .tempo-line.fixed-tempo"));
	REQUIRE(fixed != nullptr);

	// Fixed tempo mode: the tempo track is inactive, and the fixed tempo is a line at 120 BPM in a curve from 80 to
	// 140 BPM (reference manual pp. 1447, 1456).
	CHECK(fixture.curve().IsClassSet("inactive"));
	REQUIRE(fixed->points().size() == 1);
	CHECK(fixed->points()[0].value == Approx(40.0 / 60.0));
	CHECK(fixed->points()[0].shape == engine::AutomationShape::Step);
	fixture.call("project.set", {{"tempo", 130.0}});
	REQUIRE(fixture.waitUntil(
		[fixed] { return fixed->points().size() == 1 && fixed->points()[0].value == Approx(50.0 / 70.0); }));

	// Tempo track mode: the tempo track's curve, without the line.
	fixture.call("project.set", {{"tempoTrackActive", true}});
	REQUIRE(fixture.waitUntil([fixed] { return fixed->points().empty(); }));
	CHECK(!fixture.curve().IsClassSet("inactive"));
}

TEST_CASE("the tempo curve selects tempo events with Shift and Ctrl clicks and with a rectangle", "[unit]")
{
	Fixture fixture;
	fixture.call("track.add", {{"type", "tempo"}});
	fixture.call("project.set", {{"tempoTrackActive", true}});
	fixture.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	fixture.call("tempo.add", {{"position", 8 * g_quarter}, {"bpm", 110.0}, {"ramp", false}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.points().size() == 3; }));
	using Positions = std::vector<int64_t>;

	// A click selects an event, Shift-click adds one (reference manual p. 1450), Ctrl-click takes a selected one out.
	fixture.automation.click(std::nullopt, centre(*fixture.points()[1]), ui::MouseButton::Left, 1, {});
	fixture.step();
	CHECK(fixture.selection() == Positions{4 * g_quarter});
	fixture.automation.click(std::nullopt, centre(*fixture.points()[2]), ui::MouseButton::Left, 1, {"shift"});
	fixture.step();
	CHECK(fixture.selection() == Positions{4 * g_quarter, 8 * g_quarter});
	CHECK(fixture.points()[2]->IsClassSet("selected"));
	// Shift-click on a selected event takes it out too (reference manual p. 1450).
	fixture.automation.click(std::nullopt, centre(*fixture.points()[2]), ui::MouseButton::Left, 1, {"shift"});
	fixture.step();
	CHECK(fixture.selection() == Positions{4 * g_quarter});
	fixture.automation.click(std::nullopt, centre(*fixture.points()[2]), ui::MouseButton::Left, 1, {"shift"});
	fixture.step();
	CHECK(fixture.selection() == Positions{4 * g_quarter, 8 * g_quarter});
	fixture.automation.click(std::nullopt, centre(*fixture.points()[1]), ui::MouseButton::Left, 1, {"ctrl"});
	fixture.step();
	CHECK(fixture.selection() == Positions{8 * g_quarter});

	// A drag beside the events selects those in its rectangle and adds none; with Shift it adds them.
	Rml::Element& curve = fixture.curve();
	const double height = curve.GetOffsetHeight();
	fixture.automation.drag(std::nullopt, fixture.at(1.5, 2.0), fixture.at(3.0, height - 2.0));
	fixture.step();
	CHECK(fixture.selection() == Positions{4 * g_quarter});
	CHECK(!fixture.document.QuerySelector(".tempo-curve .tool-rectangle")->IsClassSet("shown"));
	fixture.automation.drag(std::nullopt, fixture.at(3.0, 2.0), fixture.at(6.0, height - 2.0), {"shift"});
	fixture.step();
	CHECK(fixture.selection() == Positions{4 * g_quarter, 8 * g_quarter});
	CHECK(fixture.tempoTrack().size() == 3);

	// A click there adds an event, and selects none.
	fixture.automation.click(std::nullopt, fixture.at(5.0, height / 2.0), ui::MouseButton::Left, 1, {});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.tempoTrack().size() == 4; }));
	CHECK(fixture.selection().empty());

	// Select All Events.
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.points().size() == 4; }));
	fixture.lane.curve().selectAll();
	CHECK(fixture.selection().size() == 4);
}

TEST_CASE("the transport bar's tempo field is the tempo at the cursor in tempo track mode", "[ui]")
{
	test::UiApp app{{1024, 500}};
	showBar(app);
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	REQUIRE(app.waitUntil([&app] { return attributeOf(app, "#transport-bar #tempo", "value") == "120"; }));

	// After the tempo event, its tempo, which the field changes (reference manual p. 1446).
	app.call("transport.locate", {{"position", 5 * g_second / 2}});
	REQUIRE(app.waitUntil([&app] { return attributeOf(app, "#transport-bar #tempo", "value") == "100"; }));
	type(app, "#transport-bar #tempo", "90");
	REQUIRE(app.waitUntil([&app] { return tempo(app)[1].at("bpm") == 90.0; }));
	CHECK(tempo(app)[0].at("bpm") == 120.0);
	REQUIRE(app.waitUntil([&app] { return attributeOf(app, "#transport-bar #tempo", "value") == "90"; }));

	// In fixed tempo mode the fixed tempo, wherever the cursor is.
	app.call("project.set", {{"tempoTrackActive", false}});
	REQUIRE(app.waitUntil([&app] { return attributeOf(app, "#transport-bar #tempo", "value") == "120"; }));
	type(app, "#transport-bar #tempo", "95");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("fixedTempo") == 95.0; }));
	CHECK(app.call("project.get").at("tempoTrack")[1].at("bpm") == 90.0);
}

TEST_CASE("the Tap Tempo Mode menu shows the mode the user setting has", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.call("settings.set", {{"key", "transport.tapTempoMode"}, {"value", "project"}});
	showBar(app);
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "menuitem[tap-mode=project]")[0].at("attributes").contains("checked"); }));
	CHECK(!elements(app, "menuitem[tap-mode=auto]")[0].at("attributes").contains("checked"));
	// Taps say what this mode does.
	app.ui().click(std::nullopt, {"#transport-bar #tap-tempo", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return hasClass(app, "#tap-tempo-message", "shown"); }));
	CHECK(elements(app, "#tap-tempo-message")[0].value("text", "") == "Tap Tempo: sets the project tempo");

	// A change of the setting after the bar opened moves the check.
	app.call("settings.set", {{"key", "transport.tapTempoMode"}, {"value", "cursor"}});
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "menuitem[tap-mode=cursor]")[0].at("attributes").contains("checked"); }));
	CHECK(!elements(app, "menuitem[tap-mode=project]")[0].at("attributes").contains("checked"));
}

TEST_CASE("the transport panel's Tap Tempo message says where the tapped tempo goes", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.setDpRatio(0.8F); // all the transport's sections fit the panel
	app.appUi().showProject();
	app.ui().key(std::nullopt, "F2");
	app.shell().step(milliseconds(0));
	ui::Window* panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);

	// A tap in the panel shows the message in the panel, over its right end (reference manual p. 1455).
	app.ui().click(panel->id(), {"#tap-tempo", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.shell().step(milliseconds(0));
	const Json message = app.ui().dom(panel->id(), "#tap-tempo-message", 0).at("elements");
	REQUIRE(message.size() == 1);
	CHECK(message[0].value("text", "") == "Tap Tempo: sets the project tempo");
	const Json classes = message[0].value("classes", Json::array());
	CHECK(std::find(classes.begin(), classes.end(), "shown") != classes.end());
}

TEST_CASE("the Tap Tempo Mode menu picks where the tapped tempo goes, and tapping says where", "[ui]")
{
	test::UiApp app{{1024, 500}};
	showBar(app);
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "menuitem[tap-mode=auto]")[0].at("attributes").contains("checked"); }));

	// The pop-up menu, a right click on Tap Tempo, sets the user setting (reference manual p. 1458).
	app.ui().click(std::nullopt, {"#transport-bar #tap-tempo", 0, std::nullopt}, ui::MouseButton::Right, 1, {});
	click(app, "menuitem[tap-mode=cursor]");
	REQUIRE(app.waitUntil(
		[&app] { return app.call("settings.get", {{"key", "transport.tapTempoMode"}}).at("value") == "cursor"; }));
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "menuitem[tap-mode=cursor]")[0].at("attributes").contains("checked"); }));
	CHECK(!elements(app, "menuitem[tap-mode=auto]")[0].at("attributes").contains("checked"));

	// While tapping, Tap Tempo is .tapping and a message says where the tempo goes (p. 1455).
	app.call("transport.locate", {{"position", g_second}});
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	for (int tap = 0; tap < 3; ++tap)
	{
		if (tap > 0)
			clock->advance(0.5);
		click(app, "#transport-bar #tap-tempo");
	}
	const bool tapping = hasClass(app, "#transport-bar #tap-tempo", "tapping");
	const bool shown = hasClass(app, "#tap-tempo-message", "shown");
	const Json message = elements(app, "#tap-tempo-message");
	clock->advance(2.5);
	const bool added = app.waitUntil([&app] { return app.call("project.get").at("tempoTrack").size() == 2; });
	clock->freeze(false);
	CHECK(tapping);
	CHECK(shown);
	REQUIRE(message.size() == 1);
	CHECK(message[0].value("text", "") == "Tap Tempo: inserts a tempo event at the cursor position");
	REQUIRE(added);
	CHECK(app.call("project.get").at("tempoTrack")[1].at("position") == 2 * g_quarter);
	REQUIRE(app.waitUntil([&app] { return !hasClass(app, "#transport-bar #tap-tempo", "tapping"); }));
}

TEST_CASE("the tempo track's Current Tempo and Type of New Tempo Points in the track list", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	const auto value = [&app, &row]
	{
		const Json found = elements(app, row + " input.current-tempo");
		return found.empty() ? std::string() : found[0].value("value", std::string());
	};
	REQUIRE(app.waitUntil([&value] { return value() == "120.000"; }));
	CHECK(elements(app, row + " button[control]").size() == 1); // Activate Tempo Track
	REQUIRE(elements(app, row + " select.tempo-type").size() == 1);

	// In fixed tempo mode it is the fixed tempo, which it changes; what isn't a tempo goes.
	type(app, row + " input.current-tempo", "90", 1);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").value("fixedTempo", 0.0) == 90.0; }));
	REQUIRE(app.waitUntil([&value] { return value() == "90.000"; }));
	type(app, row + " input.current-tempo", "fast", 1);
	REQUIRE(app.waitUntil([&value] { return value() == "90.000"; }));
	CHECK(app.call("project.get").value("fixedTempo", 0.0) == 90.0);

	// The tempo at the cursor (reference manual p. 162), which it changes.
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	app.call("transport.locate", {{"position", 3 * g_second}});
	REQUIRE(app.waitUntil([&value] { return value() == "100.000"; }));
	type(app, row + " input.current-tempo", "105", 1);
	REQUIRE(app.waitUntil([&app] { return tempo(app)[1].at("bpm") == 105.0; }));
	CHECK(tempo(app)[0].at("bpm") == 120.0);

	// The type of new tempo events is the user setting editing.tempoType, both ways.
	auto* type = dynamic_cast<Rml::ElementFormControlSelect*>(find(app, row + " select.tempo-type"));
	REQUIRE(type != nullptr);
	CHECK(type->GetValue() == "auto");
	type->SetValue("step");
	REQUIRE(app.waitUntil(
		[&app] { return app.call("settings.get", {{"key", "editing.tempoType"}}).at("value") == "step"; }));
	app.call("settings.set", {{"key", "editing.tempoType"}, {"value", "ramp"}});
	REQUIRE(app.waitUntil([type] { return type->GetValue() == "ramp"; }));
}

TEST_CASE("the transport bar's tempo field follows the cursor during playback", "[ui]")
{
	test::UiApp app{{1024, 500}};
	showBar(app);
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 2 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	REQUIRE(app.waitUntil([&app] { return attributeOf(app, "#transport-bar #tempo", "value") == "120"; }));

	// Playback carries the cursor over the tempo event, and the field follows without a locate (p. 1446).
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	REQUIRE(app.waitUntil([&app] { return attributeOf(app, "#transport-bar #tempo", "value") == "100"; }));
	CHECK(app.call("transport.get").at("position").get<int64_t>() >= g_second);
	app.call("transport.stop");
}

TEST_CASE("Current Tempo on the tempo track and in the editor follows the cursor during playback", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 2 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	REQUIRE(app.appUi().toggleTempoEditor() != nullptr);
	const ui::WindowId editor = app.appUi().tempoWindow();
	const auto trackValue = [&app, &row]
	{
		const Json found = elements(app, row + " input.current-tempo");
		return found.empty() ? std::string() : found[0].value("value", std::string());
	};
	const auto editorValue = [&app, editor]
	{
		const Json found = app.ui().dom(editor, "#current-tempo", 0).at("elements");
		return found.empty() ? std::string() : found[0].value("value", std::string());
	};
	REQUIRE(
		app.waitUntil([&trackValue, &editorValue] { return trackValue() == "120.000" && editorValue() == "120.000"; }));

	// Playback carries the cursor over the tempo event, and both fields follow without a locate (p. 1446).
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	REQUIRE(app.waitUntil([&trackValue] { return trackValue() == "100.000"; }));
	REQUIRE(app.waitUntil([&editorValue] { return editorValue() == "100.000"; }));
	CHECK(app.call("transport.get").at("position").get<int64_t>() >= g_second);
	app.call("transport.stop");
}

TEST_CASE("the track list watches the cursor while playing only when it has a Current Tempo field", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 2 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	const auto position = [&app] { return app.call("transport.get").at("position").get<int64_t>(); };

	// Without a tempo track the list has no Current Tempo field: the frames of the playback build no time context
	// (#1338).
	app.call("transport.play");
	REQUIRE(app.waitUntil([&position] { return position() >= g_second; }));
	const uint64_t builds = ui::projectTimeBuilds();
	REQUIRE(app.waitUntil([&position] { return position() >= 2 * g_second; }));
	CHECK(ui::projectTimeBuilds() == builds);

	// A tempo track added while playing brings the field, and it follows the cursor right away.
	const auto track = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	const auto value = [&app, &row]
	{
		const Json found = elements(app, row + " input.current-tempo");
		return found.empty() ? std::string() : found[0].value("value", std::string());
	};
	REQUIRE(app.waitUntil([&value] { return value() == "100.000"; }));
	app.call("transport.stop");
}

TEST_CASE("the track list's Current Tempo reads the display's time context, not one built per frame", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	const auto value = [&app, &row]
	{
		const Json found = elements(app, row + " input.current-tempo");
		return found.empty() ? std::string() : found[0].value("value", std::string());
	};
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 2 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	REQUIRE(app.waitUntil([&value] { return value() == "120.000"; }));

	// Playback carries the cursor over the tempo event; the frames that follow it read the context the display
	// keeps (#1338), as the transport bar reads its own.
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	REQUIRE(app.waitUntil([&value] { return value() == "100.000"; }));
	const uint64_t builds = ui::projectTimeBuilds();
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("position").get<int64_t>() >= 2 * g_second; }));
	CHECK(ui::projectTimeBuilds() == builds);
	app.call("transport.stop");

	// Locates and undo keep the field following.
	app.call("transport.locate", {{"position", 0}});
	REQUIRE(app.waitUntil([&value] { return value() == "120.000"; }));
	type(app, row + " input.current-tempo", "90", 1);
	REQUIRE(app.waitUntil([&value] { return value() == "90.000"; }));
	app.call("history.undo");
	REQUIRE(app.waitUntil([&value] { return value() == "120.000"; }));
}

TEST_CASE(
	"the track list's Current Tempo follows the cursor after tracks are added and removed during playback", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	const auto value = [&app, &row]
	{
		const Json found = elements(app, row + " input.current-tempo");
		return found.empty() ? std::string() : found[0].value("value", std::string());
	};
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 2 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	app.call("tempo.add", {{"position", 8 * g_quarter}, {"bpm", 90.0}, {"ramp", false}}); // at 4.6 seconds
	REQUIRE(app.waitUntil([&value] { return value() == "120.000"; }));

	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	REQUIRE(app.waitUntil([&value] { return value() == "100.000"; }));

	// Tracks added before and after the tempo track and removed again rebuild the rows: the list keeps the new
	// fields and drops the old ones (#1406), and the frames of the playback go on filling the new field.
	const auto before = app.call("track.add", {{"type", "midi"}, {"index", 0}}).at("track").at("id").get<uint64_t>();
	const auto after = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	// The UI steps between the changes, so that it builds the rows with the added tracks and again without them.
	const auto rowOf = [](uint64_t id) { return "#track-list listitem[value=\"" + std::to_string(id) + "\"]"; };
	REQUIRE(
		app.waitUntil([&] { return !elements(app, rowOf(before)).empty() && !elements(app, rowOf(after)).empty(); }));
	app.call("track.remove", {{"track", before}});
	app.call("track.remove", {{"track", after}});
	REQUIRE(app.waitUntil([&] { return elements(app, rowOf(before)).empty() && elements(app, rowOf(after)).empty(); }));
	CHECK(app.call("transport.get").at("position").get<int64_t>() < 4 * g_second); // before the tempo event
	REQUIRE(app.waitUntil([&value] { return value() == "90.000"; }, std::chrono::seconds(10)));
	app.call("transport.stop");
}

TEST_CASE("Insert Tempo Event at Cursor asks for a tempo and adds a tempo event at the cursor", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("transport.locate", {{"position", g_second}});

	// The dialog starts at the tempo at the cursor (reference manual pp. 1452-1453).
	app.keys().run("tempo.insertAtCursor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#insert-tempo-dialog").empty(); }));
	CHECK(attributeOf(app, "#insert-tempo-value", "value") == "120.000");
	type(app, "#insert-tempo-value", "135.5");
	REQUIRE(app.waitUntil([&app] { return tempo(app).size() == 2; }));
	CHECK(tempo(app)[1].at("position") == 2 * g_quarter);
	CHECK(tempo(app)[1].at("bpm") == 135.5);
	REQUIRE(app.waitUntil([&app] { return elements(app, "#insert-tempo-dialog").empty(); }));
}

TEST_CASE("the Insert Tempo dialog says what it can't take and stays open", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("transport.locate", {{"position", g_second}});
	const auto message = [&app]
	{
		const Json found = elements(app, "#insert-tempo-message");
		return found.empty() ? std::string() : found[0].value("text", std::string());
	};

	// Text that isn't a tempo (reference manual pp. 1452-1453).
	app.keys().run("tempo.insertAtCursor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#insert-tempo-dialog").empty(); }));
	type(app, "#insert-tempo-value", "fast");
	REQUIRE(app.waitUntil([&message] { return message() == "The tempo must be a number of BPM"; }));
	CHECK(!elements(app, "#insert-tempo-dialog").empty());
	CHECK(tempo(app).size() == 1);

	// A tempo the engine refuses: outside its 1 to 999 BPM.
	type(app, "#insert-tempo-value", "1000");
	REQUIRE(app.waitUntil([&message] { return message().find("999") != std::string::npos; }));
	CHECK(!elements(app, "#insert-tempo-dialog").empty());
	CHECK(tempo(app).size() == 1);

	// A tempo it takes closes the dialog again.
	type(app, "#insert-tempo-value", "135.5");
	REQUIRE(app.waitUntil([&app] { return tempo(app).size() == 2; }));
	REQUIRE(app.waitUntil([&app] { return elements(app, "#insert-tempo-dialog").empty(); }));
}

TEST_CASE("Select All Events selects every tempo event of the selected tempo track", "[ui]")
{
	test::UiApp app{{1024, 500}};
	app.appUi().showProject();
	const auto audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const auto tempoTrack = app.call("track.add", {{"type", "tempo"}}).at("track").at("id").get<uint64_t>();
	app.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}});
	ui::TempoLane* lane = app.appUi().tempoLane();
	REQUIRE(lane != nullptr);
	REQUIRE(app.waitUntil([lane] { return lane->curve().tempo().tempoTrack.points().size() == 2; }));

	// Only with the tempo track selected (reference manual p. 1450).
	REQUIRE(app.waitUntil(
		[&app, audio]
		{
			app.appUi().trackList()->select({audio});
			return app.appUi().trackList()->selection().size() == 1;
		}));
	CHECK(!app.keys().canRun("tempo.selectAll", app.main(), nullptr));
	app.appUi().trackList()->select({tempoTrack});
	CHECK(app.keys().canRun("tempo.selectAll", app.main(), nullptr));
	app.keys().run("tempo.selectAll", Json::object(), app.main(), nullptr);
	CHECK(lane->curve().selection() == std::vector<int64_t>{0, 4 * g_quarter});
	CHECK(!elements(app, "menuitem[command=\"tempo.selectAll\"]").empty());
}
