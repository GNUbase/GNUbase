// SPDX-License-Identifier: GPL-3.0-or-later
// The sample editor in the app (#140, docs/features/sample-editor.md), driven with ui::Automation as MCP's UI tools
// drive it: opening it, the info line, the overview and the waveform display, the tools, the snap point, the
// inspector's Range and Process sections, the regions list and the window layout. tests/sampleeditor_tests.cpp checks
// the engine's side.
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_second = 48000;
	constexpr int64_t g_frames = 9600;

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	// An audio track with an event at one second playing frames 1000 to 5000 of a sine that crosses zero every 100
	// frames, selected in the project window and open in the sample editor.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			test::Audio sine;
			for (int64_t frame = 0; frame < g_frames; ++frame)
				sine.samples.push_back(
					static_cast<float>(0.5 * std::sin(static_cast<double>(frame) * 3.14159265358979323846 / 100.0)));
			test::writeWav(file, sine);
			track =
				app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<uint64_t>();
			const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media");
			clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
			event =
				app.call("audioEvent.add",
					   {{"track", track}, {"clip", clip}, {"position", g_second}, {"offset", 1000}, {"length", 4000}})
					.at("event")
					.at("id")
					.get<uint64_t>();
			REQUIRE(app.waitUntil(
				[&] { return display.project().is_object() && display.project().at("audioEvents").size() == 1; }));
		}

		// Open/Close Editor with the event selected (p. 613).
		void open()
		{
			display.select({event});
			app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([&] { return app.appUi().sampleEditorWindow() != nullptr; }));
			window = app.appUi().sampleEditorWindowId();
			REQUIRE(app.waitUntil([&] { return !has(".sample-editor", "empty") && editor().scale() > 0.0; }));
		}

		ui::SampleEditor& editor() { return *app.appUi().sampleEditorWindow(); }

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		std::string text(const std::string& selector) { return elements(selector).at(0).value("text", ""); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json names = found.at(0).value("classes", Json::array());
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		Json view() { return app.call("sampleEditor.get"); }

		Json regions()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("clips"))
				if (each.at("id") == clip)
					return each.at("regions");
			return Json::array();
		}

		float dpRatio() { return app.shell().find(*window)->context().GetDensityIndependentPixelRatio(); }

		// The x of a clip frame in the waveform display, in the window's pixels.
		double x(double frame)
		{
			return box(".se-display").at("x").get<double>() +
				((frame - editor().start()) / editor().scale() * dpRatio());
		}

		ui::Target at(double frame)
		{
			const Json display = box(".se-display");
			const double y = display.at("y").get<double>() + (display.at("height").get<double>() / 2.0);
			return {"", 0, ui::Point{static_cast<int>(std::lround(x(frame))), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, 1, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void drag(const ui::Target& from, const ui::Target& to)
		{
			app.ui().drag(window, from, to);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void tool(const std::string& name)
		{
			click(of(".se-tool[se-tool=" + name + "]"));
			REQUIRE(has(".se-tool[se-tool=" + name + "]", "checked"));
		}

		static bool near(double a, double b, double tolerance = 1.5) { return std::abs(a - b) <= tolerance; }

		test::UiApp app{{1200, 800}};
		ui::EventDisplay& display;
		test::TempDir dir;
		std::filesystem::path file = dir.path() / "sine.wav";
		std::optional<ui::WindowId> window;
		uint64_t track = 0;
		uint64_t clip = 0;
		uint64_t event = 0;
	};
}

TEST_CASE("the sample editor opens an audio event with its info line, overview and waveform", "[ui]")
{
	Fixture f;
	f.open();
	CHECK(f.view().at("event") == f.event);
	// The info line (p. 614).
	CHECK(f.text(".se-info [field=format]") == "48000 Hz, Mono");
	CHECK(f.text(".se-info [field=length]") == "9600");
	CHECK(f.text(".se-info [field=selectionStart]").empty());
	// The display first shows the whole clip, shading what lies outside the event; the overview shows the event.
	const Json display = f.box(".se-display");
	const double width = display.at("width").get<double>();
	CHECK(f.near(f.box(".se-before").at("width").get<double>(), width * 1000 / g_frames));
	CHECK(f.near(f.box(".se-after").at("x").get<double>() - display.at("x").get<double>(), width * 5000 / g_frames));
	const Json overview = f.box(".se-overview");
	const double overviewWidth = overview.at("width").get<double>();
	CHECK(f.near(f.box(".se-overview-event").at("x").get<double>() - overview.at("x").get<double>(),
		overviewWidth * 1000 / g_frames));
	CHECK(f.near(f.box(".se-overview-view").at("width").get<double>(), overviewWidth, 3)); // with its border
	// Both waveforms get their columns from media.peaks: the sine's peaks reach about half of full scale.
	Rml::ElementDocument* document = f.app.shell().find(*f.window)->context().GetDocument(0);
	for (const char* selector : {".se-wave", ".se-overview-wave"})
	{
		auto* wave = dynamic_cast<ui::Waveform*>(document->QuerySelector(selector));
		REQUIRE(wave != nullptr);
		REQUIRE(f.app.waitUntil([&] { return !wave->channels().empty() && !wave->channels()[0].high.empty(); }));
		const std::vector<int16_t>& high = wave->channels()[0].high;
		CHECK(std::abs(*std::max_element(high.begin(), high.end()) - 16384) < 200);
	}
	// Closing the window closes the view; the engine opens and closes the window too.
	f.app.shell().close(*f.window);
	REQUIRE(f.app.waitUntil([&] { return f.view().at("open") == false; }));
	f.app.call("sampleEditor.open", {{"event", f.event}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().sampleEditorWindow() != nullptr; }));
	f.app.call("sampleEditor.close");
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().sampleEditorWindow() == nullptr; }));
}

TEST_CASE("the range selection tool and the Range section select ranges of the clip", "[ui]")
{
	Fixture f;
	f.open();
	// A drag selects a range (p. 635), shown in the display, the overview and the info line.
	f.drag(f.at(2000), f.at(3000));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionEnd").get<int64_t>() > 0; }));
	Json view = f.view();
	CHECK(std::abs(view.at("selectionStart").get<int64_t>() - 2000) <= 10);
	CHECK(std::abs(view.at("selectionEnd").get<int64_t>() - 3000) <= 10);
	REQUIRE(f.app.waitUntil([&] { return !f.text(".se-info [field=selectionLength]").empty(); }));
	CHECK(f.near(f.box(".se-selection").at("x").get<double>(), f.x(view.at("selectionStart").get<double>())));
	// Shift and a click moves the nearer end.
	f.click(f.at(3500), {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionEnd").get<int64_t>() > 3400; }));
	CHECK(f.view().at("selectionStart") == view.at("selectionStart"));
	// With Snap to Zero Crossing, the ends go to the sine's zero crossings.
	f.click(of("#se-zero-crossing"));
	REQUIRE(f.app.waitUntil([&] { return f.has("#se-zero-crossing", "checked"); }));
	CHECK(f.app.call("snap.get").at("zeroCrossing") == true);
	f.drag(f.at(1230), f.at(2870));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionStart") == 1200; }));
	CHECK(f.view().at("selectionEnd") == 2900);

	// The Select menu (p. 618).
	f.click(of(".se-select[select=all]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionEnd") == g_frames; }));
	f.click(of(".se-select[select=event]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionStart") == 1000; }));
	CHECK(f.view().at("selectionEnd") == 5000);
	// Set Locators to Selection Range and Locate Selection, in the project's samples.
	f.click(of("#se-set-locators"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("transport.get").at("left") == g_second; }));
	CHECK(f.app.call("transport.get").at("right") == g_second + 4000);
	f.click(of(".se-select[select=none]"));
	REQUIRE(f.app.waitUntil([&] { return f.has(".se-selection", "off"); }));
	f.click(of(".se-select[select=loop]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionStart") == 1000; }));
	f.click(of("#se-locate"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("transport.get").at("position") == g_second; }));
}

TEST_CASE("regions are added from the selection, listed, selected and removed", "[ui]")
{
	Fixture f;
	f.open();
	// Without a selection, Event or Range as Region makes a region of the event's section (p. 620).
	f.click(of("#se-range-region"));
	REQUIRE(f.app.waitUntil([&] { return f.regions().size() == 1; }));
	CHECK(f.regions()[0].at("start") == 1000);
	CHECK(f.regions()[0].at("end") == 5000);
	// Add Region makes one of the selection (p. 625); the list shows both, by start, the new one selected.
	f.app.call("sampleEditor.set", {{"selectionStart", 200}, {"selectionEnd", 800}});
	REQUIRE(f.app.waitUntil([&] { return f.text(".se-info [field=selectionStart]") == "200"; }));
	f.click(of("#se-add-region"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".se-region").size() == 2; }));
	CHECK(f.text(".se-region-name") == "Region 2");
	CHECK(f.text(".se-region-start") == "200");
	REQUIRE(f.app.waitUntil([&] { return f.has(".se-region", "selected"); }));
	CHECK(f.elements(".se-region.selected").size() == 1);
	// Clicking the other one and Select Region selects its section and zooms to it.
	f.click(of(".se-region[region=\"" + f.regions()[1].at("id").dump() + "\"]"));
	f.click(of("#se-select-region"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selectionStart") == 1000; }));
	CHECK(f.view().at("selectionEnd") == 5000);
	CHECK(std::abs(f.editor().start() - 1000) < 1);
	CHECK(f.app.waitUntil(
		[&]
		{
			return f.near(f.box(".se-region-shown").at("width").get<double>(),
				f.box(".se-display").at("width").get<double>(), 3); // with its border
		}));
	// Remove Region removes the selected one; undo brings it back.
	f.click(of("#se-remove-region"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".se-region").size() == 1; }));
	CHECK(f.regions()[0].at("start") == 200);
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.elements(".se-region").size() == 2; }));
}

TEST_CASE("the snap point, the zoom, play and scrub tools, the overview and the window layout", "[ui]")
{
	Fixture f;
	f.app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	f.app.call("audio.start");
	f.open();
	// Without a snap point of its own, the event snaps at its start (p. 626); dragging it sets one (p. 628).
	REQUIRE(f.app.waitUntil([&] { return f.near(f.box(".se-snap-point").at("x").get<double>() + 1, f.x(1000), 2.5); }));
	f.drag(of(".se-snap-point"), f.at(2500));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json project = f.app.call("project.get");
			const Json& point = project.at("audioEvents").at(0).at("snapPoint");
			return !point.is_null() && std::abs(point.get<int64_t>() - 2500) <= 10;
		}));
	CHECK(f.app.call("history.get").at("steps").back().at("action") == "Snap Point to Cursor");

	// The zoom tool zooms in where it clicks, with Alt out again (p. 630); the info line shows samples per pixel.
	const double full = f.editor().scale();
	CHECK(f.text(".se-info [field=zoom]").find("samples/pixel") != std::string::npos);
	f.tool("zoom");
	f.click(f.at(4000));
	CHECK(std::abs(f.editor().scale() - (full / 2)) < 1e-9);
	const ui::Point clicked = f.at(4000).point.value_or(ui::Point{});
	CHECK(f.near(f.x(4000), clicked.x, 2.0)); // the frame clicked stays under the mouse
	f.click(f.at(4000), {"alt"});
	CHECK(std::abs(f.editor().scale() - full) < 1e-9);
	// Zoomed in, a click in the overview shows that part of the clip (p. 615).
	f.click(f.at(4000));
	const Json overview = f.box(".se-overview");
	f.click({"", 0,
		ui::Point{static_cast<int>(overview.at("x").get<double>() + (overview.at("width").get<double>() * 0.75)),
			static_cast<int>(overview.at("y").get<double>() + 5)}});
	CHECK(std::abs(f.editor().start() - (g_frames * 0.75) + (g_frames / 4.0)) < 20);

	// The play tool auditions the track from the frame pressed until the release; the scrub tool scrubs it.
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&modes](const engine::Event& event) { modes.push_back(event.data.at("mode").get<std::string>()); });
	f.tool("play");
	f.click(f.at(6000));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play", "off"};
		}));
	f.tool("scrub");
	f.drag(f.at(6000), f.at(6500));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes.size() >= 4 && modes[2] == "scrub" && modes.back() == "off";
		}));

	// Set up Window Layout's switches hide and show the info line, the overview and the regions (p. 614).
	for (const char* part : {"info", "overview", "regions"})
	{
		const std::string button = std::string(".se-layout[layout=") + part + "]";
		f.click(of(button));
		CHECK(f.has(".sample-editor", std::string("no-") + part));
		CHECK(!f.has(button, "checked"));
		f.click(of(button));
		CHECK(!f.has(".sample-editor", std::string("no-") + part));
	}
}

TEST_CASE("Open/Close Editor without an audio event selected closes the sample editor", "[ui]")
{
	Fixture f;
	f.open();
	f.display.select({});
	f.app.keys().run("editor.openClose", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.view().at("open") == false; }));
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().sampleEditorWindow() == nullptr; }));
}
