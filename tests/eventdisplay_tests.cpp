// SPDX-License-Identifier: GPL-3.0-or-later
// The event display (#84) on the fake window system, following a real engine: events and the project cursor where
// the engine has them, zoom, its history and presets, scrolling with the wheel, the ruler and the overview line,
// auto-scroll and the cross-hair cursor. tests/uiapp_tests.cpp checks it in the app's own window through SDL_GPU.
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	ui::Target point(double x, double y)
	{
		return {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	}

	// The event display filling a 1000 by 400 window of the fake UI at a monitor scale of 1, so that a dp is a pixel,
	// following an engine on the dummy device.
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

		// Runs an engine command from another client, as MCP or Lua would; it must succeed.
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = engine::Client(engine).call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		void step() { ui.shell.step(milliseconds(0)); }

		// Steps the shell until done() holds; false after five seconds.
		bool waitUntil(const std::function<bool()>& done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!done())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				ui.shell.step(milliseconds(5));
			}
			step(); // lays the changes out
			return true;
		}

		Rml::Element& element(const char* selector) const
		{
			Rml::Element* found = document.QuerySelector(selector);
			REQUIRE(found != nullptr);
			return *found;
		}

		const std::vector<ui::EventLanes::Lane>& lanes() const
		{
			auto* events = dynamic_cast<ui::EventLanes*>(&element("event-lanes.events"));
			REQUIRE(events != nullptr);
			return events->lanes();
		}

		// Where an element's left edge is, in dp from the event lanes' left edge.
		double left(const char* selector) const
		{
			return element(selector).GetAbsoluteLeft() - element("event-lanes.events").GetAbsoluteLeft();
		}

		// The middle of the ruler's lower half, at x dp from its left.
		ui::Target ruler(double x) const
		{
			Rml::Element& ruler = element("ruler");
			return point(ruler.GetAbsoluteLeft() + x, ruler.GetAbsoluteTop() + (ruler.GetClientHeight() * 0.75));
		}

		// A point over the event lanes, x dp from their left and y dp from their top.
		ui::Target lanesAt(double x, double y) const
		{
			Rml::Element& lanes = element("event-lanes.events");
			return point(lanes.GetAbsoluteLeft() + x, lanes.GetAbsoluteTop() + y);
		}

		// Adds an audio track with an event from 2 to 2.5 seconds and a MIDI track with a part from 1 to 3 seconds
		// (two quarters at 120 BPM, four long), and waits for the display to show them.
		void addEvents()
		{
			const Json audio = call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
			const Json midi = call("track.add", {{"type", "midi"}}).at("track");
			const Json media =
				call("media.add", {{"path", "take.wav"}, {"sampleRate", 44100}, {"channels", 1}, {"length", 441000}})
					.at("media");
			const Json clip = call("clip.add", {{"media", media.at("id")}}).at("clip");
			call("audioEvent.add",
				{{"track", audio.at("id")}, {"clip", clip.at("id")}, {"position", 96000}, {"length", 22050}});
			call("midiPart.add", {{"track", midi.at("id")}, {"position", 2 * g_quarter}, {"length", 4 * g_quarter}});
			REQUIRE(waitUntil([this] { return lanes().size() == 2 && !lanes()[1].events.empty(); }));
		}

		void locate(int64_t sample)
		{
			call("transport.locate", {{"position", sample}});
			REQUIRE(waitUntil([this, sample] { return display.cursor() == Approx(sample / 48000.0); }));
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::Automation automation{ui.shell};
	};

	bool same(const ui::TimelineView& a, const ui::TimelineView& b)
	{
		return a.start == Approx(b.start).margin(1e-9) && a.scale == Approx(b.scale) && a.top == Approx(b.top) &&
			a.trackZoom == Approx(b.trackZoom);
	}
}

TEST_CASE("the event display shows events and the project cursor where the engine has them", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	fixture.locate(72000);

	// One lane per track, in the project's order; events at their time in seconds.
	const std::vector<ui::EventLanes::Lane>& lanes = fixture.lanes();
	CHECK(lanes[0].height == Approx(ui::g_trackHeight));
	REQUIRE(lanes[0].events.size() == 1);
	CHECK(lanes[0].events[0].start == Approx(2.0));
	CHECK(lanes[0].events[0].length == Approx(0.5)); // the clip's frames at its own rate
	REQUIRE(lanes[1].events.size() == 1);
	CHECK(lanes[1].events[0].start == Approx(1.0));
	CHECK(lanes[1].events[0].length == Approx(2.0));

	// The ruler, the lanes and the overview share the timeline; the attributes show the view to the UI tools.
	const ui::TimelineView& view = fixture.display.view();
	CHECK(view.start == 0.0);
	CHECK(view.scale == Approx(0.01));
	for (const char* selector : {"ruler", "event-lanes.events"})
	{
		INFO(selector);
		CHECK(fixture.element(selector).GetAttribute<double>("start", -1.0) == view.start);
		CHECK(fixture.element(selector).GetAttribute<double>("scale", -1.0) == view.scale);
	}
	CHECK(fixture.element("ruler").GetAttribute<std::string>("format", "") == "barsBeats");

	// The project cursor at 1.5 seconds: 150 dp at 0.01 seconds per dp, over the ruler and the lanes.
	CHECK(fixture.display.cursor() == Approx(1.5));
	CHECK(fixture.left(".project-cursor") == Approx(150.0).margin(0.5));
	CHECK(fixture.element(".project-cursor").GetAbsoluteTop() <= fixture.element("ruler").GetAbsoluteTop());

	// Changes from any client follow, also undo.
	fixture.call("history.undo");
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.lanes()[1].events.empty(); }));
	fixture.call("transport.locate", {{"position", 480000}}); // 10 s: right of the view
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.cursor() == Approx(10.0); }));
	CHECK(fixture.left(".project-cursor") == Approx(1000.0).margin(0.5));
}

namespace
{
	// The <waveform> elements over the audio events (#83).
	std::vector<ui::Waveform*> waveforms(Rml::ElementDocument& document)
	{
		Rml::ElementList found;
		document.QuerySelectorAll(found, ".waveforms waveform");
		std::vector<ui::Waveform*> list;
		for (Rml::Element* element : found)
			list.push_back(dynamic_cast<ui::Waveform*>(element));
		return list;
	}

	// The columns of media.peaks, as a waveform takes them.
	std::vector<ui::PeakColumns> columnsOf(const Json& peaks)
	{
		std::vector<ui::PeakColumns> channels;
		for (const Json& channel : peaks.at("channels"))
			channels.push_back(
				{channel.at("min").get<std::vector<int16_t>>(), channel.at("max").get<std::vector<int16_t>>()});
		return channels;
	}

	bool same(const std::vector<ui::PeakColumns>& a, const std::vector<ui::PeakColumns>& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (a[i].low != b[i].low || a[i].high != b[i].high)
				return false;
		return true;
	}
}

TEST_CASE("audio events show their waveforms from the engine's waveform overviews", "[unit]")
{
	// Two seconds of stereo audio: a loud sine on the left in the first second, a quiet one on the right throughout.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	test::Audio audio;
	audio.channels = 2;
	for (int frame = 0; frame < 96000; ++frame)
	{
		const auto sine = static_cast<float>(std::sin(frame / 50.0));
		audio.samples.push_back(frame < 48000 ? 0.9F * sine : 0.0F);
		audio.samples.push_back(0.2F * sine);
	}
	test::writeWav(file, audio);

	Fixture fixture;
	const Json track = fixture.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
	const Json media = fixture.call("media.add", {{"path", file.u8string()}}).at("media");
	const Json clip = fixture.call("clip.add", {{"media", media.at("id")}}).at("clip");
	// From 1 to 2 seconds, playing the file from half a second: half of it loud on the left.
	fixture.call("audioEvent.add",
		{{"track", track.at("id")}, {"clip", clip.at("id")}, {"position", 48000}, {"offset", 24000},
			{"length", 48000}});

	// At 0.01 seconds per dp the event is 100 dp wide: a column per pixel, as media.peaks has them once they're ready.
	engine::Client client(fixture.engine);
	const auto peaks = [&](int64_t pixels)
	{
		Json result;
		REQUIRE(fixture.waitUntil(
			[&]
			{
				result = client
							 .call("media.peaks",
								 {{"media", media.at("id")}, {"start", 24000}, {"frames", 48000}, {"pixels", pixels}})
							 .value;
				return result.at("status") == "ready";
			}));
		return columnsOf(result);
	};
	const std::vector<ui::PeakColumns> expected = peaks(100);
	REQUIRE(fixture.waitUntil(
		[&]
		{
			return waveforms(fixture.document).size() == 1 &&
				same(waveforms(fixture.document)[0]->channels(), expected);
		}));
	ui::Waveform& wave = *waveforms(fixture.document)[0];
	CHECK(wave.span().start == Approx(1.0));
	CHECK(wave.span().length == Approx(1.0));
	CHECK(wave.GetAttribute<int64_t>("media", 0) == media.at("id").get<int64_t>());
	// Over the event's lane, on the same timeline.
	for (const char* attribute : {"start", "scale"})
		CHECK(wave.GetAttribute<double>(attribute, -1.0) ==
			fixture.element("event-lanes.events").GetAttribute<double>(attribute, -2.0));
	CHECK(wave.GetAbsoluteTop() == Approx(fixture.element("event-lanes.events").GetAbsoluteTop()));
	CHECK(wave.GetBox().GetSize(Rml::BoxArea::Border).y == Approx(ui::g_trackHeight));
	CHECK(expected[0].high[10] > 20000); // loud
	CHECK(expected[0].high[90] == 0);    // silent
	CHECK(expected[1].high[90] > 5000);

	// Zooming in asks for more columns: 2000 pixels for the second the event lasts.
	fixture.display.zoomTo(1.0, 1.5);
	const std::vector<ui::PeakColumns> fine = peaks(2000);
	REQUIRE(fixture.waitUntil([&] { return same(wave.channels(), fine); }));
	CHECK(wave.span().start == Approx(1.0));
	CHECK(wave.GetAttribute<double>("scale", 0.0) == Approx(0.0005));

	// Scrolled far away, the event has no waveform element; scrolled back, it has one again.
	fixture.display.scrollTo(100.0);
	fixture.step();
	CHECK(waveforms(fixture.document).empty());
	fixture.display.scrollTo(1.0);
	REQUIRE(fixture.waitUntil([&]
		{ return waveforms(fixture.document).size() == 1 && same(waveforms(fixture.document)[0]->channels(), fine); }));

	// Removing the event removes its waveform, through any client.
	fixture.call("history.undo");
	REQUIRE(fixture.waitUntil([&] { return waveforms(fixture.document).empty(); }));
}

TEST_CASE("the waveform of a one-hour file draws at once when its peak file exists", "[integration][slow]")
{
#ifdef GNUBASE_SANITIZED
	SKIP("sanitizer builds are too slow to measure this");
#endif
	// One hour of a mono 100 Hz sine at 48 kHz, 16 bits: 345 MB, never committed.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "hour.wav";
	constexpr int64_t hour = 3600LL * 48000;
	{
		engine::WriteOptions options;
		options.channels = 1;
		options.sampleFormat = engine::SampleFormat::Int16;
		const auto writer = engine::AudioFileWriter::create(file.u8string(), options);
		std::vector<float> second(48000);
		for (size_t i = 0; i < second.size(); ++i)
			second[i] =
				static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 100.0 * static_cast<double>(i) / 48000.0));
		for (int i = 0; i < 3600; ++i)
			writer->write(second.data(), second.size());
		writer->close();
	}
	{
		// The first time, the engine generates the peaks and writes the peak file.
		engine::Engine engine;
		engine::Client client(engine);
		REQUIRE(client.call("media.add", {{"path", file.u8string()}}).ok());
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
		const auto ready = [&client]
		{ return client.call("media.peakStatus").value.at("media").at(0).at("status") == "ready"; };
		while (!ready() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(milliseconds(100));
		REQUIRE(ready());
	}

	// The next time, the whole hour draws as soon as the event is added.
	Fixture fixture;
	const Json track = fixture.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
	const auto before = std::chrono::steady_clock::now();
	const Json media = fixture.call("media.add", {{"path", file.u8string()}}).at("media");
	const Json clip = fixture.call("clip.add", {{"media", media.at("id")}}).at("clip");
	fixture.call(
		"audioEvent.add", {{"track", track.at("id")}, {"clip", clip.at("id")}, {"position", 0}, {"length", hour}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.projectLength() == Approx(3600.0); }));
	fixture.display.zoomFull();
	const auto drawn = [&fixture]
	{
		const std::vector<ui::Waveform*> list = waveforms(fixture.document);
		if (list.size() != 1 || list[0]->channels().empty() || list[0]->span().length != Approx(3600.0))
			return false;
		const std::vector<int16_t>& high = list[0]->channels()[0].high;
		return !high.empty() && std::all_of(high.begin(), high.end(), [](int16_t peak) { return peak > 10000; });
	};
	REQUIRE(fixture.waitUntil(drawn));
	const auto took = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - before);
	WARN("the waveform of a one-hour file drew after " << took.count() << " ms");
	CHECK(took < std::chrono::seconds(2));
	CHECK(waveforms(fixture.document)[0]->channels()[0].high.size() == 1000); // a column per pixel
}

TEST_CASE("zooming in and out keeps the project cursor in place, and zoom steps can be undone", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	fixture.locate(72000); // 1.5 s, 150 dp from the left
	const ui::TimelineView start = fixture.display.view();
	CHECK(fixture.display.visibleLength() == Approx(10.0));

	fixture.display.zoomIn();
	ui::TimelineView in = start;
	in.scale = 0.005;
	in.start = 1.5 - (150.0 * 0.005);
	CHECK(same(fixture.display.view(), in));
	fixture.step();
	CHECK(fixture.left(".project-cursor") == Approx(150.0).margin(0.5));
	fixture.display.zoomOut();
	CHECK(same(fixture.display.view(), start));

	// A cursor outside the view goes to the middle.
	fixture.locate(int64_t{48000} * 60);
	fixture.display.zoomIn();
	CHECK(fixture.display.view().start == Approx(60.0 - (500.0 * 0.005)));

	// Zoom history.
	CHECK(fixture.display.undoZoom());
	CHECK(same(fixture.display.view(), start));
	CHECK(fixture.display.undoZoom());
	CHECK(same(fixture.display.view(), in));
	CHECK(fixture.display.undoZoom());
	CHECK(same(fixture.display.view(), start));
	CHECK(!fixture.display.undoZoom());
	CHECK(fixture.display.redoZoom());
	CHECK(same(fixture.display.view(), in));
	// A new zoom step drops what could be redone.
	fixture.display.zoomOut();
	CHECK(!fixture.display.redoZoom());

	// Zoom full: from the project start to its end, but at least a minute (the project length comes with #78).
	fixture.display.zoomFull();
	CHECK(fixture.display.view().start == 0.0);
	CHECK(fixture.display.visibleLength() == Approx(ui::g_minimumProjectLength));
	fixture.call("transport.locate", {{"position", 0}});
	fixture.call("midiPart.set",
		{{"part", fixture.call("project.get").at("midiParts")[0].at("id")},
			{"position", 400 * g_quarter}}); // 200 s to 202 s
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.lanes()[1].events.at(0).start == Approx(200.0); }));
	fixture.display.zoomFull();
	CHECK(fixture.display.visibleLength() == Approx(202.0));

	// Zoom to a stretch, such as a cycle marker's.
	fixture.display.zoomTo(4.0, 6.0);
	CHECK(fixture.display.view().start == Approx(4.0));
	CHECK(fixture.display.visibleLength() == Approx(2.0));
}

TEST_CASE("vertical zoom changes the track heights and goes into the zoom history", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	fixture.display.zoomInVertically();
	CHECK(fixture.display.view().trackZoom > 1.0);
	REQUIRE(fixture.waitUntil([&fixture]
		{ return fixture.lanes()[0].height == Approx(ui::g_trackHeight * fixture.display.view().trackZoom); }));
	fixture.display.zoomOutVertically();
	fixture.display.zoomOutVertically();
	CHECK(fixture.display.view().trackZoom < 1.0);
	CHECK(fixture.display.undoZoom());
	CHECK(fixture.display.undoZoom());
	CHECK(fixture.display.view().trackZoom > 1.0);
}

TEST_CASE("the mouse wheel scrolls the event display, and zooms it with Ctrl", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	for (int i = 0; i < 10; ++i) // taller than the view, to scroll down
		fixture.call("track.add", {{"type", "audio"}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.lanes().size() == 12; }));

	// Down, then right with Shift.
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(500, 50), 0.0F, 1.0F, {});
	CHECK(fixture.display.view().top > 0.0);
	CHECK(fixture.element("event-lanes.events").GetAttribute<double>("scroll-top", 0.0) == fixture.display.view().top);
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(500, 50), 0.0F, -10.0F, {});
	CHECK(fixture.display.view().top == 0.0); // not above the first track
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(500, 50), 0.0F, 2.0F, {"shift"});
	const double scrolled = fixture.display.view().start;
	CHECK(scrolled > 0.0);
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(500, 50), 0.0F, -100.0F, {"shift"});
	CHECK(fixture.display.view().start == 0.0); // not before the project start

	// Ctrl zooms horizontally around the mouse: the time under it stays there.
	fixture.display.scrollTo(3.0);
	const double under = 3.0 + (250.0 * fixture.display.view().scale);
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(250, 50), 0.0F, -1.0F, {"ctrl"});
	CHECK(fixture.display.view().scale < 0.01);
	CHECK(fixture.display.view().start + (250.0 * fixture.display.view().scale) == Approx(under));
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(250, 50), 0.0F, 1.0F, {"ctrl"});
	CHECK(fixture.display.view().scale == Approx(0.01));
	// Ctrl and Shift zoom vertically.
	const double tracks = fixture.display.view().trackZoom;
	fixture.automation.wheel(std::nullopt, fixture.lanesAt(250, 50), 0.0F, -1.0F, {"ctrl", "shift"});
	CHECK(fixture.display.view().trackZoom > tracks);
}

TEST_CASE("a click in the ruler moves the project cursor, and dragging down or up zooms", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	fixture.automation.click(std::nullopt, fixture.ruler(300), ui::MouseButton::Left, 1, {});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.cursor() == Approx(3.0); }));
	CHECK(fixture.call("transport.get").at("position") == 3 * 48000);

	// Dragging down zooms in around where the drag started, dragging up zooms out.
	const ui::Target from = fixture.ruler(300);
	const ui::Point start = from.point.value_or(ui::Point{});
	fixture.automation.drag(std::nullopt, from, point(start.x, start.y + 60));
	const ui::TimelineView in = fixture.display.view();
	CHECK(in.scale < 0.01);
	CHECK(in.start + (300.0 * in.scale) == Approx(3.0));
	fixture.automation.drag(std::nullopt, from, point(start.x, start.y - 60));
	CHECK(fixture.display.view().scale > in.scale);
	// One zoom step each in the history.
	CHECK(fixture.display.undoZoom());
	CHECK(same(fixture.display.view(), in));
}

TEST_CASE("the overview line shows the whole project and moves and resizes the view", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	// The project is a minute long, the view shows its first 10 seconds: a sixth of the overview.
	Rml::Element& overview = fixture.element(".overview");
	Rml::Element& rectangle = fixture.element(".overview-view");
	const double width = overview.GetClientWidth();
	CHECK(rectangle.GetAbsoluteLeft() - overview.GetAbsoluteLeft() == Approx(0.0).margin(0.5));
	CHECK(rectangle.GetOffsetWidth() == Approx(width / 6.0).margin(1.0));
	auto* events = dynamic_cast<ui::EventLanes*>(&fixture.element(".overview event-lanes"));
	REQUIRE(events != nullptr);
	CHECK(events->lanes().size() == 1); // all tracks in one row
	CHECK(events->lanes()[0].events.size() == 2);

	// Dragging the rectangle scrolls.
	const double y = overview.GetAbsoluteTop() + (overview.GetClientHeight() / 2.0);
	const double middle = rectangle.GetAbsoluteLeft() + (rectangle.GetOffsetWidth() / 2.0);
	fixture.automation.drag(std::nullopt, point(middle, y), point(middle + (width / 12.0), y));
	CHECK(fixture.display.view().start == Approx(5.0).margin(0.2));
	CHECK(fixture.display.visibleLength() == Approx(10.0));
	fixture.step();

	// A click outside it centres the view there.
	fixture.automation.click(
		std::nullopt, point(overview.GetAbsoluteLeft() + (width / 2.0), y), ui::MouseButton::Left, 1, {});
	CHECK(fixture.display.view().start == Approx(25.0).margin(0.2));
	fixture.step();

	// Dragging its right edge zooms.
	const double right = rectangle.GetAbsoluteLeft() + rectangle.GetOffsetWidth() - 1.0;
	fixture.automation.drag(std::nullopt, point(right, y), point(right + (width / 6.0), y));
	CHECK(fixture.display.view().start == Approx(25.0).margin(0.2));
	CHECK(fixture.display.visibleLength() == Approx(20.0).margin(0.5));
	CHECK(fixture.display.undoZoom());
	CHECK(fixture.display.visibleLength() == Approx(10.0));
}

TEST_CASE("auto-scroll keeps the project cursor in view while playing", "[integration]")
{
	Fixture fixture;
	fixture.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	fixture.call("audio.start");
	fixture.display.zoomTo(0.0, 4.0);
	fixture.display.setAutoScroll(true);
	CHECK(fixture.display.autoScrollMode() == ui::AutoScrollMode::Page);

	// Page: the view turns a page when the cursor leaves it.
	fixture.call("transport.play");
	fixture.call("transport.locate", {{"position", 48000 * 9}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.view().start >= 9.0; }));
	CHECK(fixture.display.view().start < 10.0);

	// Stationary: the cursor stays in the middle.
	fixture.display.setAutoScrollMode(ui::AutoScrollMode::Stationary);
	fixture.call("transport.locate", {{"position", 48000 * 30}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.view().start >= 28.0; }));
	CHECK(fixture.display.cursor() - fixture.display.view().start == Approx(2.0).margin(0.2));

	// Suspended while editing: a click in the event display during playback stops it until playback stops.
	fixture.display.setSuspendAutoScrollWhenEditing(true);
	fixture.automation.click(std::nullopt, fixture.lanesAt(100, 10), ui::MouseButton::Left, 1, {});
	CHECK(fixture.display.autoScrollSuspended());
	const double suspended = fixture.display.view().start;
	fixture.call("transport.locate", {{"position", 48000 * 50}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.cursor() >= 50.0; }));
	CHECK(fixture.display.view().start == suspended);
	fixture.call("transport.stop");
	REQUIRE(fixture.waitUntil([&fixture] { return !fixture.display.autoScrollSuspended(); }));

	// Off: the view stays.
	fixture.display.setAutoScroll(false);
	fixture.call("transport.play");
	fixture.call("transport.locate", {{"position", 48000 * 100}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.cursor() >= 100.0; }));
	CHECK(fixture.display.view().start < 100.0);
	fixture.call("transport.stop");
	fixture.call("audio.stop");
}

TEST_CASE("horizontal zoom presets are user settings for every project", "[unit]")
{
	Fixture fixture;
	fixture.display.zoomTo(0.0, 4.0);
	fixture.display.addZoomPreset("Detail");
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.zoomPresets().size() == 1; }));
	CHECK(fixture.call("settings.get", {{"key", "ui.zoomPresets"}}).at("value") ==
		Json::array({{{"name", "Detail"}, {"seconds", 4.0}}}));

	fixture.display.zoomOut();
	fixture.display.scrollTo(7.0);
	CHECK(fixture.display.applyZoomPreset("Detail"));
	CHECK(fixture.display.visibleLength() == Approx(4.0));
	CHECK(fixture.display.view().start == Approx(7.0)); // keeps the left edge
	CHECK(!fixture.display.applyZoomPreset("Nothing"));

	// Presets set from elsewhere, e.g. through MCP, apply as well.
	fixture.call("settings.set",
		{{"key", "ui.zoomPresets"},
			{"value",
				Json::array({{{"name", "Detail"}, {"seconds", 4.0}}, {{"name", "Overview"}, {"seconds", 120.0}}})}});
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.zoomPresets().size() == 2; }));
	CHECK(fixture.display.applyZoomPreset("Overview"));
	CHECK(fixture.display.visibleLength() == Approx(120.0));

	fixture.display.renameZoomPreset("Overview", "Song");
	fixture.display.removeZoomPreset("Detail");
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.display.zoomPresets().size() == 1; }));
	CHECK(fixture.display.zoomPresets()[0].at("name") == "Song");
}

TEST_CASE("the cross-hair cursor follows the mouse over the event lanes", "[unit]")
{
	Fixture fixture;
	fixture.addEvents();
	fixture.automation.move(std::nullopt, fixture.lanesAt(321, 45));
	fixture.step();
	Rml::Element& lanes = fixture.element("event-lanes.events");
	Rml::Element& vertical = fixture.element(".cross-hair-vertical");
	Rml::Element& horizontal = fixture.element(".cross-hair-horizontal");
	CHECK(vertical.IsVisible());
	CHECK(vertical.GetAbsoluteLeft() - lanes.GetAbsoluteLeft() == Approx(321.0).margin(0.5));
	CHECK(horizontal.GetAbsoluteTop() - lanes.GetAbsoluteTop() == Approx(45.0).margin(0.5));

	// Gone outside the lanes, and when switched off.
	fixture.automation.move(std::nullopt, fixture.ruler(10));
	fixture.step();
	CHECK(!vertical.IsVisible());
	fixture.display.setCrossHair(false);
	fixture.automation.move(std::nullopt, fixture.lanesAt(321, 45));
	fixture.step();
	CHECK(!vertical.IsVisible());
}

TEST_CASE("the ruler shows the chosen time format", "[unit]")
{
	Fixture fixture;
	CHECK(fixture.display.rulerFormat() == ui::TimeFormat::BarsBeats);
	fixture.display.setRulerFormat(ui::TimeFormat::Timecode);
	CHECK(fixture.display.rulerFormat() == ui::TimeFormat::Timecode);
	CHECK(fixture.element("ruler").GetAttribute<std::string>("format", "") == "timecode");
}

TEST_CASE("the event display's actions are commands by name, for key commands", "[unit]")
{
	Fixture fixture;
	const std::map<std::string, std::function<void()>>& actions = fixture.display.actions();
	for (const char* id : {"zoom.in", "zoom.out", "zoom.inVertically", "zoom.outVertically", "zoom.full", "zoom.undo",
			 "zoom.redo", "view.autoScroll"})
		CHECK(actions.count(id) == 1);
	actions.at("zoom.in")();
	CHECK(fixture.display.view().scale == Approx(0.005));
	actions.at("zoom.undo")();
	CHECK(fixture.display.view().scale == Approx(0.01));
	actions.at("view.autoScroll")();
	CHECK(fixture.display.autoScroll());
	fixture.step();
	CHECK(fixture.element("event-lanes.events").GetAttribute<double>("scale", 0.0) == Approx(0.01));
}
