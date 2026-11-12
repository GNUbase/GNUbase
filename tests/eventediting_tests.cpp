// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the event display (#85) on the fake window system, following a real engine: selecting,
// moving, copying, resizing and splitting with the mouse, adding parts with a double click, and the edit commands on
// the selection; snapping them (#87). tests/eventedit_tests.cpp checks the event.* commands themselves.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	ui::Target point(double x, double y)
	{
		return {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	}

	// The event display filling a 1000 by 400 window at a dp ratio of 1, at 0.01 seconds per dp, with an audio event
	// from 2 to 2.5 seconds on a linear audio track (lane 0, 0 to 40 dp down) and a part from 1 to 3 seconds on a
	// MIDI track (lane 1).
	struct Fixture
	{
		Fixture()
		{
			// GNUbase's mouse pointers, as the app has them: a name the window system shows is one of them.
			ui.rmlui.loadCursors(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "cursors");
			ui.shell.eachStep([this] { return binding.update(); });
			step();
			const Json audio = call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
			const Json midi = call("track.add", {{"type", "midi"}}).at("track");
			const Json media =
				call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
					.at("media");
			const Json clip = call("clip.add", {{"media", media.at("id")}}).at("clip");
			event = call("audioEvent.add",
				{{"track", audio.at("id")}, {"clip", clip.at("id")}, {"position", 96000}, {"length", 24000}})
						.at("event")
						.at("id")
						.get<uint64_t>();
			part =
				call("midiPart.add", {{"track", midi.at("id")}, {"position", 2 * g_quarter}, {"length", 4 * g_quarter}})
					.at("part")
					.at("id")
					.get<uint64_t>();
			REQUIRE(waitUntil([this] { return lanes("events").size() == 2 && !lanes("events")[1].events.empty(); }));
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

		const std::vector<ui::EventLanes::Lane>& lanes(const std::string& kind) const
		{
			auto* found = dynamic_cast<ui::EventLanes*>(document.QuerySelector("event-lanes." + kind));
			REQUIRE(found != nullptr);
			return found->lanes();
		}

		// A point over the event lanes, x dp from their left and y dp from their top.
		ui::Target at(double x, double y) const
		{
			Rml::Element* lanes = document.QuerySelector("event-lanes.events");
			return point(lanes->GetAbsoluteLeft() + x, lanes->GetAbsoluteTop() + y);
		}

		void click(double x, double y, const std::vector<std::string>& modifiers = {}, int count = 1)
		{
			automation.click(std::nullopt, at(x, y), ui::MouseButton::Left, count, modifiers);
			step();
		}

		void drag(double x, double y, double toX, double toY, const std::vector<std::string>& modifiers = {})
		{
			automation.drag(std::nullopt, at(x, y), at(toX, toY), modifiers);
			step();
		}

		Json parts() { return call("project.get").at("midiParts"); }
		Json audioEvents() { return call("project.get").at("audioEvents"); }
		Json audioParts() { return call("project.get").at("audioParts"); }

		void run(const char* action)
		{
			display.actions().at(action)();
			step();
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::Automation automation{ui.shell};
		uint64_t event = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("clicks select events, a drag moves them, Alt and a drag copies them", "[unit]")
{
	Fixture f;
	// A click selects; the selected event shows over the others, and the root names it.
	f.click(150, 60);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	REQUIRE(f.lanes("selected-events")[1].events.size() == 1);
	CHECK(f.lanes("selected-events")[0].events.empty());
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("selection", "") == std::to_string(f.part));
	// Ctrl adds to it, a click elsewhere selects nothing.
	f.click(225, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event, f.part});
	f.click(225, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	f.click(700, 60);
	CHECK(f.display.selection().empty());

	// Dragging the part 100 dp to the right moves it a second later: two quarters.
	f.drag(150, 60, 250, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});

	// Alt and a drag copies it two seconds later, and selects the copy.
	f.drag(250, 60, 450, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 8 * g_quarter);
	CHECK(f.parts()[1].at("shared") == 0);
	REQUIRE(f.waitUntil([&f] { return f.display.selection().size() == 1 && f.display.selection()[0] != f.part; }));

	// With Shift too, a shared copy; a drag down a lane moves it onto the track there.
	f.drag(450, 60, 650, 60, {"alt", "shift"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.parts()[2].at("shared") != 0);
	const Json midi = f.call("track.add", {{"type", "midi"}}).at("track");
	REQUIRE(f.waitUntil([&f] { return f.lanes("events").size() == 3; }));
	f.drag(750, 60, 750, 100);
	REQUIRE(f.waitUntil([&f, &midi] { return f.parts()[2].at("track") == midi.at("id"); }));
	CHECK(f.parts()[2].at("position") == 12 * g_quarter);
}

TEST_CASE("nudging and moving to the cursor stop the selection at the project start", "[unit]")
{
	Fixture f;
	const auto steps = [&] { return f.call("history.get").at("steps").size(); };
	SECTION("a musical nudge is limited in ticks")
	{
		f.call("snap.set", {{"grid", "bar"}});
		REQUIRE(f.waitUntil([&] { return f.display.snap().grid() == engine::GridType::Bar; }));
		f.display.select({f.part});
		const size_t before = steps();
		f.run("edit.nudgeLeft");
		REQUIRE(f.waitUntil([&] { return f.parts()[0].at("position") == 0; }));
		CHECK(steps() == before + 1);
		f.call("history.undo");
		CHECK(f.parts()[0].at("position") == 2 * g_quarter);
	}
	SECTION("a linear nudge is limited in samples")
	{
		f.call("snap.set", {{"grid", "bar"}});
		f.call("audioEvent.set", {{"event", f.event}, {"position", 12000}});
		REQUIRE(f.waitUntil(
			[&]
			{
				return f.display.project().at("audioEvents")[0].at("position") == 12000 &&
					f.display.snap().grid() == engine::GridType::Bar;
			}));
		f.display.select({f.event});
		const size_t before = steps();
		f.run("edit.nudgeLeft");
		REQUIRE(f.waitUntil([&] { return f.audioEvents()[0].at("position") == 0; }));
		CHECK(steps() == before + 1);
	}
	SECTION("moving the end to an earlier cursor keeps the event at zero")
	{
		f.call("transport.locate", {{"position", 4800}});
		REQUIRE(f.waitUntil([&] { return f.display.cursor() == 0.1; }));
		f.display.select({f.event});
		const size_t before = steps();
		f.run("edit.moveEndToCursor");
		REQUIRE(f.waitUntil([&] { return f.audioEvents()[0].at("position") == 0; }));
		CHECK(f.audioEvents()[0].at("length") == 24000);
		CHECK(steps() == before + 1);
	}
}

TEST_CASE("mixed selections keep musical nudge steps across tempo changes", "[unit]")
{
	Fixture f;
	f.call("project.set", {{"tempoTrackActive", true}});
	f.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 60.0}});
	f.call("audioEvent.set", {{"event", f.event}, {"position", 24000}});
	f.call("midiPart.set", {{"part", f.part}, {"position", 8 * g_quarter}});
	f.call("snap.set", {{"grid", "bar"}});
	REQUIRE(f.waitUntil(
		[&]
		{
			const Json& project = f.display.project();
			return project.at("tempo").size() == 2 && project.at("audioEvents")[0].at("position") == 24000 &&
				project.at("midiParts")[0].at("position") == 8 * g_quarter &&
				f.display.snap().grid() == engine::GridType::Bar;
		}));
	f.display.select({f.event, f.part});
	const Json before = f.call("project.get");
	const size_t steps = f.call("history.get").at("steps").size();
	SECTION("a right nudge moves every event by a full bar in ticks")
	{
		f.run("edit.nudgeRight");
		REQUIRE(f.waitUntil([&] { return f.parts()[0].at("position") != 8 * g_quarter; }));
		CHECK(f.parts()[0].at("position") == 12 * g_quarter);
		CHECK(f.audioEvents()[0].at("position") == 144000);
	}
	SECTION("a left nudge clamps the tick step at the earliest event")
	{
		f.run("edit.nudgeLeft");
		REQUIRE(f.waitUntil([&] { return f.parts()[0].at("position") != 8 * g_quarter; }));
		CHECK(f.parts()[0].at("position") == 7 * g_quarter);
		CHECK(f.audioEvents()[0].at("position") == 0);
	}
	CHECK(f.call("history.get").at("steps").size() == steps + 1);
	f.call("history.undo");
	CHECK(f.call("project.get") == before);
}

TEST_CASE("clamped musical previews apply the same tick offset across tempo changes", "[unit]")
{
	Fixture f;
	f.call("project.set", {{"tempoTrackActive", true}});
	f.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 60.0}});
	const auto later =
		f.call("midiPart.add",
			 {{"track", f.parts()[0].at("track")}, {"position", 8 * g_quarter}, {"length", 4 * g_quarter}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	f.call("snap.set", {{"on", true}, {"type", "grid"}, {"grid", "bar"}});
	REQUIRE(f.waitUntil(
		[&]
		{
			return f.lanes("events")[1].events.size() == 2 && f.lanes("events")[1].events[1].start == 6.0 &&
				f.display.snap().on;
		}));
	f.display.select({f.part, later});
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });
	f.automation.press(std::nullopt, f.at(650, 60), ui::MouseButton::Left);
	clock->advance(1.0);
	f.automation.move(std::nullopt, f.at(50, 60));
	f.step();
	const auto& preview = f.lanes("selected-events")[1].events;
	REQUIRE(preview.size() == 2);
	CHECK(preview[0].start == 0.0);
	CHECK(preview[0].length == 2.0);
	CHECK(preview[1].start == 4.0);
	CHECK(preview[1].length == 4.0);
	f.automation.release(std::nullopt, f.at(50, 60), ui::MouseButton::Left);
	REQUIRE(
		f.waitUntil([&] { return f.parts()[0].at("position") == 0 && f.parts()[1].at("position") == 6 * g_quarter; }));
}

TEST_CASE("a drag at an edge resizes, Alt and a click splits, a double click adds a part", "[unit]")
{
	Fixture f;
	// The part's left edge half a second earlier: it starts a quarter sooner.
	f.drag(102, 60, 52, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == g_quarter; }));
	CHECK(f.parts()[0].at("length") == 5 * g_quarter);
	// The audio event's right edge 20 dp further: a fifth of a second longer.
	f.drag(248, 20, 268, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 24000 + 9600; }));

	// Alt and a click at 2 seconds splits the part there.
	f.click(200, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 4 * g_quarter);

	// A double click on an empty MIDI lane at 7 seconds adds the bar from 6 to 8 seconds, and selects it.
	f.click(700, 60, {}, 2);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.parts()[2].at("position") == 12 * g_quarter);
	CHECK(f.parts()[2].at("length") == 4 * g_quarter);
	REQUIRE(f.waitUntil(
		[&f] { return f.display.selection() == std::vector<uint64_t>{f.parts()[2].at("id").get<uint64_t>()}; }));
	// Between the locators, a part from one to the other; on an audio track only there, an audio part (#471).
	f.call("transport.set", {{"left", 408000}, {"right", 456000}}); // 8.5 to 9.5 seconds
	f.step();
	f.click(900, 60, {}, 2);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	CHECK(f.parts()[3].at("position") == 17 * g_quarter);
	CHECK(f.parts()[3].at("length") == 2 * g_quarter);
	f.click(700, 20, {}, 2);
	f.step();
	CHECK(f.audioParts().empty());
	f.click(900, 20, {}, 2);
	REQUIRE(f.waitUntil([&f] { return f.audioParts().size() == 1; }));
	CHECK(f.audioParts()[0].at("position") == 408000);
	CHECK(f.audioParts()[0].at("length") == 48000);
	REQUIRE(f.waitUntil(
		[&f] { return f.display.selection() == std::vector<uint64_t>{f.audioParts()[0].at("id").get<uint64_t>()}; }));
	CHECK(f.audioEvents().size() == 1);
	CHECK(f.parts().size() == 4);
}

namespace
{
	// Whether the display draws the audio event (lane 0) from start to end seconds, playing its clip from offset: its
	// box among the events and the selected events, its waveform, its fade in from its start, its fade out up to its
	// end and its volume line.
	bool drawsAudioEvent(Fixture& f, double start, double end, int64_t offset)
	{
		const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
		const auto boxed = [&f, &near, start, end](const char* kind)
		{
			const std::vector<ui::EventLanes::Lane>& lanes = f.lanes(kind);
			return !lanes.empty() && lanes[0].events.size() == 1 && near(lanes[0].events[0].start, start) &&
				near(lanes[0].events[0].end(), end);
		};
		Rml::ElementList waves;
		f.document.QuerySelectorAll(waves, ".waveforms waveform");
		const auto* wave = waves.size() == 1 ? dynamic_cast<ui::Waveform*>(waves[0]) : nullptr;
		const auto* fadeLines = dynamic_cast<ui::FadeLines*>(f.document.QuerySelector("fade-lines"));
		if (!boxed("events") || !boxed("selected-events") || wave == nullptr || fadeLines == nullptr ||
			fadeLines->lanes().empty())
			return false;
		const ui::FadeLines::Lane& lane = fadeLines->lanes()[0];
		const bool fadesIn = std::any_of(lane.fades.begin(), lane.fades.end(),
			[&near, start](const ui::FadeLine& fade) { return fade.in && near(fade.start, start); });
		const bool fadesOut = std::any_of(lane.fades.begin(), lane.fades.end(),
			[&near, end](const ui::FadeLine& fade) { return !fade.in && near(fade.start + fade.length, end); });
		return near(wave->span().start, start) && near(wave->span().end(), end) && fadesIn && fadesOut &&
			lane.volumes.size() == 1 && near(lane.volumes[0].start, start) && near(lane.volumes[0].end, end) &&
			lane.volumes[0].offset == offset;
	}
}

TEST_CASE("a drag of an event's edge shows the size the release gives it at every step, Escape cancels it", "[unit]")
{
	Fixture f;
	Rml::Context& context = f.window.context();
	Rml::Element* lanes = f.document.QuerySelector("event-lanes.events");
	REQUIRE(lanes != nullptr);
	const auto move = [&context, lanes](double x, double y)
	{
		context.ProcessMouseMove(
			static_cast<int>(lanes->GetAbsoluteLeft() + x), static_cast<int>(lanes->GetAbsoluteTop() + y), 0);
	};
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true); // RmlUi's time stands still but for advance()
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });
	const auto press = [&context, &move, clock](double x, double y)
	{
		clock->advance(1.0); // no double click with the press before
		move(x, y);
		context.ProcessMouseButtonDown(0, 0);
	};
	// The audio event (2 to 2.5 seconds) with fades and a volume, so that its fades and volume line show.
	f.call(
		"audioEvent.setFades", {{"event", f.event}, {"fadeIn", {{"length", 2400}}}, {"fadeOut", {{"length", 4800}}}});
	f.call("audioEvent.set", {{"event", f.event}, {"volume", -6.0}});
	f.display.select({f.event});
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.0, 2.5, 0); }));
	// After a release: the display has the project as the engine has it, and no preview is on its way.
	const auto settled = [&f]
	{
		return f.waitUntil(
			[&f]
			{
				return f.binding.waiting("event.previewResize") == 0 && f.binding.waiting("project.get") == 0 &&
					f.display.project().at("audioEvents") == f.audioEvents() &&
					f.display.project().at("midiParts") == f.parts();
			});
	};

	// Its end, low in the lane where no fade handle is, in steps without letting go: each one shows the new end at
	// once, its fade out and volume line with it.
	press(248, 35);
	for (const double x : {268.0, 300.0, 210.0, 278.0})
	{
		move(x, 35);
		f.step();
		INFO("end at " << x);
		REQUIRE(f.waitUntil([&f, x] { return drawsAudioEvent(f, 2.0, (x + 2.0) / 100.0, 0); }));
	}
	CHECK(f.audioEvents()[0].at("length") == 24000); // nothing changed yet
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 38400; })); // to 2.8 seconds
	CHECK(f.audioEvents()[0].at("position") == 96000);
	CHECK(drawsAudioEvent(f, 2.0, 2.8, 0));
	REQUIRE(settled());
	CHECK(drawsAudioEvent(f, 2.0, 2.8, 0));
	const auto steps = [&f] { return f.call("history.get").at("steps").size(); };
	const size_t resized = steps();

	// Its start: later reveals less of the clip, earlier stops at the clip's start, where the event starts already,
	// as the release would.
	press(202, 35);
	move(222, 35);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.2, 2.8, 9600); }));
	move(150, 35);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.0, 2.8, 0); }));
	move(212, 35);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.1, 2.8, 4800); }));
	// Escape cancels the drag: the event shows its size again, and the release changes nothing.
	f.automation.key(std::nullopt, "Escape");
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.0, 2.8, 0); }));
	move(230, 35);
	REQUIRE(settled());
	CHECK(drawsAudioEvent(f, 2.0, 2.8, 0));
	context.ProcessMouseButtonUp(0, 0);
	REQUIRE(settled());
	CHECK(steps() == resized);
	CHECK(f.audioEvents()[0].at("position") == 96000);
	CHECK(f.audioEvents()[0].at("offset") == 0);

	// Sizing moves contents: the clip moves with the start, earlier as well.
	f.display.setSizingMovesContents(true);
	press(202, 35);
	move(232, 35);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.3, 2.8, 0); }));
	move(152, 35);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 1.5, 2.8, 0); }));
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("position") == 72000; }));
	CHECK(f.audioEvents()[0].at("length") == 62400);
	CHECK(f.audioEvents()[0].at("offset") == 0);
	CHECK(steps() == resized + 1);
	REQUIRE(settled());
	// Its end (2.8 seconds) with the contents: earlier, the clip moves with it; later, it stops where the clip's start
	// reaches the event's.
	struct Step
	{
		double x = 0.0;
		double end = 0.0;
		int64_t offset = 0;
	};
	press(278, 35);
	for (const Step& step :
		{Step{258.0, 2.6, 9600}, Step{298.0, 2.8, 0}, Step{238.0, 2.4, 19200}, Step{258.0, 2.6, 9600}})
	{
		move(step.x, 35);
		f.step();
		INFO("end at " << step.x);
		REQUIRE(f.waitUntil([&f, &step] { return drawsAudioEvent(f, 1.5, step.end, step.offset); }));
	}
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 52800; })); // 1.5 to 2.6 seconds
	CHECK(f.audioEvents()[0].at("position") == 72000);
	CHECK(f.audioEvents()[0].at("offset") == 9600);
	REQUIRE(settled());
	CHECK(drawsAudioEvent(f, 1.5, 2.6, 9600));
	f.call("history.undo");
	REQUIRE(settled());
	REQUIRE(drawsAudioEvent(f, 1.5, 2.8, 0));
	f.display.setSizingMovesContents(false);

	// Snapped to beats, the end (1.5 to 2.8 seconds) at 3.03 seconds goes to 3; Ctrl (Disable Snapping) pressed and
	// released without moving the mouse unsnaps and snaps it again.
	f.call("snap.set", {{"on", true}, {"grid", "beat"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on; }));
	press(278, 35);
	move(301, 35);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 1.5, 3.0, 0); }));
	context.ProcessKeyDown(Rml::Input::KI_LCONTROL, Rml::Input::KM_CTRL);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 1.5, 3.03, 0); }));
	// The event changes meanwhile (another client moves it half a second later): the preview follows, the edge as
	// far from where it was as the drag went.
	f.call("event.move", {{"events", {f.event}}, {"delta", 24000}});
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.0, 3.53, 0); }));
	context.ProcessKeyUp(Rml::Input::KI_LCONTROL, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return drawsAudioEvent(f, 2.0, 3.5, 0); }));
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 72000; })); // 2 to 3.5 seconds
	CHECK(f.audioEvents()[0].at("position") == 96000);
	REQUIRE(settled());
	CHECK(drawsAudioEvent(f, 2.0, 3.5, 0));
	f.call("snap.set", {{"on", false}});
	REQUIRE(f.waitUntil([&f] { return !f.display.snap().on; }));

	// A locked event: the preview fails as the release does, and the event stays as it is.
	f.call("event.set", {{"events", {f.event}}, {"lock", true}});
	REQUIRE(settled());
	press(348, 35);
	move(368, 35);
	REQUIRE(settled());
	CHECK(drawsAudioEvent(f, 2.0, 3.5, 0));
	context.ProcessMouseButtonUp(0, 0);
	REQUIRE(settled());
	CHECK(f.audioEvents()[0].at("length") == 72000);
	CHECK(drawsAudioEvent(f, 2.0, 3.5, 0));

	// Two events selected, the audio event (2 to 3.5 seconds) and the MIDI part (1 to 3 seconds) on its musical track:
	// a drag of the part's end moves both ends as far, at every step.
	f.call("event.set", {{"events", {f.event}}, {"lock", false}});
	REQUIRE(settled());
	f.display.select({f.event, f.part});
	const auto partEnd = [&f](const char* kind) { return f.lanes(kind)[1].events.at(0).end(); };
	const auto bothEnds = [&f, &partEnd](double end, double audioEnd)
	{
		return std::abs(partEnd("selected-events") - end) < 1e-6 && std::abs(partEnd("events") - end) < 1e-6 &&
			drawsAudioEvent(f, 2.0, audioEnd, 0);
	};
	press(298, 60);
	for (const double x : {318.0, 368.0, 348.0})
	{
		move(x, 60);
		f.step();
		INFO("part's end at " << x);
		const double shift = (x - 298.0) / 100.0;
		REQUIRE(f.waitUntil([&bothEnds, shift] { return bothEnds(3.0 + shift, 3.5 + shift); }));
	}
	context.ProcessMouseButtonUp(0, 0);
	// Pressed again at once, before the display has the project with the resize: the drag starts from the ends the
	// release left, and the release's answer doesn't take the new drag's preview away.
	press(348, 60);
	CHECK(std::abs(partEnd("events") - 3.5) < 1e-6);
	move(368, 60);
	f.step();
	REQUIRE(f.waitUntil([&bothEnds] { return bothEnds(3.7, 4.2); }));
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("length") == 5 * g_quarter; })); // the first release's
	CHECK(f.audioEvents()[0].at("length") == 96000);
	REQUIRE(settled());
	CHECK(bothEnds(3.7, 4.2));
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("length") == (27 * g_quarter) / 5; })); // 1 to 3.7 seconds
	CHECK(f.audioEvents()[0].at("length") == 105600);                                         // 2 to 4.2 seconds
	REQUIRE(settled());
	CHECK(bothEnds(3.7, 4.2));
}

TEST_CASE("the object selection tool's modifiers act on events whatever keys they are set to", "[unit]")
{
	Fixture f;
	f.call("settings.set",
		{{"key", "ui.toolModifiers"}, {"value", {{"select.splitEvent", "shift"}, {"dragDrop.copy", "ctrl"}}}});
	f.step();
	// Shift and a click at 2 seconds splits the part there rather than adding it to the selection.
	f.click(200, 60, {"shift"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 4 * g_quarter);
	// Ctrl and a drag of the piece from 2 to 3 seconds copies it two seconds later.
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 2; }));
	f.drag(250, 60, 450, 60, {"ctrl"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.parts()[2].at("position") == 8 * g_quarter);
	CHECK(f.parts()[2].at("shared") == 0);
	// Ctrl copying, it doesn't restrict the drag: the copy goes later and down onto a new track.
	const Json midi = f.call("track.add", {{"type", "midi"}}).at("track");
	REQUIRE(f.waitUntil([&f] { return f.lanes("events").size() == 3 && f.lanes("events")[1].events.size() == 3; }));
	f.drag(450, 60, 650, 100, {"ctrl"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	CHECK(f.parts()[3].at("position") == 12 * g_quarter);
	CHECK(f.parts()[3].at("track") == midi.at("id"));
}

TEST_CASE("Ctrl and a click adds to the selection also where Ctrl is a drag's tool modifier", "[unit]")
{
	Fixture f;
	f.click(150, 60);
	REQUIRE(f.display.selection() == std::vector<uint64_t>{f.part});
	// At the audio event's right edge, where Ctrl is Disable Snapping: a click adds it, another takes it out.
	f.click(248, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event, f.part});
	f.click(248, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	// With Ctrl as Copy, the same in the middle of it; a drag still copies only the event pressed.
	f.call("settings.set", {{"key", "ui.toolModifiers"}, {"value", {{"dragDrop.copy", "ctrl"}}}});
	f.step();
	f.click(225, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event, f.part});
	f.click(225, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	f.drag(225, 20, 425, 20, {"ctrl"});
	REQUIRE(f.waitUntil([&f] { return f.audioEvents().size() == 2; }));
	CHECK(f.parts().size() == 1);
}

TEST_CASE("a double click on an audio event opens it in the sample editor, on an audio part in its editor", "[unit]")
{
	Fixture f;
	// On the MIDI part it doesn't (the MIDI editors open with Open/Close Editor).
	f.click(150, 60, {}, 2);
	f.step();
	CHECK(f.call("sampleEditor.get").at("open") == false);
	f.click(220, 20, {}, 2);
	REQUIRE(f.waitUntil([&f] { return f.call("sampleEditor.get").at("open") == true; }));
	CHECK(f.call("sampleEditor.get").at("event") == f.event);

	// An audio part opens in the audio part editor (p. 692).
	const Json part = f.call("event.toPart", {{"events", {f.event}}}).at("events").at(0);
	REQUIRE(f.waitUntil([&f] { return f.display.project().at("audioParts").size() == 1; }));
	f.click(240, 20, {}, 2); // away from the last double click, so the clicks don't count with it
	REQUIRE(f.waitUntil([&f] { return f.call("editor.get", {{"editor", "window"}}).at("open") == true; }));
	CHECK(f.call("editor.get", {{"editor", "window"}}).at("parts") == Json::array({part}));
	CHECK(f.call("editor.get", {{"editor", "window"}}).at("content") == "audioPart");
}

TEST_CASE("a double click below the tracks adds a MIDI track", "[unit]")
{
	Fixture f;
	const auto tracks = [&f] { return f.call("project.get").at("tracks"); };
	f.click(500, 200, {}, 2); // the lanes end 80 dp down
	REQUIRE(f.waitUntil([&tracks] { return tracks().size() == 3; }));
	CHECK(tracks()[2].at("type") == "midi");
	CHECK(tracks()[2].at("name") == "MIDI 02");
	CHECK(f.parts().size() == 1);
}

TEST_CASE("the edit commands act on the selection", "[unit]")
{
	Fixture f;
	const std::map<std::string, std::function<void()>>& actions = f.display.actions();
	for (const char* id : {"edit.delete", "edit.mute", "edit.unmute", "edit.lock", "edit.unlock", "edit.splitAtCursor",
			 "edit.splitLoop", "edit.glue", "edit.duplicate", "edit.repeat", "edit.fillLoop", "edit.toRealCopy",
			 "edit.nudgeLeft", "edit.nudgeRight", "edit.nudgeStartLeft", "edit.nudgeStartRight", "edit.nudgeEndLeft",
			 "edit.nudgeEndRight", "edit.moveStartToCursor", "edit.moveEndToCursor", "edit.selectAll",
			 "edit.selectNone", "edit.invertSelection", "edit.selectInLoop", "edit.selectFromStartToCursor",
			 "edit.selectFromCursorToEnd", "edit.selectUnderCursor"})
		CHECK(actions.count(id) == 1);

	// The select submenu, at the cursor at 2.25 seconds: under it both, from it on neither, before it neither.
	f.call("transport.locate", {{"position", 108000}});
	REQUIRE(f.waitUntil([&f] { return f.display.cursor() == 2.25; }));
	f.run("edit.selectUnderCursor");
	CHECK(f.display.selection().size() == 2);
	f.run("edit.selectFromCursorToEnd");
	CHECK(f.display.selection().empty());
	f.run("edit.selectAll");
	CHECK(f.display.selection().size() == 2);
	f.display.select({f.part});
	f.run("edit.invertSelection");
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});

	// Mute shows the event greyed out; lock keeps it from being deleted.
	f.run("edit.mute");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("mute") == true; }));
	REQUIRE(f.waitUntil([&f] { return f.lanes("muted-events")[0].events.size() == 1; }));
	f.run("edit.lock");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("lock") == true; }));
	f.run("edit.delete");
	f.call("project.get");
	f.step();
	CHECK(f.audioEvents().size() == 1);
	f.run("edit.unlock");
	f.run("edit.delete");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents().empty(); }));

	// Nudging moves the part a grid step, here a beat; duplicating selects the copy right after it; moving its start
	// to the cursor.
	f.call("snap.set", {{"grid", "beat"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().grid() == engine::GridType::Beat; }));
	f.display.select({f.part});
	f.run("edit.nudgeRight");
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 3 * g_quarter; }));
	f.run("edit.nudgeEndLeft");
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("length") == 3 * g_quarter; }));
	f.run("edit.duplicate");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 6 * g_quarter);
	REQUIRE(f.waitUntil(
		[&f] { return f.display.selection() == std::vector<uint64_t>{f.parts()[1].at("id").get<uint64_t>()}; }));
	f.run("edit.moveStartToCursor");
	REQUIRE(f.waitUntil([&f] { return f.parts()[1].at("position") == (9 * g_quarter) / 2; }));

	// Split at the cursor without a selection cuts what is there; glue puts it together again.
	f.run("edit.selectNone");
	f.call("transport.locate", {{"position", 84000}}); // 1.75 s, in the first part
	REQUIRE(f.waitUntil([&f] { return f.display.cursor() == 1.75; }));
	f.run("edit.splitAtCursor");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	f.display.select({f.part, f.parts()[2].at("id").get<uint64_t>()});
	f.run("edit.glue");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[0].at("length") == 3 * g_quarter);
}

TEST_CASE("fill loop, convert to real copy and split loop act on the selection", "[unit]")
{
	Fixture f;
	// The locators from 1 to 7.5 seconds: copies of the part (1 to 3 seconds) at 3 and 5 seconds, and one at 7
	// seconds cut at the right locator.
	f.call("transport.set", {{"left", 48000}, {"right", 360000}});
	f.display.select({f.part});
	f.run("edit.fillLoop");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	for (size_t i = 1; i < 4; ++i)
	{
		CHECK(f.parts()[i].at("position") == static_cast<int64_t>(2 + (4 * i)) * g_quarter);
		CHECK(f.parts()[i].at("shared") == 0);
	}
	CHECK(f.parts()[1].at("length") == 4 * g_quarter);
	CHECK(f.parts()[3].at("length") == g_quarter);

	// Alt, Shift and a drag makes a shared copy of the last one, 1.5 seconds later; convert to real copy unshares it.
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 4; }));
	f.drag(725, 60, 875, 60, {"alt", "shift"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 5; }));
	const auto copy = f.parts()[4].at("id").get<uint64_t>();
	CHECK(f.parts()[4].at("position") == 17 * g_quarter);
	CHECK(f.parts()[4].at("shared") != 0);
	REQUIRE(f.waitUntil([&f, copy] { return f.display.selection() == std::vector<uint64_t>{copy}; }));
	f.run("edit.toRealCopy");
	REQUIRE(f.waitUntil([&f] { return f.parts()[4].at("shared") == 0; }));

	// Split loop without a selection cuts everything at the locators, here 2.25 and 4 seconds: the part, its first
	// copy and the audio event. Locating the cursor afterwards tells when the display knows the locators.
	f.run("edit.selectNone");
	f.call("transport.set", {{"left", 108000}, {"right", 192000}});
	f.call("transport.locate", {{"position", 24000}});
	REQUIRE(f.waitUntil([&f] { return f.display.cursor() == 0.5; }));
	f.run("edit.splitLoop");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 7; }));
	CHECK(f.audioEvents().size() == 2);
}

TEST_CASE("with snap on, moving, resizing, splitting and nudging follow the grid, events and the cursor", "[unit]")
{
	Fixture f;
	const auto audioPosition = [&f] { return f.audioEvents()[0].at("position").get<int64_t>(); };
	// Whether the display shows an event starting at seconds on the lane, before the next gesture.
	const auto shown = [&f](size_t lane, double seconds)
	{
		return f.waitUntil(
			[&f, lane, seconds]
			{
				const std::vector<ui::TimeSpan>& events = f.lanes("events")[lane].events;
				return std::any_of(events.begin(), events.end(),
					[seconds](const ui::TimeSpan& event) { return std::abs(event.start - seconds) < 1e-9; });
			});
	};
	f.call("snap.set", {{"on", true}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on; }));

	// Grid, bars (two seconds at 120 BPM): the part dragged from 1 to 1.7 seconds starts on the bar at 2 seconds,
	// exactly, in ticks; its end dragged to 5.3 seconds ends on the bar at 6.
	f.drag(150, 60, 220, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));
	REQUIRE(shown(1, 2.0));
	f.drag(398, 60, 528, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("length") == 8 * g_quarter; }));
	// Alt and a click at 3.3 seconds splits it on the bar at 4 seconds.
	f.click(330, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 8 * g_quarter);

	// Shuffle: the first half dragged past the middle of the second trades places with it.
	f.call("snap.set", {{"type", "shuffle"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().type == engine::SnapType::Shuffle; }));
	f.drag(300, 60, 450, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 8 * g_quarter; }));
	CHECK(f.parts()[1].at("position") == 4 * g_quarter);

	// Events: the audio event dragged to 3.95 seconds starts where the parts meet, at 4 seconds (8 dp attract).
	f.call("snap.set", {{"type", "events"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().type == engine::SnapType::Events; }));
	f.drag(225, 20, 420, 20);
	REQUIRE(f.waitUntil([&audioPosition] { return audioPosition() == 192000; }));
	// Cursor: dragged to 6.96 seconds, it starts at the cursor at 7.
	f.call("transport.locate", {{"position", 336000}});
	f.call("snap.set", {{"type", "cursor"}});
	REQUIRE(
		f.waitUntil([&f] { return f.display.snap().type == engine::SnapType::Cursor && f.display.cursor() == 7.0; }));
	REQUIRE(shown(0, 4.0));
	f.drag(425, 20, 721, 20);
	REQUIRE(f.waitUntil([&audioPosition] { return audioPosition() == 336000; }));

	// Snap point to cursor at 7.25 seconds; then the snap point, not the start, snaps to the bar at 8 seconds.
	f.call("transport.locate", {{"position", 348000}});
	f.call("snap.set", {{"type", "grid"}});
	REQUIRE(
		f.waitUntil([&f] { return f.display.snap().type == engine::SnapType::Grid && f.display.cursor() == 7.25; }));
	f.display.select({f.event});
	f.run("edit.snapPointToCursor");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("snapPoint") == 12000; }));
	REQUIRE(shown(0, 7.0));
	f.step();
	f.call("project.get"); // the display has asked for the snap point
	f.step();
	f.drag(710, 20, 760, 20);
	REQUIRE(f.waitUntil([&audioPosition] { return audioPosition() == 372000; }));

	// Grid relative in beats: dragged 0.6 seconds, the part keeps its place on the beat and moves a beat.
	f.call("snap.set", {{"type", "gridRelative"}, {"grid", "beat"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().grid() == engine::GridType::Beat; }));
	f.drag(500, 60, 560, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 9 * g_quarter; }));

	// Nudging moves a grid step, snap on or off: a beat, then a bar.
	f.display.select({f.part});
	f.run("edit.nudgeLeft");
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 8 * g_quarter; }));
	f.run("edit.snap");
	REQUIRE(f.waitUntil([&f] { return !f.display.snap().on; }));
	f.call("snap.set", {{"grid", "bar"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().grid() == engine::GridType::Bar; }));
	f.run("edit.nudgeRight");
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 12 * g_quarter; }));
	// Off, a drag doesn't snap: 0.3 seconds is 0.6 quarters.
	REQUIRE(shown(1, 6.0));
	f.drag(650, 60, 680, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == (12 * g_quarter) + ((3 * g_quarter) / 5); }));
}

TEST_CASE("with snap to events, a position marker attracts a dragged event", "[unit]")
{
	Fixture f;
	// A position marker at 6 seconds has no end; the display reads the markers with the project after every change.
	f.call("marker.add", {{"position", 12 * g_quarter}});
	f.call("snap.set", {{"on", true}, {"type", "events"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on && f.display.snap().type == engine::SnapType::Events; }));
	REQUIRE(f.waitUntil([&f] { return f.display.project().at("markers").size() == 1; }));
	// The audio event dragged from 2 to 5.95 seconds starts on the marker (8 dp attract).
	f.drag(225, 20, 620, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("position") == 288000; }));
}

// #415: every marker track's markers attract, the active one's or not, in the display as in snap.position.
TEST_CASE("with snap to events, the markers of a marker track that isn't active attract as in snap.position", "[unit]")
{
	Fixture f;
	// On the first marker track, a linear one with positions in samples, a position marker at 6 seconds and a cycle
	// marker from 7 to 8.5; the second marker track, musical and without markers, is the active one. (The test above
	// has a musical marker track's marker.)
	const Json first = f.call("track.add", {{"type", "marker"}, {"timeBase", "linear"}}).at("track").at("id");
	f.call("marker.add", {{"track", first}, {"position", 288000}});
	f.call("marker.add", {{"track", first}, {"type", "cycle"}, {"position", 336000}, {"end", 408000}});
	REQUIRE(f.call("marker.list", {{"track", first}}).at("markers").at(0).at("positionSamples") == 288000);
	const Json second = f.call("track.add", {{"type", "marker"}}).at("track").at("id");
	f.call("marker.activateTrack", {{"track", second}});
	CHECK(f.call("marker.list").at("markers").empty());
	f.call("snap.set", {{"on", true}, {"type", "events"}});
	REQUIRE(f.waitUntil(
		[&f, &second]
		{
			return f.display.snap().type == engine::SnapType::Events &&
				f.display.project().at("activeMarkerTrack") == second;
		}));

	// 8 dp attract: 0.08 seconds. The audio event's start dragged from 2 to 5.95 seconds goes where snap.position
	// puts 5.95 seconds: on the position marker.
	const int64_t range = 3840;
	const Json onMarker = f.call("snap.position", {{"position", 285600}, {"range", range}, {"events", {f.event}}});
	CHECK(onMarker.at("position") == 288000);
	f.drag(225, 20, 620, 20);
	REQUIRE(f.waitUntil([&f, &onMarker] { return f.audioEvents()[0].at("position") == onMarker.at("position"); }));

	// Dragged on from 6 to 8.45 seconds, it starts at the cycle marker's end, as snap.position has it.
	const Json onEnd = f.call("snap.position", {{"position", 405600}, {"range", range}, {"events", {f.event}}});
	CHECK(onEnd.at("position") == 408000);
	REQUIRE(f.waitUntil(
		[&f]
		{
			const std::vector<ui::TimeSpan>& events = f.lanes("events")[0].events;
			return events.size() == 1 && std::abs(events[0].start - 6.0) < 1e-9;
		}));
	f.drag(625, 20, 870, 20);
	REQUIRE(f.waitUntil([&f, &onEnd] { return f.audioEvents()[0].at("position") == onEnd.at("position"); }));
}

// Reference manual pp. 107-109: the grid types depend on the ruler's time format.
TEST_CASE("the ruler's time format chooses the grid that moves snap to", "[unit]")
{
	Fixture f;
	f.call("snap.set", {{"on", true}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on; }));

	// Seconds: 100 ms; the part dragged from 1 to 1.63 seconds starts at 1.6.
	f.display.setRulerFormat(ui::TimeFormat::Seconds);
	CHECK(f.display.snap().grid() == engine::GridType::Ms100);
	REQUIRE(f.waitUntil([&f] { return f.call("snap.get").at("format") == "seconds"; }));
	f.drag(150, 60, 213, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == (16 * g_quarter) / 5; }));

	// Bars and beats again: bars; dragged from 1.6 to 2.6 seconds it starts on the bar at 2.
	f.display.setRulerFormat(ui::TimeFormat::BarsBeats);
	REQUIRE(f.waitUntil([&f] { return f.call("snap.get").at("format") == "barsBeats"; }));
	f.drag(170, 60, 270, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));

	// The ruler follows the project's snap settings.
	f.call("snap.set", {{"format", "samples"}});
	REQUIRE(f.waitUntil([&f] { return f.display.rulerFormat() == ui::TimeFormat::Samples; }));
	CHECK(f.display.snap().grid() == engine::GridType::Samples100);

	// A snap.changed sent before the display chose its format doesn't switch the ruler back.
	f.call("snap.set", {{"format", "seconds"}});
	f.display.setRulerFormat(ui::TimeFormat::Timecode);
	f.step();
	CHECK(f.display.rulerFormat() == ui::TimeFormat::Timecode);
	CHECK(f.display.snap().format == engine::GridFormat::Timecode);
	REQUIRE(f.waitUntil([&f] { return f.call("snap.get").at("format") == "timecode"; }));
	f.step();
	CHECK(f.display.rulerFormat() == ui::TimeFormat::Timecode);
}

// #565: settings the engine sent before it had the ruler's new format, in a snap.changed or a project.get answer,
// never switch the ruler, the snap grid or the grid type menu back, not even for a step.
TEST_CASE("the ruler keeps the format it chose until the engine has it", "[unit]")
{
	Fixture f;
	// The display's ruler and snap format right after it took each snap.changed and project.get answer (the
	// display's handlers run first), and after each step.
	std::vector<std::pair<ui::TimeFormat, std::string>> seen;
	const auto see = [&f, &seen]
	{ seen.emplace_back(f.display.rulerFormat(), engine::gridFormatName(f.display.snap().format)); };
	const ui::Subscription changed = f.binding.on("snap.changed", [&see](const engine::Event&) { see(); });
	const ui::Subscription answered = f.binding.onResult("project.get", [&see](const engine::Result&) { see(); });
	// Steps until the engine has the format and the display has taken everything sent before; whether the display
	// had the format all the while.
	const auto settle = [&f, &see, &seen](ui::TimeFormat format, const std::string& name)
	{
		seen.clear();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (int confirmed = 0; confirmed < 3 && std::chrono::steady_clock::now() < deadline;)
		{
			f.ui.shell.step(milliseconds(5));
			see();
			if (f.call("snap.get").at("format") == name)
				++confirmed;
		}
		return std::all_of(seen.begin(), seen.end(),
			[&format, &name](const auto& each) { return each.first == format && each.second == name; });
	};

	// A snap.changed of another setting, sent before the ruler switches.
	f.call("snap.set", {{"on", true}});
	f.display.setRulerFormat(ui::TimeFormat::Seconds);
	CHECK(settle(ui::TimeFormat::Seconds, "seconds"));
	CHECK(f.display.snap().on);
	CHECK(f.display.snap().grid() == engine::GridType::Ms100);

	// A project.get answer that the display asked for before the ruler switched.
	// The display asks for the project when history.changed comes in; this handler, after the display's, switches the
	// ruler before the answer can be handed on.
	const uint64_t loads = f.display.loads();
	bool asked = false;
	{
		const ui::Subscription history = f.binding.on("history.changed",
			[&f, &asked](const engine::Event&)
			{
				asked = f.binding.waiting("project.get") > 0;
				f.display.setRulerFormat(ui::TimeFormat::Samples);
			});
		f.call("track.add", {{"type", "audio"}});
		CHECK(settle(ui::TimeFormat::Samples, "samples"));
	}
	CHECK(asked);
	CHECK(f.display.loads() > loads);

	// Several switches in a row end on the last one.
	f.call("snap.set", {{"zeroCrossing", true}});
	f.display.setRulerFormat(ui::TimeFormat::Timecode);
	f.display.setRulerFormat(ui::TimeFormat::BarsBeats);
	CHECK(settle(ui::TimeFormat::BarsBeats, "barsBeats"));
	CHECK(f.display.snap().zeroCrossing);
}

// The project window's tools (#86, reference manual pp. 57-65): what a press in the event lanes does with each.
TEST_CASE("the tools are chosen by command, and the display names the one chosen", "[unit]")
{
	Fixture f;
	const Rml::Element* root = f.document.GetElementById("display");
	CHECK(f.display.tool() == ui::Tool::ObjectSelection);
	CHECK(root->GetAttribute<std::string>("tool", "") == "objectSelection");
	f.run("tool.split");
	CHECK(f.display.tool() == ui::Tool::Split);
	CHECK(root->GetAttribute<std::string>("tool", "") == "split");
	// Next and previous go along the toolbar: ... erase, split, glue ...
	f.run("tool.next");
	CHECK(f.display.tool() == ui::Tool::Glue);
	f.run("tool.previous");
	f.run("tool.previous");
	CHECK(f.display.tool() == ui::Tool::Erase);
	f.run("tool.objectSelection");
	f.run("tool.previous"); // around to the last
	CHECK(f.display.tool() == ui::Tool::TimeWarp);
	for (const char* id : {"tool.objectSelection", "tool.rangeSelection", "tool.split", "tool.glue", "tool.erase",
			 "tool.zoom", "tool.mute", "tool.draw", "tool.line", "tool.play", "tool.timeWarp", "tool.next",
			 "tool.previous", "tool.combineSelectionTools"})
		CHECK(f.display.actions().count(id) == 1);
	f.run("tool.combineSelectionTools");
	CHECK(f.display.combineSelectionTools());
	// Tools that don't use the cross-hair cursor hide it (reference manual p. 110).
	f.run("tool.mute");
	CHECK(!root->IsClassSet("cross-hair"));
	f.run("tool.split");
	CHECK(root->IsClassSet("cross-hair"));
}

TEST_CASE(
	"the mouse pointer shows the tool over the event lanes, the resize and range pointers where they apply", "[unit]")
{
	Fixture f;
	const Rml::Element* area = f.document.QuerySelector(".event-area");
	// The pointer the display names and the one the window system shows, as the style sheets name it.
	const auto pointer = [area] { return area->GetAttribute<std::string>("pointer", ""); };
	const auto shown = [&f]
	{
		f.window.context().Update();
		return f.ui.windows.cursor().name;
	};
	const auto moveTo = [&f](double x, double y)
	{
		f.automation.move(std::nullopt, f.at(x, y));
		f.step();
	};

	// The object selection tool has the OS's arrow; at an event's left or right edge, where a drag resizes it, the
	// resize pointer.
	moveTo(500, 20);
	CHECK(pointer() == "objectSelection");
	CHECK(shown().empty());
	moveTo(102, 60);
	CHECK(pointer() == "resize");
	CHECK(shown() == "resize");
	moveTo(298, 60);
	CHECK(pointer() == "resize");
	moveTo(200, 60);
	CHECK(pointer() == "objectSelection");
	CHECK(shown().empty());
	// The view scrolls the event's end under the mouse, which stays where it is: the pointer follows.
	const double start = f.display.view().start;
	f.display.scrollTo(start + (98 * f.display.view().scale));
	f.step();
	CHECK(pointer() == "resize");
	CHECK(shown() == "resize");
	f.display.scrollTo(start);
	f.step();
	CHECK(pointer() == "objectSelection");

	// With the selection tools combined, the range selection tool's in a lane's upper half (reference manual p. 253).
	f.display.setCombineSelectionTools(true);
	moveTo(150, 45);
	CHECK(pointer() == "rangeSelection");
	CHECK(shown() == "tool-range");
	moveTo(150, 75);
	CHECK(pointer() == "objectSelection");
	f.display.setCombineSelectionTools(false);

	// Every other tool its own, at once when it is chosen while the mouse rests; no resize pointer at the edges.
	moveTo(102, 60);
	const std::vector<std::pair<ui::Tool, std::string>> tools = {{ui::Tool::RangeSelection, "tool-range"},
		{ui::Tool::Draw, "tool-draw"}, {ui::Tool::Erase, "tool-erase"}, {ui::Tool::Split, "tool-split"},
		{ui::Tool::Glue, "tool-glue"}, {ui::Tool::Mute, "tool-mute"}, {ui::Tool::Zoom, "tool-zoom"},
		{ui::Tool::Line, "tool-line"}, {ui::Tool::Play, "tool-play"}};
	for (const auto& [tool, cursor] : tools)
	{
		INFO(ui::toolName(tool));
		f.display.setTool(tool);
		CHECK(pointer() == ui::toolName(tool));
		CHECK(shown() == cursor);
	}
	// In scrub mode the play tool resizes an audio event at its lower left or right corner, scrubbing along (reference
	// manual p. 268): in the lower half of the edge, not the upper, where it scrubs the track; not a MIDI part, which
	// it doesn't scrub.
	f.display.setPlayMode(ui::PlayMode::Scrub);
	moveTo(248, 10);
	CHECK(pointer() == ui::toolName(ui::Tool::Play));
	moveTo(248, 30);
	CHECK(pointer() == "resize");
	CHECK(shown() == "resize");
	moveTo(102, 60);
	CHECK(pointer() == ui::toolName(ui::Tool::Play));
	f.display.setPlayMode(ui::PlayMode::Play);
	moveTo(249, 20);
	CHECK(pointer() == ui::toolName(ui::Tool::Play));
	moveTo(102, 60);

	// Over the ruler, the OS's arrow.
	f.display.setTool(ui::Tool::ObjectSelection);
	CHECK(pointer() == "resize");
	Rml::Element* ruler = f.document.QuerySelector("ruler");
	f.automation.move(
		std::nullopt, point(ruler->GetAbsoluteLeft() + 100, ruler->GetAbsoluteTop() + (ruler->GetClientHeight() / 2)));
	CHECK(shown().empty());
}

TEST_CASE("the mouse pointer shows what a press with the modifier keys held does", "[unit]")
{
	Fixture f;
	Rml::Context& context = f.window.context();
	const Rml::Element* area = f.document.QuerySelector(".event-area");
	Rml::Element* lanes = f.document.QuerySelector("event-lanes.events");
	const auto pointer = [area] { return area->GetAttribute<std::string>("pointer", ""); };
	const auto shown = [&f]
	{
		f.window.context().Update();
		return f.ui.windows.cursor().name;
	};
	const auto moveTo = [&f, &context, lanes](double x, double y, int keys = 0)
	{
		context.ProcessMouseMove(
			static_cast<int>(lanes->GetAbsoluteLeft() + x), static_cast<int>(lanes->GetAbsoluteTop() + y), keys);
		f.step();
	};

	// The object selection tool with the tool modifier Split Event (Alt): over an event a click splits it, on an
	// empty spot of a lane a drag draws a part (reference manual pp. 250, 268). At an edge a drag still resizes.
	moveTo(200, 60, Rml::Input::KM_ALT);
	CHECK(pointer() == "split");
	CHECK(shown() == "tool-split");
	moveTo(500, 60, Rml::Input::KM_ALT);
	CHECK(pointer() == "draw");
	moveTo(102, 60, Rml::Input::KM_ALT);
	CHECK(pointer() == "resize");
	// Shift adds the event to the selection, at its edge too; Ctrl there is Disable Snapping, which resizes. (RmlUi
	// sends a move only where the mouse moves.)
	moveTo(103, 60, Rml::Input::KM_SHIFT);
	CHECK(pointer() == "objectSelection");
	moveTo(102, 60, Rml::Input::KM_CTRL);
	CHECK(pointer() == "resize");
	moveTo(200, 60);
	CHECK(pointer() == "objectSelection");

	// Alt going down and up while the mouse rests changes it at once.
	f.automation.key(std::nullopt, "Alt", ui::KeyAction::Down);
	f.step();
	CHECK(pointer() == "split");
	CHECK(shown() == "tool-split");
	f.automation.key(std::nullopt, "Alt", ui::KeyAction::Up);
	f.step();
	CHECK(pointer() == "objectSelection");
	CHECK(shown().empty());
	// Both Alt keys down, one up: the other still holds Alt, which the window system's modifiers say too.
	context.ProcessKeyDown(Rml::Input::KI_LMENU, Rml::Input::KM_ALT);
	context.ProcessKeyDown(Rml::Input::KI_RMENU, Rml::Input::KM_ALT);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, Rml::Input::KM_ALT);
	f.step();
	CHECK(pointer() == "split");
	context.ProcessKeyUp(Rml::Input::KI_RMENU, 0);
	f.step();
	CHECK(pointer() == "objectSelection");
	// So does Cmd, which is Ctrl for the tool modifiers on macOS, when Ctrl goes up; a move without them ends both.
	f.call("settings.set", {{"key", "ui.toolModifiers"}, {"value", {{"select.splitEvent", "ctrl"}}}});
	int x = 200;
	REQUIRE(f.waitUntil(
		[&]
		{
			x = x == 200 ? 201 : 200; // moves, without walking to the event's edge
			moveTo(x, 60, Rml::Input::KM_CTRL);
			return pointer() == "split";
		}));
	moveTo(x == 200 ? 201 : 200, 60);
	CHECK(pointer() == "objectSelection");
	context.ProcessKeyDown(Rml::Input::KI_LMETA, Rml::Input::KM_META);
	context.ProcessKeyDown(Rml::Input::KI_LCONTROL, Rml::Input::KM_META | Rml::Input::KM_CTRL);
	context.ProcessKeyUp(Rml::Input::KI_LCONTROL, Rml::Input::KM_META);
	f.step();
	CHECK(pointer() == "split");
	moveTo(x, 60);
	CHECK(pointer() == "objectSelection");
	context.ProcessKeyUp(Rml::Input::KI_LMETA, 0);

	// The tool modifiers are the user's: with Split Event on Ctrl and Alt, Alt alone splits nothing.
	f.call("settings.set", {{"key", "ui.toolModifiers"}, {"value", {{"select.splitEvent", "ctrl+alt"}}}});
	REQUIRE(f.waitUntil(
		[&]
		{
			x = x == 200 ? 201 : 200;
			moveTo(x, 60, Rml::Input::KM_CTRL | Rml::Input::KM_ALT);
			return pointer() == "split";
		}));
	moveTo(x == 200 ? 201 : 200, 60, Rml::Input::KM_ALT);
	CHECK(pointer() == "objectSelection");
	// Other tools keep theirs.
	f.display.setTool(ui::Tool::Draw);
	moveTo(x, 60, Rml::Input::KM_CTRL | Rml::Input::KM_ALT);
	CHECK(pointer() == "draw");
}

TEST_CASE("the pointer stays the drag's while it leaves the lanes, and stops following once the mouse is out", "[unit]")
{
	Fixture f;
	Rml::Context& context = f.window.context();
	const Rml::Element* area = f.document.QuerySelector(".event-area");
	Rml::Element* lanes = f.document.QuerySelector("event-lanes.events");
	Rml::Element* ruler = f.document.QuerySelector("ruler");
	const auto pointer = [area] { return area->GetAttribute<std::string>("pointer", ""); };
	const auto shown = [&f]
	{
		f.window.context().Update();
		return f.ui.windows.cursor().name;
	};
	const auto moveTo = [&f, &context](float x, float y)
	{
		context.ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
		f.step();
	};
	const float rulerY = ruler->GetAbsoluteTop() + (ruler->GetClientHeight() / 2);

	// A drag of the draw tool from the lanes over the ruler shows the draw tool's pointer all the way.
	f.display.setTool(ui::Tool::Draw);
	moveTo(lanes->GetAbsoluteLeft() + 500, lanes->GetAbsoluteTop() + 60);
	CHECK(shown() == "tool-draw");
	context.ProcessMouseButtonDown(0, 0);
	f.step();
	moveTo(lanes->GetAbsoluteLeft() + 550, rulerY);
	CHECK(shown() == "tool-draw");
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	CHECK(shown().empty()); // up over the ruler: its own, the OS's arrow

	// Out of the lanes, changes to the view or the tool don't change the pointer for where the mouse was.
	f.display.setTool(ui::Tool::ObjectSelection);
	moveTo(lanes->GetAbsoluteLeft() + 298, lanes->GetAbsoluteTop() + 60);
	CHECK(pointer() == "resize");
	moveTo(lanes->GetAbsoluteLeft() + 298, rulerY);
	f.display.setTool(ui::Tool::Split);
	f.step();
	CHECK(pointer() == "resize");
	// Back over the lanes, it follows again.
	moveTo(lanes->GetAbsoluteLeft() + 298, lanes->GetAbsoluteTop() + 60);
	CHECK(pointer() == "split");
	CHECK(shown() == "tool-split");
}

TEST_CASE("a right click selects the event under the mouse only where a press selects events", "[unit]")
{
	Fixture f;
	const auto rightClick = [&f](double x, double y)
	{
		f.automation.click(std::nullopt, f.at(x, y), ui::MouseButton::Right, 1, {});
		f.step();
	};
	// The object selection tool selects it, for the context menu to act on.
	rightClick(200, 60);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	f.display.select({});
	// The range selection tool leaves events unselected, with a range or without.
	f.display.setTool(ui::Tool::RangeSelection);
	rightClick(200, 60);
	CHECK(f.display.selection().empty());
	f.drag(150, 60, 250, 60);
	REQUIRE(f.display.range().has_value());
	const ui::RangeSelection range = f.display.range().value_or(ui::RangeSelection{});
	rightClick(225, 20);
	CHECK(f.display.selection().empty());
	REQUIRE(f.display.range().has_value());
	CHECK(f.display.range().value_or(ui::RangeSelection{}).start == range.start);
	// Combined, so does a lane's upper half, where a press selects a range; its lower half selects events.
	f.display.setTool(ui::Tool::ObjectSelection);
	f.display.select({}); // the other tool took over the range's events
	f.display.setCombineSelectionTools(true);
	rightClick(200, 45);
	CHECK(f.display.selection().empty());
	rightClick(200, 75);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	// There, as a press would, it ends a range, which the context menu would act on instead of the event.
	f.display.select({});
	f.drag(150, 45, 250, 45);
	REQUIRE(f.display.range().has_value());
	rightClick(200, 75);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	CHECK(!f.display.range().has_value());
}

TEST_CASE("the object selection tool selects with a rectangle and sets the cursor with Alt and Shift", "[unit]")
{
	Fixture f;
	// A rectangle from an empty spot across both events selects them; another one replaces the selection, with Ctrl
	// it adds to it.
	f.drag(50, 5, 400, 75);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event, f.part});
	f.drag(50, 45, 120, 75);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	f.drag(50, 5, 230, 20, {"ctrl"});
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event, f.part});
	CHECK(!f.document.QuerySelector(".tool-rectangle")->IsClassSet("shown"));
	// Alt, Shift and a click moves the project cursor there (the tool modifier Set Cursor Position), on an event too.
	f.click(500, 60, {"alt", "shift"});
	REQUIRE(f.waitUntil([&f] { return f.display.cursor() == 5.0; }));
	f.click(150, 60, {"alt", "shift"});
	REQUIRE(f.waitUntil([&f] { return f.display.cursor() == 1.5; }));
	CHECK(f.parts().size() == 1);
	f.display.setSizingMovesContents(true);
	CHECK(f.display.sizingMovesContents());
}

TEST_CASE("the split tool splits where it clicks, with Alt into equal pieces", "[unit]")
{
	Fixture f;
	f.display.setTool(ui::Tool::Split);
	f.click(150, 60); // the part from 1 to 3 seconds at 1.5
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 3 * g_quarter);
	// Alt at 2 seconds in the piece from 1.5 to 3: pieces of half a second, at 2 and 2.5.
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 2; }));
	f.click(200, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	std::vector<int64_t> starts;
	for (const Json& part : f.parts())
		starts.push_back(part.at("position").get<int64_t>());
	std::sort(starts.begin(), starts.end());
	CHECK(starts == std::vector<int64_t>{2 * g_quarter, 3 * g_quarter, 4 * g_quarter, 5 * g_quarter});
	// With snap on beats, a click at 2.2 seconds in the audio event (2 to 2.5) goes to its start: nothing to split.
	f.call("snap.set", {{"on", true}, {"grid", "beat"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on; }));
	f.click(220, 20);
	f.call("project.get");
	f.step();
	CHECK(f.audioEvents().size() == 1);
	// The tool modifiers are a user setting: here Ctrl splits repeatedly, into pieces of a tenth of a second.
	f.call("settings.set", {{"key", "ui.toolModifiers"}, {"value", {{"split.splitRepeated", "ctrl"}}}});
	f.call("snap.set", {{"on", false}});
	REQUIRE(f.waitUntil([&f] { return !f.display.snap().on; }));
	f.click(210, 20, {"ctrl"});
	REQUIRE(f.waitUntil([&f] { return f.audioEvents().size() == 5; }));
}

TEST_CASE("the glue tool glues an event to the next one, with Alt to all that follow", "[unit]")
{
	Fixture f;
	f.call("event.split", {{"events", {f.part}}, {"positions", {60000, 72000, 96000}}}); // 1.25, 1.5 and 2 s
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 4; }));
	f.display.setTool(ui::Tool::Glue);
	f.click(110, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 3; }));
	f.click(110, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 1; }));
	CHECK(f.parts()[0].at("length") == 4 * g_quarter);
}

TEST_CASE("the erase tool deletes what it clicks, with Alt everything after it on the track", "[unit]")
{
	Fixture f;
	f.display.setTool(ui::Tool::Erase);
	f.click(225, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents().empty(); }));
	const Json midi = f.call("project.get").at("tracks")[1];
	f.call("midiPart.add", {{"track", midi.at("id")}, {"position", 8 * g_quarter}, {"length", g_quarter}});
	f.call("midiPart.add", {{"track", midi.at("id")}, {"position", 0}, {"length", g_quarter}});
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 3; }));
	f.click(150, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 1; }));
	CHECK(f.parts()[0].at("position") == 0);
}

TEST_CASE("the mute tool mutes and unmutes with a click and mutes with a rectangle", "[unit]")
{
	Fixture f;
	f.display.setTool(ui::Tool::Mute);
	f.click(150, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("mute") == true; }));
	REQUIRE(f.waitUntil([&f] { return f.lanes("muted-events")[1].events.size() == 1; }));
	f.click(150, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("mute") == false; }));
	REQUIRE(f.waitUntil([&f] { return f.lanes("muted-events")[1].events.empty(); }));
	f.drag(50, 5, 400, 75);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("mute") == true && f.audioEvents()[0].at("mute") == true; }));
}

TEST_CASE(
	"the zoom tool zooms in where it clicks, out with Alt, onto a rectangle, and back with a double click", "[unit]")
{
	Fixture f;
	f.display.setTool(ui::Tool::Zoom);
	const ui::TimelineView before = f.display.view();
	f.click(300, 60); // 3 seconds stay under the mouse
	CHECK(f.display.view().scale == before.scale / 2);
	CHECK(std::abs(f.display.view().start + (300 * f.display.view().scale) - 3.0) < 1e-9);
	f.click(300, 60, {"alt"});
	CHECK(f.display.view().scale == before.scale);
	CHECK(std::abs(f.display.view().start - before.start) < 1e-9);
	// A rectangle from 1 to 3 seconds shows them across.
	f.drag(100, 30, 300, 70);
	CHECK(std::abs(f.display.view().start - 1.0) < 1e-9);
	CHECK(std::abs(f.display.visibleLength() - 2.0) < 1e-9);
	// A double click undoes the last zoom; its first click's zoom in isn't left to redo.
	f.click(500, 60, {}, 2);
	CHECK(f.display.view().scale == before.scale);
	CHECK(std::abs(f.display.view().start - before.start) < 1e-9);
	// Alt and a double click redoes it (reference manual p. 104), and there is nothing more to redo.
	f.click(500, 60, {"alt"}, 2);
	CHECK(std::abs(f.display.view().start - 1.0) < 1e-9);
	CHECK(std::abs(f.display.visibleLength() - 2.0) < 1e-9);
	CHECK(!f.display.redoZoom());
}

TEST_CASE("with Horizontal Zooming Only off, the zoom tool's rectangle zooms the track heights too", "[unit]")
{
	Fixture f;
	f.call("track.add", {{"type", "midi"}});
	f.call("track.add", {{"type", "midi"}});
	REQUIRE(f.waitUntil([&f] { return f.lanes("events").size() == 4; }));
	f.display.setTool(ui::Tool::Zoom);
	const ui::TimelineView before = f.display.view();
	// On by default: across only.
	f.drag(100, 40, 300, 120);
	CHECK(f.display.view().trackZoom == 1.0);
	CHECK(f.display.undoZoom());
	f.call("settings.set", {{"key", "ui.zoomToolHorizontalOnly"}, {"value", false}});
	f.step();
	// From 1 to 3 seconds, and from 40 to 120 dp down: those 80 dp fill the lanes' height, in one zoom step.
	f.drag(100, 40, 300, 120);
	const double height = f.document.QuerySelector("event-lanes.events")->GetClientHeight();
	CHECK(std::abs(f.display.view().start - 1.0) < 1e-9);
	CHECK(std::abs(f.display.visibleLength() - 2.0) < 1e-9);
	CHECK(std::abs(f.display.view().trackZoom - (height / 80.0)) < 1e-6);
	CHECK(std::abs(f.display.view().top - (40.0 * f.display.view().trackZoom)) < 1e-6);
	CHECK(f.display.undoZoom());
	CHECK(f.display.view().trackZoom == 1.0);
	CHECK(f.display.view().top == 0.0);
	CHECK(f.display.view().scale == before.scale);
}

TEST_CASE(
	"the draw tool draws parts on MIDI and audio tracks, snapped; the line tool waits for automation tracks", "[unit]")
{
	Fixture f;
	f.display.setTool(ui::Tool::Draw);
	// From 4 to 5.5 seconds: eight quarters on, three long.
	f.drag(400, 60, 550, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 8 * g_quarter);
	CHECK(f.parts()[1].at("length") == 3 * g_quarter);
	REQUIRE(f.waitUntil(
		[&f] { return f.display.selection() == std::vector<uint64_t>{f.parts()[1].at("id").get<uint64_t>()}; }));
	// With snap on bars, from 6.3 to 9.6 seconds: the bars from 6 to 10. A click draws the bar there.
	f.call("snap.set", {{"on", true}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on; }));
	f.drag(630, 60, 960, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.parts()[2].at("position") == 12 * g_quarter);
	CHECK(f.parts()[2].at("length") == 8 * g_quarter);
	f.click(50, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	CHECK(f.parts()[3].at("position") == 0);
	CHECK(f.parts()[3].at("length") == 4 * g_quarter);
	// Audio parts on audio tracks (#471): from 4 to 5.5 seconds snaps to the bars from 4 to 6.
	f.drag(400, 20, 550, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioParts().size() == 1; }));
	CHECK(f.audioParts()[0].at("position") == 192000);
	CHECK(f.audioParts()[0].at("length") == 96000);
	// So does the object selection tool with Alt (reference manual p. 250): from 6.9 to 8.1 seconds, the bars 6 to 8.
	f.display.setTool(ui::Tool::ObjectSelection);
	f.drag(690, 20, 810, 20, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.audioParts().size() == 2; }));
	CHECK(f.audioParts()[1].at("position") == 288000);
	CHECK(f.audioParts()[1].at("length") == 96000);
	// The line tool draws on automation tracks (#121).
	f.display.setTool(ui::Tool::Line);
	f.display.setLineMode(ui::LineMode::Sine);
	CHECK(f.display.lineMode() == ui::LineMode::Sine);
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("line-mode", "") == "sine");
	f.drag(630, 60, 960, 60);
	f.call("project.get");
	f.step();
	CHECK(f.parts().size() == 4);
	CHECK(f.audioEvents().size() == 1);
	CHECK(f.audioParts().size() == 2);
}

TEST_CASE("Events to Part and Dissolve Part act on the selected audio events and parts", "[unit]")
{
	Fixture f;
	f.click(225, 20);
	REQUIRE(f.display.selection() == std::vector<uint64_t>{f.event});
	f.run("audio.eventsToPart");
	REQUIRE(f.waitUntil([&f] { return f.audioParts().size() == 1; }));
	const auto part = f.audioParts()[0].at("id").get<uint64_t>();
	CHECK(f.audioEvents()[0].at("part") == part);
	// The part is shown and selected; its event isn't one of its own any more.
	REQUIRE(f.waitUntil([&f, part] { return f.display.selection() == std::vector<uint64_t>{part}; }));
	CHECK(f.lanes("events")[0].events.size() == 1);
	f.run("audio.dissolvePart");
	REQUIRE(f.waitUntil([&f] { return f.audioParts().empty(); }));
	CHECK(f.audioEvents()[0].at("part") == 0);
	REQUIRE(f.waitUntil([&f] { return f.display.selection() == std::vector<uint64_t>{f.event}; }));
	// Neither does anything to a MIDI part.
	f.click(150, 60);
	f.run("audio.eventsToPart");
	f.run("audio.dissolvePart");
	f.call("project.get");
	f.step();
	CHECK(f.audioParts().empty());
	CHECK(f.parts().size() == 1);
}

TEST_CASE("the play tool auditions the pressed audio track until it is released, or scrubs it", "[unit]")
{
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	f.call("audio.start");
	const auto audio = f.call("project.get").at("tracks")[0].at("id").get<uint64_t>();
	std::vector<Json> auditions;
	const ui::Subscription subscription =
		f.binding.on("audition.changed", [&auditions](const engine::Event& event) { auditions.push_back(event.data); });
	const auto modes = [&auditions]
	{
		std::vector<std::string> result;
		result.reserve(auditions.size());
		for (const Json& audition : auditions)
			result.push_back(audition.at("mode").get<std::string>());
		return result;
	};
	f.display.setTool(ui::Tool::Play);
	CHECK(f.display.playMode() == ui::PlayMode::Play);
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("play-mode", "") == "play");

	// A click on the audio lane at 3 seconds auditions its track from there until the release; the transport stays.
	f.click(300, 10);
	REQUIRE(f.waitUntil([&modes] { return modes() == std::vector<std::string>{"play", "off"}; }));
	CHECK(auditions[0].at("track") == audio);
	CHECK(f.call("transport.get").at("playing") == false);
	CHECK(f.call("transport.get").at("position") == 0);
	// On the MIDI lane it does nothing.
	f.click(300, 60);
	f.step();
	CHECK(auditions.size() == 2);

	// Scrubbing: a drag from 2.2 to 2.4 seconds scrubs the track and moves the cursor along.
	f.display.setPlayMode(ui::PlayMode::Scrub);
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("play-mode", "") == "scrub");
	f.drag(220, 10, 240, 10);
	REQUIRE(f.waitUntil([&modes] { return modes().size() >= 4 && modes().back() == "off"; }));
	CHECK(modes()[2] == "scrub");
	CHECK(f.call("transport.get").at("position") == 115200);
	CHECK(f.call("transport.get").at("playing") == false);
	CHECK(f.parts().size() == 1); // nothing edited
	CHECK(f.audioEvents().size() == 1);

	// Only an audio event's lower corners resize it (reference manual p. 268): a drag from the upper part of its right
	// edge to 2.3 seconds scrubs the track and moves the cursor, as anywhere else, and leaves the event's length.
	auditions.clear();
	f.drag(248, 10, 230, 10);
	REQUIRE(f.waitUntil([&modes] { return modes().size() >= 2 && modes().back() == "off"; }));
	CHECK(modes()[0] == "scrub");
	CHECK(auditions[0].at("track") == audio);
	CHECK(f.call("transport.get").at("position") == 110400);
	CHECK(f.audioEvents()[0].at("length") == 24000);

	// At its lower right corner it resizes the event and scrubs along: the right edge 20 dp further makes it a fifth
	// of a second longer, and the event's clip is scrubbed, not the track, so the cursor stays.
	auditions.clear();
	f.drag(248, 30, 268, 30);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 24000 + 9600; }));
	REQUIRE(f.waitUntil([&modes] { return modes().size() >= 2 && modes().back() == "off"; }));
	CHECK(modes()[0] == "scrub");
	CHECK(auditions[0].at("track") == 0);
	CHECK(f.call("transport.get").at("position") == 110400);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});

	// A click there without a drag is no click of the object selection tool: with the tool modifier Split Event (Alt)
	// it doesn't split the event, with Set Cursor Position (Alt+Shift) it doesn't move the cursor.
	REQUIRE(f.waitUntil([&f] { return std::abs(f.lanes("events")[0].events[0].end() - 2.7) < 1e-6; }));
	f.click(268, 30, {"alt"});
	f.click(268, 30, {"alt", "shift"});
	REQUIRE(f.waitUntil([&modes] { return modes().size() >= 6 && modes().back() == "off"; }));
	CHECK(f.audioEvents().size() == 1);
	CHECK(f.audioEvents()[0].at("length") == 24000 + 9600);
	CHECK(f.call("transport.get").at("position") == 110400);
}

TEST_CASE("the play tool's resize in scrub mode plays the clip at the dragged edge", "[unit]")
{
	Fixture f;
	f.call("audio.setDevice", {{"captureSeconds", 1}}); // the dummy device, which keeps what it plays
	// A take silent for its first half second and a constant 0.5 after it, and an event of its first half second
	// at the project's start.
	const test::TempDir dir;
	test::Audio take;
	take.channels = 1;
	take.sampleRate = 48000;
	take.samples.assign(48000, 0.5f);
	std::fill(take.samples.begin(), take.samples.begin() + 24000, 0.0f);
	const std::filesystem::path file = dir.path() / "take.wav";
	test::writeWav(file, take);
	const Json media = f.call("media.add", {{"path", file.u8string()}}).at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto track = f.call("project.get").at("tracks")[0].at("id").get<uint64_t>();
	const auto event =
		f.call("audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", 0}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[0].events.size() == 2; }));
	f.display.setTool(ui::Tool::Play);
	f.display.setPlayMode(ui::PlayMode::Scrub);

	// Its end dragged out by a fifth of a second at its lower right corner, over the take's hidden audio, where the
	// track plays nothing yet: while the button is held, the device plays the take's 0.5.
	f.automation.press(std::nullopt, f.at(48, 30), ui::MouseButton::Left);
	f.step();
	f.automation.move(std::nullopt, f.at(68, 30));
	f.step();
	// The preview of the resize shows it 0.7 seconds long once the edge's scrub is on its way.
	REQUIRE(f.waitUntil(
		[&f]
		{
			const auto& events = f.lanes("events")[0].events;
			return std::any_of(events.begin(), events.end(),
				[](const ui::TimeSpan& shown) { return shown.start == 0.0 && std::abs(shown.end() - 0.7) < 1e-6; });
		}));
	// The scrub plays at most 0.1 seconds, a block of the render once the engine has it.
	int64_t rendered = 0;
	size_t heard = 0;
	while (heard < 480 && rendered < 48000) // at least 0.01 seconds, within the second captured
	{
		f.call("audio.render", {{"frames", 4800}});
		const auto played = f.call("audio.capture", {{"channel", 0}, {"start", rendered}, {"frames", 4800}})
								.at("samples")
								.get<std::vector<float>>();
		heard = static_cast<size_t>(std::count(played.begin(), played.end(), 0.5f));
		rendered += 4800;
		f.step();
	}
	CHECK(heard >= 480);
	f.automation.release(std::nullopt, f.at(68, 30), ui::MouseButton::Left);
	f.step();
	REQUIRE(f.waitUntil(
		[&f, event]
		{
			for (const Json& shown : f.audioEvents())
				if (shown.at("id") == event)
					return shown.at("length") == 24000 + 9600;
			return false;
		}));
	CHECK(f.call("transport.get").at("position") == 0);
}

TEST_CASE("the range selection tool selects ranges, snapped, by dragging and double clicking", "[unit]")
{
	Fixture f;
	const Json tracks = f.call("project.get").at("tracks");
	const auto audio = tracks[0].at("id").get<uint64_t>();
	const auto midi = tracks[1].at("id").get<uint64_t>();
	const Rml::Element* root = f.document.GetElementById("display");
	const auto range = [&f]
	{
		REQUIRE(f.display.range().has_value());
		return f.display.range().value_or(ui::RangeSelection{});
	};
	f.display.select({f.part});
	f.display.setTool(ui::Tool::RangeSelection);
	CHECK(f.display.selection().empty()); // the range tool selects ranges, not events
	// A drag from 1.5 seconds on the audio lane to 3.5 on the MIDI lane.
	f.drag(150, 10, 350, 70);
	REQUIRE(f.display.range().has_value());
	CHECK(range().start == 72000);
	CHECK(range().end == 168000);
	CHECK(range().tracks == std::vector<uint64_t>{audio, midi});
	CHECK(root->GetAttribute<std::string>("range", "") == "72000 168000");
	CHECK(root->GetAttribute<std::string>("range-tracks", "") == std::to_string(audio) + " " + std::to_string(midi));
	CHECK(f.document.QuerySelector(".range-selection")->IsClassSet("shown"));
	// A click is a range of no length there.
	f.click(500, 60);
	CHECK(range().start == 240000);
	CHECK(range().end == 240000);
	CHECK(range().tracks == std::vector<uint64_t>{midi});
	// A double click on an event selects its range; with Shift, one on another adds its range.
	f.click(150, 60, {}, 2);
	CHECK(range().start == 48000);
	CHECK(range().end == 144000);
	f.click(225, 20, {"shift"}, 2);
	CHECK(range().start == 48000);
	CHECK(range().end == 144000);
	CHECK(range().tracks == std::vector<uint64_t>{audio, midi});
	// Between events, the gap: on the audio lane from the start to the event at 2 seconds.
	f.click(100, 20, {}, 2);
	CHECK(range().start == 0);
	CHECK(range().end == 96000);
	CHECK(range().tracks == std::vector<uint64_t>{audio});
	// Snapped to bars: 0.7 to 3.3 seconds is 0 to 4. Alt and Shift take every track.
	f.call("snap.set", {{"on", true}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on; }));
	f.drag(70, 60, 330, 60, {"alt", "shift"});
	CHECK(range().start == 0);
	CHECK(range().end == 192000);
	CHECK(range().tracks == std::vector<uint64_t>{audio, midi});
	// Another tool selects the events in the range instead.
	f.display.setTool(ui::Tool::ObjectSelection);
	CHECK(!f.display.range().has_value());
	CHECK(!f.document.QuerySelector(".range-selection")->IsClassSet("shown"));
	CHECK(root->GetAttribute<std::string>("range", "").empty());
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event, f.part});
}

TEST_CASE("with the selection tools combined, a lane's upper half selects ranges and its lower half events", "[unit]")
{
	Fixture f;
	const auto range = [&f]
	{
		REQUIRE(f.display.range().has_value());
		return f.display.range().value_or(ui::RangeSelection{});
	};
	f.display.setCombineSelectionTools(true);
	f.drag(150, 45, 250, 50);
	REQUIRE(f.display.range().has_value());
	CHECK(range().start == 72000);
	CHECK(range().end == 120000);
	f.click(150, 75);
	CHECK(!f.display.range().has_value());
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	f.drag(150, 75, 250, 75);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));
	// The pointer shows which: the range selection tool's over the upper half.
	Rml::Element* lanes = f.document.QuerySelector("event-lanes.events");
	const auto pointerAt = [&f, lanes](double x, double y)
	{
		f.window.context().ProcessMouseMove(
			static_cast<int>(lanes->GetAbsoluteLeft() + x), static_cast<int>(lanes->GetAbsoluteTop() + y), 0);
		f.step();
		return f.document.QuerySelector(".event-area")->GetAttribute<std::string>("pointer", "");
	};
	CHECK(pointerAt(150, 5) == "rangeSelection");
	CHECK(pointerAt(150, 35) == "objectSelection");
	// A lane less than two rows high has no upper half for ranges: a drag there is a rectangle (p. 253), and the
	// pointer the object selection tool's.
	f.display.setTrackHeight(f.call("project.get").at("tracks")[0].at("id").get<uint64_t>(), 30.0);
	CHECK(pointerAt(150, 6) == "objectSelection");
	f.drag(150, 5, 250, 10);
	CHECK(!f.display.range().has_value());
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});
}

TEST_CASE("with Snap to Zero Crossing, a range's edges move to zero crossings of the audio", "[unit]")
{
	// A sine that crosses zero every 100 frames, in an audio event from 6 to 7 seconds playing from frame 30.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 96000; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);
	Fixture f;
	const uint64_t audio = f.call("project.get").at("tracks")[0].at("id").get<uint64_t>();
	const Json media =
		f.call("media.add", {{"path", file.u8string()}, {"sampleRate", 48000}, {"channels", 1}, {"length", 96000}})
			.at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	f.call("audioEvent.add",
		{{"track", audio}, {"clip", clip.at("id")}, {"position", 288000}, {"offset", 30}, {"length", 48000}});
	f.call("snap.set", {{"zeroCrossing", true}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().zeroCrossing && f.lanes("events")[0].events.size() == 2; }));
	f.display.setTool(ui::Tool::RangeSelection);
	// From 6.1 to 6.5 seconds, frames 4830 and 24030 of the file: the crossings at 4800 and 24000.
	const auto range = [&f] { return f.display.range().value_or(ui::RangeSelection{}); };
	f.drag(610, 20, 650, 20);
	REQUIRE(f.waitUntil([&range] { return range().start == 292770; }));
	CHECK(range().end == 311970);
	// Where no audio plays, the range stays as dragged.
	const auto answered = [&f] { return f.binding.waiting("snap.zeroCrossings") == 0; };
	f.drag(750, 20, 800, 20);
	REQUIRE(f.waitUntil(answered));
	CHECK(range().start == 360000);
	CHECK(range().end == 384000);
	// A click's empty range too: 6.2 seconds is frame 9630, the crossing at 9600.
	f.click(620, 20);
	REQUIRE(f.waitUntil([&range] { return range().start == 297570; }));
	CHECK(range().end == 297570);
	// A double click's range of an event keeps the event's edges; of a gap, those of the events around it.
	f.click(650, 20, {}, 2);
	REQUIRE(f.waitUntil(answered));
	CHECK(range().start == 288000);
	CHECK(range().end == 336000);
	f.click(500, 20, {}, 2);
	REQUIRE(f.waitUntil(answered));
	CHECK(range().start == 120000);
	CHECK(range().end == 288000);
	// An edit right after the release waits for the answer, and edits the range where its edges went: Crop Range
	// keeps frames 4800 to 24000 of the event.
	f.automation.drag(std::nullopt, f.at(610, 20), f.at(650, 20), {});
	REQUIRE(f.binding.waiting("snap.zeroCrossings") == 1);
	f.display.actions().at("edit.cropRange")();
	const auto cropped = [&f]
	{
		for (const Json& event : f.audioEvents())
			if (event.at("id") != f.event)
				return event;
		return Json::object();
	};
	REQUIRE(f.waitUntil([&cropped] { return cropped().value("offset", 0) != 30; }));
	CHECK(cropped().at("offset") == 4800);
	CHECK(cropped().at("length") == 19200);
}

TEST_CASE(
	"an edit that waits for a range's zero crossings edits that range, also when a press starts another", "[unit]")
{
	// As above: a sine crossing zero every 100 frames, in an audio event from 6 to 7 seconds playing from frame 30.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 96000; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);
	Fixture f;
	const uint64_t audio = f.call("project.get").at("tracks")[0].at("id").get<uint64_t>();
	const Json media =
		f.call("media.add", {{"path", file.u8string()}, {"sampleRate", 48000}, {"channels", 1}, {"length", 96000}})
			.at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	f.call("audioEvent.add",
		{{"track", audio}, {"clip", clip.at("id")}, {"position", 288000}, {"offset", 30}, {"length", 48000}});
	f.call("snap.set", {{"zeroCrossing", true}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().zeroCrossing && f.lanes("events")[0].events.size() == 2; }));
	f.display.setTool(ui::Tool::RangeSelection);

	// Crop Range right after the release of a drag from 6.1 to 6.5 seconds, and a drag from 6.2 to 6.4 seconds before
	// the answer: the crop keeps frames 4800 to 24000 of the event, where the first range's edges went, not 9600 to
	// 19200.
	f.automation.drag(std::nullopt, f.at(610, 20), f.at(650, 20), {});
	REQUIRE(f.binding.waiting("snap.zeroCrossings") == 1);
	f.display.actions().at("edit.cropRange")();
	f.automation.drag(std::nullopt, f.at(620, 20), f.at(640, 20), {});
	const auto cropped = [&f]
	{
		for (const Json& event : f.audioEvents())
			if (event.at("id") != f.event)
				return event;
		return Json::object();
	};
	REQUIRE(f.waitUntil([&cropped] { return cropped().value("offset", 0) != 30; }));
	CHECK(cropped().at("offset") == 4800);
	CHECK(cropped().at("length") == 19200);
	// The second range is the one shown, at its zero crossings: 6.2 and 6.4 seconds are frames 9630 and 19230.
	const auto range = [&f] { return f.display.range().value_or(ui::RangeSelection{}); };
	REQUIRE(f.waitUntil([&range] { return range().start == 297570; }));
	CHECK(range().end == 307170);
}

TEST_CASE("the object selection tool with Alt draws an empty MIDI part on an empty spot", "[unit]")
{
	Fixture f;
	// From 4 to 5.5 seconds on the MIDI lane (reference manual pp. 250-251), rather than a rectangle.
	f.drag(400, 60, 550, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 8 * g_quarter);
	CHECK(f.parts()[1].at("length") == 3 * g_quarter);
	REQUIRE(f.waitUntil(
		[&f] { return f.display.selection() == std::vector<uint64_t>{f.parts()[1].at("id").get<uint64_t>()}; }));
}

TEST_CASE("Ctrl and a drag moves or copies events only across or only up and down", "[unit]")
{
	Fixture f;
	const Json from = f.parts()[0].at("track");
	const Json midi = f.call("track.add", {{"type", "midi"}}).at("track");
	REQUIRE(f.waitUntil([&f] { return f.lanes("events").size() == 3; }));
	// Mostly across: the part moves a second later and stays on its track, though the mouse ends a lane lower.
	f.drag(150, 60, 250, 100, {"ctrl"});
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));
	CHECK(f.parts()[0].at("track") == from);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	// Mostly down: onto the track below, at the same time, though the mouse ends 20 dp later.
	f.drag(250, 60, 270, 100, {"ctrl"});
	REQUIRE(f.waitUntil([&f, &midi] { return f.parts()[0].at("track") == midi.at("id"); }));
	CHECK(f.parts()[0].at("position") == 4 * g_quarter);
	// With Alt too, a copy two seconds later on the same track, though the mouse ends lower.
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[2].events.size() == 1; })); // once the display shows it
	f.drag(250, 100, 450, 130, {"ctrl", "alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 8 * g_quarter);
	CHECK(f.parts()[1].at("track") == midi.at("id"));
	// Ctrl and a click still takes a selected event out of the selection.
	REQUIRE(f.waitUntil([&f] { return f.display.selection().size() == 1 && f.display.selection()[0] != f.part; }));
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[2].events.size() == 2; }));
	f.click(450, 100, {"ctrl"});
	CHECK(f.display.selection().empty());
	// Ctrl and Alt on an event that isn't selected copies it alone, as Alt does, rather than adding it.
	f.display.select({f.event});
	f.drag(300, 100, 500, 105, {"ctrl", "alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.parts()[2].at("position") == 8 * g_quarter);
	CHECK(f.audioEvents().size() == 1);
}

TEST_CASE("a press becomes a move of events only after the drag delay", "[unit]")
{
	Fixture f;
	Rml::Context& context = f.window.context();
	Rml::Element* lanes = f.document.QuerySelector("event-lanes.events");
	REQUIRE(lanes != nullptr);
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true); // RmlUi's time stands still but for advance()
	const auto move = [&context, lanes](double x, double y, int keys = 0)
	{
		context.ProcessMouseMove(
			static_cast<int>(lanes->GetAbsoluteLeft() + x), static_cast<int>(lanes->GetAbsoluteTop() + y), keys);
	};
	const auto previewStart = [&f] { return f.lanes("selected-events")[1].events.at(0).start; };

	// Pressed, dragged and released at once: a click, which selects the part and leaves it where it is.
	move(150, 60);
	context.ProcessMouseButtonDown(0, 0);
	move(250, 60);
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});
	f.run("edit.mute"); // after the move, if the click had sent one
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("mute") == true; }));
	CHECK(f.parts()[0].at("position") == 2 * g_quarter);
	clock->advance(1.0); // no double click with the next press

	// The same with Ctrl, though the mouse moved a little: a click, which takes the part out of the selection.
	move(150, 60, Rml::Input::KM_CTRL);
	context.ProcessMouseButtonDown(0, Rml::Input::KM_CTRL);
	move(155, 60, Rml::Input::KM_CTRL);
	context.ProcessMouseButtonUp(0, Rml::Input::KM_CTRL);
	f.step();
	CHECK(f.display.selection().empty());
	clock->advance(1.0);
	// Resizing doesn't wait: the audio event's right edge 20 dp further at once.
	move(248, 20);
	context.ProcessMouseButtonDown(0, 0);
	move(268, 20);
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 24000 + 9600; }));
	clock->advance(1.0);
	f.display.select({f.part});

	// Held past the delay, the drag shows where the part goes and moves it there.
	move(150, 60);
	context.ProcessMouseButtonDown(0, 0);
	move(250, 60);
	f.step();
	CHECK(std::abs(previewStart() - 1.0) < 1e-9);
	clock->advance(0.5);
	move(252, 60);
	f.step();
	CHECK(previewStart() > 1.5);
	move(250, 60);
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));
	clock->freeze(false);
}

TEST_CASE("a selection on several tracks moves and copies onto other tracks of their types", "[unit]")
{
	Fixture f;
	const Json from = f.parts()[0].at("track");
	const Json firstAudio = f.audioEvents()[0].at("track");
	const Json audio = f.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
	const Json midi = f.call("track.add", {{"type", "midi"}}).at("track");
	REQUIRE(f.waitUntil([&f] { return f.lanes("events").size() == 4; }));
	const auto undo = [&f] { return f.call("history.get").at("steps").size(); };
	const size_t before = undo();
	f.display.select({f.event, f.part});

	// A lane down would put the audio event on a MIDI track and the part on an audio track: they only move later.
	f.drag(150, 60, 250, 100);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 4 * g_quarter; }));
	CHECK(f.parts()[0].at("track") == from);
	CHECK(f.audioEvents()[0].at("position") == 96000 + 48000);

	// Two lanes down, each onto the track of its type there, in one undo step.
	f.drag(250, 60, 250, 140);
	REQUIRE(f.waitUntil([&f, &midi] { return f.parts()[0].at("track") == midi.at("id"); }));
	CHECK(f.audioEvents()[0].at("track") == audio.at("id"));
	CHECK(f.parts()[0].at("position") == 4 * g_quarter);
	CHECK(f.audioEvents()[0].at("position") == 96000 + 48000);
	CHECK(undo() == before + 2);

	// Alt and a drag two lanes up copies both back onto the first two tracks, and selects the copies.
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[3].events.size() == 1; })); // once the display shows it
	f.drag(250, 140, 250, 60, {"alt"});
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2 && f.audioEvents().size() == 2; }));
	CHECK(f.parts()[1].at("track") == from);
	CHECK(f.audioEvents()[1].at("track") == firstAudio);
	std::vector<uint64_t> copies{f.audioEvents()[1].at("id").get<uint64_t>(), f.parts()[1].at("id").get<uint64_t>()};
	std::sort(copies.begin(), copies.end());
	REQUIRE(f.waitUntil([&f, &copies] { return f.display.selection() == copies; }));
	CHECK(undo() == before + 3);
}

TEST_CASE("the arrow keys' commands select the event before or after on its track", "[unit]")
{
	Fixture f;
	const Json midi = f.parts()[0].at("track");
	const uint64_t later =
		f.call("midiPart.add", {{"track", midi}, {"position", 8 * g_quarter}, {"length", 2 * g_quarter}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[1].events.size() == 2; }));
	using Ids = std::vector<uint64_t>;

	// Without a selection, nothing.
	f.run("edit.selectNext");
	CHECK(f.display.selection().empty());
	// Along the track, and no further than its ends.
	f.display.select({f.part});
	f.run("edit.selectNext");
	CHECK(f.display.selection() == Ids{later});
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("selection", "") == std::to_string(later));
	f.run("edit.selectNext");
	CHECK(f.display.selection() == Ids{later});
	f.run("edit.selectPrevious");
	CHECK(f.display.selection() == Ids{f.part});
	f.run("edit.selectPrevious");
	CHECK(f.display.selection() == Ids{f.part});
}

TEST_CASE("the edit commands edit the selection range, and paste pastes at its start", "[unit]")
{
	Fixture f;
	const std::map<std::string, std::function<void()>>& actions = f.display.actions();
	for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.cutTime", "edit.deleteTime", "edit.pasteTime",
			 "edit.insertSilence", "edit.splitRange", "edit.cropRange", "edit.globalCopy"})
		CHECK(actions.count(id) == 1);
	const Json tracks = f.call("project.get").at("tracks");
	const auto audio = tracks[0].at("id").get<uint64_t>();
	const auto midi = tracks[1].at("id").get<uint64_t>();

	// From 1.5 to 2.25 seconds on both tracks: split cuts the event at 2.25 and the part at both edges.
	f.display.setTool(ui::Tool::RangeSelection);
	f.display.setRange(ui::RangeSelection{72000, 108000, {audio, midi}});
	f.run("edit.splitRange");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.audioEvents().size() == 2);
	f.call("history.undo");

	// Delete Time removes what is in the range and moves what follows back by 0.75 seconds.
	f.run("edit.deleteTime");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 3 * g_quarter);
	REQUIRE(f.audioEvents().size() == 1);
	CHECK(f.audioEvents()[0].at("position") == 72000);
	f.call("history.undo");

	// Cut leaves the gap; paste puts the range back at its start, on its first track and the one below.
	f.run("edit.cut");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.audioEvents().size() == 1);
	f.run("edit.paste");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.audioEvents().size() == 2);
	CHECK(f.display.range().has_value()); // the range stays (reference manual p. 287)

	// Without a range, paste goes to the project cursor and selects what it pasted.
	f.display.setTool(ui::Tool::ObjectSelection);
	f.call("transport.locate", {{"position", 240000}});
	REQUIRE(f.waitUntil([&f] { return f.display.cursor() == 5.0; }));
	f.run("edit.paste");
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	CHECK(f.parts()[3].at("position") == 10 * g_quarter);
	REQUIRE(f.waitUntil([&f] { return f.display.selection().size() == 2; }));
}

namespace
{
	const ui::FadeLines& fadeLines(const Fixture& f)
	{
		auto* found = dynamic_cast<ui::FadeLines*>(f.document.QuerySelector("fade-lines"));
		REQUIRE(found != nullptr);
		return *found;
	}

	int64_t fadeLength(const Json& event, const char* fade)
	{
		return event.at(fade).at("length").get<int64_t>();
	}
}

TEST_CASE("fade handles at the top corners of audio events drag the selected events' fades", "[unit]")
{
	Fixture f;
	// A second event on the audio track, from 4 to 4.5 seconds.
	const Json first = f.audioEvents()[0];
	const auto second =
		f.call("audioEvent.add",
			 {{"track", first.at("track")}, {"clip", first.at("clip")}, {"position", 192000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[0].events.size() == 2; }));

	// Over a handle the mouse pointer isn't the resize one, as a press there drags the fade; lower down it is.
	const Rml::Element* area = f.document.QuerySelector(".event-area");
	f.automation.move(std::nullopt, f.at(200, 3));
	f.step();
	CHECK(area->GetAttribute<std::string>("pointer", "") == "objectSelection");
	f.automation.move(std::nullopt, f.at(200, 20));
	f.step();
	CHECK(area->GetAttribute<std::string>("pointer", "") == "resize");

	// The fade in handle at the event's top left corner, dragged 10 dp right: a tenth of a second.
	f.drag(200, 3, 210, 3);
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[0], "fadeIn") == 4800; }));
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});
	REQUIRE(f.waitUntil([&f] { return !fadeLines(f).lanes().empty() && fadeLines(f).lanes()[0].fades.size() == 1; }));
	const ui::FadeLine line = fadeLines(f).lanes()[0].fades[0];
	CHECK(line.in);
	CHECK(line.start == 2.0);
	CHECK(std::abs(line.length - 0.1) < 1e-9);
	// The selected event shows its handles, where its fade in ends and its fade out starts, and its event volume
	// handle in the middle (#139).
	REQUIRE(fadeLines(f).lanes()[0].handles.size() == 3);
	CHECK(fadeLines(f).lanes()[0].handles[2] == 2.25);

	// The fade out handle at the top right corner, 20 dp left: a fifth of a second.
	f.drag(249, 3, 229, 3);
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[0], "fadeOut") == 9600; }));

	// With both events selected, a drag gives both the fade the pressed one gets, as one undo step.
	f.display.select({f.event, second});
	f.drag(210, 3, 220, 3);
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[1], "fadeIn") == 9600; }));
	CHECK(fadeLength(f.audioEvents()[0], "fadeIn") == 9600);
	f.call("history.undo");
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[1], "fadeIn") == 0; }));
	CHECK(fadeLength(f.audioEvents()[0], "fadeIn") == 4800);

	// Lower down, the event's edge still resizes it.
	f.drag(248, 20, 268, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("length") == 24000 + 9600; }));
}

TEST_CASE("a fade handle drag ends without changes when the pressed event goes during it", "[unit]")
{
	Fixture f;
	const Json first = f.audioEvents()[0];
	const auto second =
		f.call("audioEvent.add",
			 {{"track", first.at("track")}, {"clip", first.at("clip")}, {"position", 192000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[0].events.size() == 2; }));
	f.display.select({f.event, second});
	Rml::Context& context = f.window.context();
	Rml::Element* lanes = f.document.QuerySelector("event-lanes.events");
	REQUIRE(lanes != nullptr);
	const auto move = [&context, lanes](double x, double y)
	{
		context.ProcessMouseMove(
			static_cast<int>(lanes->GetAbsoluteLeft() + x), static_cast<int>(lanes->GetAbsoluteTop() + y), 0);
	};

	// The first event's fade in handle pressed and dragged, then the event removed through the engine.
	move(200, 3);
	context.ProcessMouseButtonDown(0, 0);
	move(210, 3);
	f.step();
	f.call("event.remove", {{"events", {f.event}}});
	REQUIRE(f.waitUntil([&f, second] { return f.display.selection() == std::vector<uint64_t>{second}; }));
	move(220, 3);
	f.step();
	context.ProcessMouseButtonUp(0, 0);
	f.step();
	REQUIRE(f.audioEvents().size() == 1);
	CHECK(fadeLength(f.audioEvents()[0], "fadeIn") == 0);
	CHECK(fadeLength(f.audioEvents()[0], "fadeOut") == 0);
}

TEST_CASE("the Fades menu's commands fade to the range, crossfade and remove fades", "[unit]")
{
	Fixture f;
	const std::map<std::string, std::function<void()>>& actions = f.display.actions();
	for (const char* id : {"audio.fadeInToRange", "audio.fadeOutFromRange", "audio.adjustFadesToRange",
			 "audio.crossfade", "audio.removeFadeIn", "audio.removeFadeOut", "audio.removeFades", "audio.fadeEditor"})
		CHECK(actions.count(id) == 1);
	const Json first = f.audioEvents()[0];
	const auto track = first.at("track").get<uint64_t>();

	// Fade In to Range Start: the event from 2 seconds, the range from 2.1 to 2.3.
	f.display.setTool(ui::Tool::RangeSelection);
	f.display.setRange(ui::RangeSelection{100800, 110400, {track}});
	f.run("audio.fadeInToRange");
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[0], "fadeIn") == 4800; }));
	f.run("audio.fadeOutFromRange");
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[0], "fadeOut") == 9600; }));

	// Crossfade: the selected event and one that starts inside it.
	f.display.setTool(ui::Tool::ObjectSelection);
	const auto second = f.call("audioEvent.add",
							 {{"track", track}, {"clip", first.at("clip")}, {"position", 115200}, {"length", 24000}})
							.at("event")
							.at("id")
							.get<uint64_t>();
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[0].events.size() == 2; }));
	f.display.select({f.event, second});
	f.run("audio.crossfade");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[1].at("crossfade").is_object(); }));
	// The crossfade replaces the first event's fade out and the second's fade in, over the overlap.
	REQUIRE(f.waitUntil(
		[&f]
		{
			const std::vector<ui::FadeLine>& fades = fadeLines(f).lanes()[0].fades;
			return fades.size() == 3 &&
				std::count_if(fades.begin(), fades.end(),
					[](const ui::FadeLine& fade) { return std::abs(fade.start - 2.4) < 1e-9; }) == 2;
		}));

	// Remove Fade In takes the crossfade into the second event; Remove Fades the first event's fade in too.
	f.display.select({second});
	f.run("audio.removeFadeIn");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[1].at("crossfade").is_null(); }));
	f.display.select({f.event});
	f.run("audio.removeFades");
	REQUIRE(f.waitUntil([&f] { return fadeLength(f.audioEvents()[0], "fadeIn") == 0; }));
	CHECK(fadeLength(f.audioEvents()[0], "fadeOut") == 0);
}

namespace
{
	double eventVolume(const Json& event)
	{
		return event.at("volume").get<double>();
	}
}

TEST_CASE("the event volume handle at the top middle of an audio event drags the selected events' volume", "[unit]")
{
	Fixture f;
	// 10 dp down a 40 dp lane, from 0 dB half way up (+6 dB at the top): -6 dB.
	f.drag(225, 3, 225, 13);
	REQUIRE(f.waitUntil([&f] { return std::abs(eventVolume(f.audioEvents()[0]) + 6.0) < 0.1; }));
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});
	// The event shows its volume as a line, and the selected one its fade handles and its volume handle.
	REQUIRE(f.waitUntil([&f] { return !fadeLines(f).lanes().empty() && fadeLines(f).lanes()[0].volumes.size() == 1; }));
	CHECK(fadeLines(f).lanes()[0].handles.size() == 3);
	CHECK(std::abs(fadeLines(f).lanes()[0].volumes[0].gain - 0.5) < 0.01);
	f.call("history.undo");
	REQUIRE(f.waitUntil([&f] { return eventVolume(f.audioEvents()[0]) == 0.0; }));
	// Lower down, a drag still moves the event.
	f.drag(225, 30, 325, 30);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("position") == 144000; }));
}

TEST_CASE("the draw tool adds and drags volume curve points, the erase tool removes them", "[unit]")
{
	Fixture f;
	f.display.setTool(ui::Tool::Draw);
	// A click on the event adds a point there: 0.15 seconds in, a quarter up the lane (-6 dB).
	f.click(215, 30);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve").size() == 1; }));
	Json point = f.audioEvents()[0].at("volumeCurve")[0];
	CHECK(point.at("position") == 7200);
	CHECK(std::abs(point.at("volume").get<double>() + 6.0) < 0.1);
	REQUIRE(f.waitUntil([&f] { return !fadeLines(f).lanes().empty() && fadeLines(f).lanes()[0].volumes.size() == 1; }));
	CHECK(fadeLines(f).lanes()[0].volumes[0].points.size() == 1);
	// What the display shows: the point at a clip frame.
	const auto shows = [&f](size_t points, int64_t first)
	{
		return f.waitUntil(
			[&f, points, first]
			{
				const std::vector<ui::VolumeLine>& lines = fadeLines(f).lanes()[0].volumes;
				return lines.size() == 1 && lines[0].points.size() == points && lines[0].points[0].position == first;
			});
	};

	// Dragging it moves it: 0.3 seconds in, three quarters up (+3.5 dB).
	f.drag(215, 30, 230, 10);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve")[0].at("position") == 14400; }));
	point = f.audioEvents()[0].at("volumeCurve")[0];
	CHECK(std::abs(point.at("volume").get<double>() - 3.5) < 0.1);
	CHECK(f.audioEvents()[0].at("volumeCurve").size() == 1);
	REQUIRE(shows(1, 14400));
	f.click(240, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve").size() == 2; }));
	REQUIRE(shows(2, 14400));

	// The erase tool on a point removes the point, not the event.
	f.display.setTool(ui::Tool::Erase);
	f.click(230, 10);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve").size() == 1; }));
	CHECK(f.audioEvents()[0].at("volumeCurve")[0].at("position") == 19200);
}

TEST_CASE("the draw tool's click near a curve point leaves it, a drag moves it by the drag's distance", "[unit]")
{
	Fixture f;
	// A point at 0 dB (half way up the lane), 0.2 seconds into the event.
	f.call("audioEvent.setVolumeCurve", {{"event", f.event}, {"points", {{{"position", 9600}, {"volume", 0.0}}}}});
	REQUIRE(f.waitUntil(
		[&f]
		{
			return !fadeLines(f).lanes().empty() && fadeLines(f).lanes()[0].volumes.size() == 1 &&
				fadeLines(f).lanes()[0].volumes[0].points.size() == 1;
		}));
	f.display.setTool(ui::Tool::Draw);
	// A click 5 dp below the point only presses it.
	f.click(220, 25);
	// A drag that starts 2 dp right of and 4 dp below the point moves it 10 dp right and up: 0.1 seconds later and
	// a quarter of the lane higher (+3.5 dB), not to where the mouse is.
	f.drag(222, 24, 232, 14);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve")[0].at("position") == 14400; }));
	const Json point = f.audioEvents()[0].at("volumeCurve")[0];
	CHECK(std::abs(point.at("volume").get<double>() - 3.5) < 0.1);
	CHECK(f.audioEvents()[0].at("volumeCurve").size() == 1);
	// One undo goes back to the point as it was: the click made no undo step.
	f.call("history.undo");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve")[0].at("position") == 9600; }));
	CHECK(f.audioEvents()[0].at("volumeCurve")[0].at("volume") == 0.0);
}

TEST_CASE("the draw tool moves a curve point drawn clipped at the lane's top from its own volume", "[unit]")
{
	Fixture f;
	// Waits until the display shows the event at +3 dB with one point at volume dB, 0.2 seconds in.
	const auto shows = [&f](double volume)
	{
		return f.waitUntil(
			[&f, volume]
			{
				const std::vector<ui::FadeLines::Lane>& lanes = fadeLines(f).lanes();
				if (lanes.empty() || lanes[0].volumes.size() != 1)
					return false;
				const ui::VolumeLine& line = lanes[0].volumes[0];
				return std::abs(line.gain - std::pow(10.0, 3.0 / 20.0)) < 1e-9 && line.points.size() == 1 &&
					line.points[0].position == 9600 &&
					std::abs(line.points[0].gain - std::pow(10.0, volume / 20.0)) < 1e-6;
			});
	};
	const auto pointVolume = [&f] { return f.audioEvents()[0].at("volumeCurve")[0].at("volume").get<double>(); };
	// Event volume +3 dB and a point at +6 dB: +9 dB, above the line's top (+6 dB), so the point is drawn at the top
	// of the lane, though the line would reach it 1.41 times the lane's height up.
	f.call("audioEvent.set", {{"event", f.event}, {"volume", 3.0}});
	f.call("audioEvent.setVolumeCurve", {{"event", f.event}, {"points", {{{"position", 9600}, {"volume", 6.0}}}}});
	REQUIRE(shows(6.0));
	f.display.setTool(ui::Tool::Draw);
	// A drag up leaves the point as it is and makes no undo step.
	f.drag(220, 4, 220, 0);
	// 4 dp down a 40 dp lane takes a tenth of the lane off its real height: +8.36 dB in all, the point +5.36 dB, not
	// a tenth below the top (+2.1 dB).
	f.drag(220, 1, 220, 5);
	REQUIRE(f.waitUntil([&] { return pointVolume() != 6.0; }));
	CHECK(std::abs(pointVolume() - 5.36) < 0.05);
	CHECK(f.audioEvents()[0].at("volumeCurve")[0].at("position") == 9600);
	// The display has the change before the undo, so the next drag starts from the point as the undo leaves it.
	REQUIRE(shows(pointVolume()));
	f.call("history.undo");
	REQUIRE(shows(6.0));
	CHECK(pointVolume() == 6.0);
	// The next undo step is the curve the test set: the drag up made none.
	f.call("history.undo");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve").empty(); }));
	REQUIRE(f.waitUntil(
		[&f]
		{
			const std::vector<ui::FadeLines::Lane>& lanes = fadeLines(f).lanes();
			return !lanes.empty() && lanes[0].volumes.size() == 1 && lanes[0].volumes[0].points.empty();
		}));
	f.call("history.redo");
	REQUIRE(shows(6.0));
	// 20 dp down: half the lane off 1.41, +5.2 dB in all, the point +2.2 dB.
	f.drag(220, 1, 220, 21);
	REQUIRE(f.waitUntil([&] { return pointVolume() != 6.0; }));
	CHECK(std::abs(pointVolume() - 2.2) < 0.05);

	// A point drawn below the top still goes no higher than the top: a press 5 dp below one at -3 dB (0 dB in all,
	// half way up) dragged 25 dp up to the top takes it to +6 dB in all, the point to +3 dB.
	f.call("audioEvent.setVolumeCurve", {{"event", f.event}, {"points", {{{"position", 9600}, {"volume", -3.0}}}}});
	REQUIRE(shows(-3.0));
	f.drag(220, 25, 220, 0);
	REQUIRE(f.waitUntil([&] { return pointVolume() != -3.0; }));
	CHECK(std::abs(pointVolume() - 3.0) < 0.05);
	CHECK(f.audioEvents()[0].at("volumeCurve").size() == 1);
}

TEST_CASE("Remove Volume Curve removes the selected events' curves, or resets them in the selection range", "[unit]")
{
	Fixture f;
	CHECK(f.display.actions().count("audio.removeVolumeCurve") == 1);
	const auto track = f.audioEvents()[0].at("track").get<uint64_t>();
	const auto setCurve = [&f]
	{ f.call("audioEvent.setVolumeCurve", {{"event", f.event}, {"points", {{{"position", 0}, {"volume", -12.0}}}}}); };
	setCurve();
	f.display.select({f.event});
	f.run("audio.removeVolumeCurve");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve").empty(); }));

	// In the range from 2.1 to 2.3 seconds the original level, -12 dB outside it.
	setCurve();
	f.display.setTool(ui::Tool::RangeSelection);
	f.display.setRange(ui::RangeSelection{100800, 110400, {track}});
	f.run("audio.removeVolumeCurve");
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("volumeCurve").size() == 5; }));
	const Json curve = f.audioEvents()[0].at("volumeCurve");
	CHECK(curve[2] == Json{{"position", 4800}, {"volume", 0.0}, {"curve", 0.0}});
	CHECK(curve[3] == Json{{"position", 14400}, {"volume", 0.0}, {"curve", 0.0}});
}

TEST_CASE("lanes show below their track, which shows the take in front; the comp tool brings takes to the front and "
		  "comps a range dragged on a lane",
	"[unit]")
{
	Fixture f;
	const auto audioTrack = f.call("project.get").at("tracks").at(0).at("id").get<uint64_t>();
	// A second take over the first, on lane 1: in front, it covers the first on the track's lane.
	const Json clip = f.audioEvents()[0].at("clip");
	const auto second =
		f.call("audioEvent.add", {{"track", audioTrack}, {"clip", clip}, {"position", 96000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	f.call("event.set", {{"events", {second}}, {"lane", 1}});
	REQUIRE(f.waitUntil([&f] { return f.display.project().at("audioEvents").size() == 2; }));
	CHECK(f.lanes("events")[0].events.size() == 1);

	// Show Lanes: a lane each below the track's, as high as the track.
	f.display.showLanes(audioTrack, true);
	f.step();
	CHECK(f.display.laneTakes() == std::vector<int>{-1, 0, 1, -1});
	CHECK(f.display.laneTracks()[2] == audioTrack);
	REQUIRE(f.lanes("events").size() == 4);
	CHECK(f.lanes("events")[1].events.size() == 1);
	CHECK(f.lanes("events")[2].events.size() == 1);

	// A click with the comp tool on the first take, on its lane (40 to 80 dp), brings it to the front.
	f.display.setTool(ui::Tool::Comp);
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("tool", "") == "comp");
	f.click(225, 60);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("front") != 0; }));
	// A drag on the second take's lane from 2.1 to 2.3 seconds comps it in: both takes split there, and the track's
	// lane shows the first take, the second, the first.
	f.drag(210, 100, 230, 100);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents().size() == 6; }));
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[0].events.size() == 3; }));
	const std::vector<ui::TimeSpan>& shown = f.lanes("events")[0].events;
	CHECK(std::abs(shown[1].start - 2.1) < 1e-6);
	CHECK(std::abs(shown[1].end() - 2.3) < 1e-6);
	uint64_t front = 0;
	int64_t frontOrder = 0;
	for (const Json& event : f.audioEvents())
	{
		const auto order =
			event.at("front").get<int64_t>() != 0 ? event.at("front").get<int64_t>() : event.at("id").get<int64_t>();
		if (order > frontOrder)
		{
			frontOrder = order;
			front = event.at("id").get<uint64_t>();
		}
	}
	for (const Json& event : f.audioEvents())
		if (event.at("id") == front)
		{
			CHECK(event.at("lane") == 1);
			CHECK(event.at("position") == 100800);
		}

	// Shift and a click selects a take; hiding the lanes leaves the track's own lane.
	f.click(205, 100, {"shift"});
	CHECK(f.display.selection().size() == 1);
	f.display.showLanes(audioTrack, false);
	f.step();
	CHECK(f.display.laneTakes() == std::vector<int>{-1, -1});
}

TEST_CASE("a muted take covers nothing on its track's lane, which shows what plays", "[unit]")
{
	Fixture f;
	const auto audioTrack = f.call("project.get").at("tracks").at(0).at("id").get<uint64_t>();
	// A second take from 2.1 to 2.3 seconds on lane 1, in front of the first but muted: the first plays all through.
	const auto second = f.call("audioEvent.add",
							 {{"track", audioTrack}, {"clip", f.audioEvents()[0].at("clip")}, {"position", 100800},
								 {"length", 9600}, {"lane", 1}, {"mute", true}})
							.at("event")
							.at("id")
							.get<uint64_t>();
	REQUIRE(f.waitUntil([&f] { return f.display.project().at("audioEvents").size() == 2; }));
	REQUIRE(f.lanes("events")[0].events.size() == 1);
	CHECK(std::abs(f.lanes("events")[0].events[0].start - 2.0) < 1e-6);
	CHECK(std::abs(f.lanes("events")[0].events[0].end() - 2.5) < 1e-6);
	CHECK(f.lanes("muted-events")[0].events.empty());
	// A click there finds the take that plays.
	f.click(220, 20);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});

	// The first take muted, the second unmuted: the track's lane shows the second where it plays, and the muted one
	// around it, muted only there.
	f.call("event.set", {{"events", {f.event}}, {"mute", true}});
	f.call("event.set", {{"events", {second}}, {"mute", false}});
	REQUIRE(f.waitUntil([&f] { return f.lanes("events")[0].events.size() == 3; }));
	const std::vector<ui::TimeSpan>& muted = f.lanes("muted-events")[0].events;
	REQUIRE(muted.size() == 2);
	CHECK(std::abs(muted[0].end() - 2.1) < 1e-6);
	CHECK(std::abs(muted[1].start - 2.3) < 1e-6);
	f.click(220, 20);
	CHECK(f.display.selection() == std::vector<uint64_t>{second});
	f.click(205, 20);
	CHECK(f.display.selection() == std::vector<uint64_t>{f.event});
}

TEST_CASE("the draw tool draws parts on the lane it draws on", "[unit]")
{
	Fixture f;
	const Json tracks = f.call("project.get").at("tracks");
	const auto audioTrack = tracks.at(0).at("id").get<uint64_t>();
	const auto midiTrack = tracks.at(1).at("id").get<uint64_t>();
	// Both tracks with something on lane 1 show lanes 0 and 1 below them: the audio track's from 40 to 120 dp, the
	// MIDI track's from 160 to 240 dp.
	f.call("event.set", {{"events", {f.event, f.part}}, {"lane", 1}});
	f.display.showLanes(audioTrack, true);
	f.display.showLanes(midiTrack, true);
	REQUIRE(f.waitUntil([&f] { return f.display.laneTakes() == std::vector<int>{-1, 0, 1, -1, 0, 1}; }));

	f.display.setTool(ui::Tool::Draw);
	// A click on the MIDI track's lane 1 at 5 seconds draws the bar there on lane 1, one on its lane 0 on lane 0.
	f.click(500, 220);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 2; }));
	CHECK(f.parts()[1].at("position") == 8 * g_quarter);
	CHECK(f.parts()[1].at("lane") == 1);
	f.click(700, 180);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 3; }));
	CHECK(f.parts()[2].at("lane") == 0);
	// A drag on the audio track's lane 1 draws an audio part there.
	f.drag(400, 100, 550, 100);
	REQUIRE(f.waitUntil([&f] { return f.audioParts().size() == 1; }));
	CHECK(f.audioParts()[0].at("lane") == 1);
	CHECK(f.audioParts()[0].at("track") == audioTrack);
}
