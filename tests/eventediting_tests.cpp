// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the event display (#85) on the fake window system, following a real engine: selecting,
// moving, copying, resizing and splitting with the mouse, adding parts with a double click, and the edit commands on
// the selection. tests/eventedit_tests.cpp checks the event.* commands themselves.
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

#include <chrono>
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

	// Nudging moves the part a quarter; duplicating selects the copy right after it; moving its start to the cursor.
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
