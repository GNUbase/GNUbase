// SPDX-License-Identifier: GPL-3.0-or-later
// Ruler tracks in the UI (#447): the time a ruler track shows with and without its time offset, the rulers on the
// ruler tracks' lanes in the event display on the fake window system, and in the app's UI the track list's ruler
// track controls and a golden screenshot of ruler lanes in each time format, with and without an offset.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/rulertracks.h"
#include "ui/shell.h"
#include "ui/timeformat.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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
	constexpr std::array<const char*, 5> g_formats = {"barsBeats", "seconds", "timecode", "samples", "userFrames"};

	// The event display with the ruler tracks' rulers, filling a 1000 by 400 window of the fake UI at a monitor scale
	// of 1, so that a dp is a pixel, following an engine on the dummy device.
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

		Rml::ElementList rulers() const
		{
			Rml::ElementList found;
			document.QuerySelectorAll(found, ".event-area ruler.ruler-lane");
			return found;
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::RulerLanes lanes{display};
	};

	std::vector<std::string> labels(ui::TimeFormat format, const ui::TimeContext& time, double from, double to)
	{
		std::vector<ui::RulerMark> marks;
		ui::rulerScale(format, time).marks(from, to, 1.0, marks);
		std::vector<std::string> found;
		for (const ui::RulerMark& mark : marks)
			if (!mark.label.empty())
				found.push_back(mark.label);
		return found;
	}

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	void click(test::UiApp& app, const std::string& selector, int index = 0, int count = 1)
	{
		app.ui().click(std::nullopt, {selector, index, std::nullopt}, ui::MouseButton::Left, count, {});
		app.shell().step(milliseconds(0));
	}
}

TEST_CASE("a ruler track counts from its time offset while it is on", "[unit]")
{
	// The project setup's start time and bar offset, which a ruler track follows with its offset off.
	ui::TimeContext project;
	project.startTime = 3600.0;
	project.barOffset = 4;
	const Json off = {{"timeBase", "musical"}, {"timeOffset", 4 * time::g_ticksPerQuarter}, {"timeOffsetOn", false}};
	const ui::TimeContext followed = ui::rulerTrackTime(project, off);
	CHECK(ui::formatTime(ui::TimeFormat::Seconds, 1.0, followed) == "1:00:01.000");
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 2.0, followed) == "6.1.1.0");
	CHECK(ui::formatTime(ui::TimeFormat::Samples, 1.0, followed) == "48000");

	// At bar 2, two seconds at 120 BPM: the ruler shows zero there, and negative times before it.
	Json on = off;
	on["timeOffsetOn"] = true;
	const ui::TimeContext offset = ui::rulerTrackTime(project, on);
	CHECK(ui::formatTime(ui::TimeFormat::Seconds, 3.0, offset) == "0:00:01.000");
	CHECK(ui::formatTime(ui::TimeFormat::Timecode, 1.0, offset) == "-00:00:01:00");
	CHECK(ui::formatTime(ui::TimeFormat::UserFrames, 2.5, offset) == "00:00:00:15");
	CHECK(ui::formatTime(ui::TimeFormat::Samples, 3.0, offset) == "48000");
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 2.0, offset) == "1.1.1.0");
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 0.0, offset) == "0.1.1.0");
	CHECK(labels(ui::TimeFormat::Seconds, offset, 1.0, 4.5) ==
		std::vector<std::string>{"-0:00:01", "0:00:00", "0:00:01", "0:00:02"});
	CHECK(labels(ui::TimeFormat::Samples, offset, 2.0, 3.5).front() == "0");

	// On a linear track the offset is in samples: bar 2 is the bar that 2.5 seconds are in.
	const ui::TimeContext linear =
		ui::rulerTrackTime(project, {{"timeBase", "linear"}, {"timeOffset", 120000}, {"timeOffsetOn", true}});
	CHECK(ui::formatTime(ui::TimeFormat::Seconds, 2.5, linear) == "0:00:00.000");
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 2.0, linear) == "1.1.1.0");
}

TEST_CASE("ruler tracks show a ruler on their lanes in their time formats", "[unit]")
{
	Fixture fixture;
	fixture.call("track.add", {{"type", "audio"}});
	const uint64_t bars = fixture.call("track.add", {{"type", "ruler"}}).at("track").at("id").get<uint64_t>();
	const uint64_t timecode =
		fixture.call("track.add", {{"type", "ruler"}, {"timeFormat", "timecode"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(fixture.waitUntil(
		[&fixture] { return fixture.rulers().size() == 2 && fixture.rulers()[1]->GetOffsetHeight() > 0.0F; }));

	// Over the second and third lanes, each with its track and format, following the view.
	const std::vector<double>& tops = fixture.display.laneTops();
	const float lanesTop = fixture.document.QuerySelector("event-lanes.events")->GetAbsoluteTop();
	for (size_t i = 0; i < 2; ++i)
	{
		Rml::Element& ruler = *fixture.rulers()[i];
		CHECK(!ruler.IsClassSet("hidden"));
		CHECK(ruler.GetAbsoluteTop() - lanesTop == Approx(tops[i + 1]));
		CHECK(ruler.GetOffsetHeight() == Approx(tops[i + 2] - tops[i + 1]));
		CHECK(ruler.GetAttribute<Rml::String>("track", "") == std::to_string(i == 0 ? bars : timecode));
		CHECK(ruler.GetAttribute<Rml::String>("scale", "") ==
			fixture.document.QuerySelector("event-lanes.events")->GetAttribute<Rml::String>("scale", ""));
	}
	CHECK(fixture.rulers()[0]->GetAttribute<Rml::String>("format", "") == "barsBeats");
	CHECK(fixture.rulers()[1]->GetAttribute<Rml::String>("format", "") == "timecode");

	// A new time format shows; a ruler track that goes takes its ruler with it.
	fixture.call("track.set", {{"track", bars}, {"timeFormat", "samples"}});
	REQUIRE(fixture.waitUntil(
		[&fixture] { return fixture.rulers()[0]->GetAttribute<Rml::String>("format", "") == "samples"; }));
	fixture.call("track.remove", {{"track", timecode}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.rulers().size() == 1; }));
}

TEST_CASE("ruler tracks show their rulers in each time format and have their controls in the track list", "[ui]")
{
	test::UiApp app({900, 560});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.call("project.set", {{"length", 60.0}});
	// Each format without and with an offset at bar 2, two seconds in.
	for (const bool on : {false, true})
		for (const char* format : g_formats)
			app.call("track.add",
				{{"type", "ruler"}, {"timeFormat", format}, {"timeOffset", 4 * time::g_ticksPerQuarter},
					{"timeOffsetOn", on}});
	const Json tracks = app.call("project.get").at("tracks");
	REQUIRE(app.waitUntil([&app] { return elements(app, "ruler.ruler-lane").size() == 10; }));
	REQUIRE(app.waitUntil([&app] { return elements(app, "[control=timeOffset]").size() == 10; }));
	app.shell().step(milliseconds(0));
	CHECK(elements(app, "ruler.ruler-lane").at(7).at("attributes").at("format") == "timecode");
	CHECK(elements(app, "[control=timeOffset]").at(1).at("attributes").at("value") == "0:00:02.000");
	test::UiApp::checkGolden("ruler-tracks", app.screenshot("ruler-tracks", {900, 560}));

	const uint64_t first = tracks.at(0).at("id").get<uint64_t>();
	const auto track = [&app] { return app.call("project.get").at("tracks").at(0); };
	REQUIRE(track().at("id") == first);

	// Time Offset On/Off.
	click(app, "[control=timeOffsetOn]");
	REQUIRE(app.waitUntil([&track] { return track().at("timeOffsetOn") == true; }));

	// Time Format: the drop-down list's next format, seconds.
	click(app, "[control=timeFormat]");
	app.ui().key(std::nullopt, "Down");
	click(app, "[control=timeFormat]");
	REQUIRE(app.waitUntil([&track] { return track().at("timeFormat") == "seconds"; }));

	// Set Time Offset to Project Cursor Position: 1.5 seconds, three beats.
	app.call("transport.locate", {{"position", 72000}});
	click(app, "[control=setTimeOffset]");
	REQUIRE(app.waitUntil([&track] { return track().at("timeOffset") == 3 * time::g_ticksPerQuarter; }));

	// Time Offset, typed in the track's time format: a second, two beats; text that isn't a time changes nothing.
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "[control=timeOffset]").at(0).at("attributes").at("value") == "0:00:01.500"; }));
	click(app, "[control=timeOffset]", 0, 2);
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "1");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&track] { return track().at("timeOffset") == 2 * time::g_ticksPerQuarter; }));
	click(app, "[control=timeOffset]", 0, 2);
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "x");
	app.ui().key(std::nullopt, "Enter");
	app.shell().step(milliseconds(0));
	CHECK(track().at("timeOffset") == 2 * time::g_ticksPerQuarter);
	REQUIRE(app.waitUntil(
		[&app] { return elements(app, "[control=timeOffset]").at(0).at("attributes").at("value") == "0:00:01.000"; }));
}
