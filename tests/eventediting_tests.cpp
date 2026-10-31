// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the event display (#85) on the fake window system, following a real engine: selecting,
// moving, copying, resizing and splitting with the mouse, adding parts with a double click, and the edit commands on
// the selection; snapping them (#87). tests/eventedit_tests.cpp checks the event.* commands themselves.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
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
	// Between the locators, a part from one to the other; never on an audio track.
	f.call("transport.set", {{"left", 408000}, {"right", 456000}}); // 8.5 to 9.5 seconds
	f.step();
	f.click(900, 60, {}, 2);
	REQUIRE(f.waitUntil([&f] { return f.parts().size() == 4; }));
	CHECK(f.parts()[3].at("position") == 17 * g_quarter);
	CHECK(f.parts()[3].at("length") == 2 * g_quarter);
	f.click(700, 20, {}, 2);
	f.step();
	CHECK(f.audioEvents().size() == 1);
	CHECK(f.parts().size() == 4);
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
	REQUIRE(f.waitUntil([&f] { return f.display.snap().grid == engine::GridType::Beat; }));
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
	REQUIRE(f.waitUntil([&f] { return f.display.snap().grid == engine::GridType::Beat; }));
	f.drag(500, 60, 560, 60);
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 9 * g_quarter; }));

	// Nudging moves a grid step, snap on or off: a beat, then a bar.
	f.display.select({f.part});
	f.run("edit.nudgeLeft");
	REQUIRE(f.waitUntil([&f] { return f.parts()[0].at("position") == 8 * g_quarter; }));
	f.run("edit.snap");
	REQUIRE(f.waitUntil([&f] { return !f.display.snap().on; }));
	f.call("snap.set", {{"grid", "bar"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().grid == engine::GridType::Bar; }));
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
	// A position marker at 6 seconds has no end; the display lists the markers again after every change.
	f.call("marker.add", {{"position", 12 * g_quarter}});
	f.step();
	f.call("marker.list"); // the display has asked for the markers
	f.step();
	f.call("snap.set", {{"on", true}, {"type", "events"}});
	REQUIRE(f.waitUntil([&f] { return f.display.snap().on && f.display.snap().type == engine::SnapType::Events; }));
	// The audio event dragged from 2 to 5.95 seconds starts on the marker (8 dp attract).
	f.drag(225, 20, 620, 20);
	REQUIRE(f.waitUntil([&f] { return f.audioEvents()[0].at("position") == 288000; }));
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
	CHECK(f.display.tool() == ui::Tool::Play);
	for (const char* id : {"tool.objectSelection", "tool.rangeSelection", "tool.split", "tool.glue", "tool.erase",
			 "tool.zoom", "tool.mute", "tool.draw", "tool.line", "tool.play", "tool.next", "tool.previous",
			 "tool.combineSelectionTools"})
		CHECK(f.display.actions().count(id) == 1);
	f.run("tool.combineSelectionTools");
	CHECK(f.display.combineSelectionTools());
	// Tools that don't use the cross-hair cursor hide it (reference manual p. 110).
	f.run("tool.mute");
	CHECK(!root->IsClassSet("cross-hair"));
	f.run("tool.split");
	CHECK(root->IsClassSet("cross-hair"));
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
	// A double click undoes the last zoom.
	f.click(500, 60, {}, 2);
	CHECK(f.display.view().scale == before.scale);
	CHECK(std::abs(f.display.view().start - before.start) < 1e-9);
}

TEST_CASE("the draw tool draws parts on MIDI tracks, snapped; the line tool waits for automation tracks", "[unit]")
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
	// Nothing on audio tracks until audio parts come; the line tool draws on automation tracks (#121).
	f.drag(400, 20, 550, 20);
	f.display.setTool(ui::Tool::Line);
	f.display.setLineMode(ui::LineMode::Sine);
	CHECK(f.display.lineMode() == ui::LineMode::Sine);
	CHECK(f.document.GetElementById("display")->GetAttribute<std::string>("line-mode", "") == "sine");
	f.drag(630, 60, 960, 60);
	f.call("project.get");
	f.step();
	CHECK(f.parts().size() == 4);
	CHECK(f.audioEvents().size() == 1);
}

TEST_CASE("the play tool plays from where it presses until it is released", "[unit]")
{
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	f.call("audio.start");
	std::vector<std::pair<bool, int64_t>> states; // playing, position
	const ui::Subscription subscription = f.binding.on("transport.changed", [&states](const engine::Event& event)
		{ states.emplace_back(event.data.at("playing").get<bool>(), event.data.at("position").get<int64_t>()); });
	const auto started = [&states]
	{ return std::find_if(states.begin(), states.end(), [](const auto& state) { return state.first; }); };
	f.display.setTool(ui::Tool::Play);
	f.click(300, 60);
	REQUIRE(f.waitUntil([&states, &started] { return started() != states.end(); }));
	CHECK(started()->second >= 144000);
	REQUIRE(f.waitUntil([&f] { return f.call("transport.get").at("playing") == false; }));
	CHECK(f.parts().size() == 1); // nothing edited
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
	CHECK(root->GetAttribute<std::string>("range", "") == "");
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
}
