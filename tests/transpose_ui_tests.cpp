// SPDX-License-Identifier: GPL-3.0-or-later
// The project root key and the transpose track in the project window (#168, docs/features/transpose.md), driven with
// ui::Automation as MCP's UI tools drive them: the toolbar's Project Root Key menu, the info line's Root Key and
// Global Transpose of MIDI parts, and the transpose events on the transpose track's lane.
#include "testutils/testutils.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/rmlui.h"

#include <RmlUi/Core/Core.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	// The project window at a dp ratio of 0.5 with the info line shown: 2000 dp wide, enough for every field of the
	// info line, in a window that fits on the test machines' screens.
	struct Window
	{
		Window() : display(app.appUi().showProject())
		{
			app.setDpRatio(0.5F);
			app.appUi().docking().setVisible("project", "infoLine", true);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		Json element(const std::string& selector)
		{
			const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
			REQUIRE(found.size() == 1);
			return found[0];
		}

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json classes = element(selector).value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json part(uint64_t id)
		{
			const Json project = call("project.get");
			for (const Json& part : project.at("midiParts"))
				if (part.at("id") == id)
					return part;
			return Json::object();
		}

		test::UiApp app{{1000, 600}};
		ui::EventDisplay& display;
	};
}

TEST_CASE("the toolbar sets the project root key, the info line a part's root key and Global Transpose", "[ui]")
{
	Window window;
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = window.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 4 * g_quarter}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();

	window.click("#root-key");
	window.click("#root-key-menu menuitem[root-key=D]");
	REQUIRE(window.app.waitUntil([&] { return window.call("project.get").at("rootKey") == "D"; }));
	CHECK(window.app.waitUntil([&] { return window.element("#root-key").value("text", std::string()) == "D"; }));
	// From another client too.
	window.call("project.set", {{"rootKey", "F#"}});
	CHECK(window.app.waitUntil([&] { return window.element("#root-key").value("text", std::string()) == "F#"; }));

	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "empty"); }));
	CHECK_FALSE(window.hasClass("#info-line [info-field=rootKey]", "unavailable"));
	// The drop-down list's next key after none is C.
	window.click("#info-line select[field=rootKey]");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#info-line select[field=rootKey]");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("rootKey") == "C"; }));
	// The Transpose field shows what the project root key adds (p. 420): a part in C in a project in D, +2.
	const auto step = [&window]
	{ return window.element("#info-line [field=rootKeyTranspose]").value("text", std::string()); };
	window.call("project.set", {{"rootKey", "D"}});
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("rootKeyTranspose") == 2; }));
	CHECK(window.app.waitUntil([&] { return step() == "(+2)"; }));
	// Independent parts show a mark in their upper right corner (p. 421), and nothing that the root key adds.
	const std::string mark = ".part-mark.independent[part=" + std::to_string(part) + "]";
	CHECK(window.app.ui().dom(std::nullopt, mark, 0).at("elements").empty());
	// The Global Transpose button reads the value it holds: Follow, then Independent while it is checked (p. 421).
	const auto globalTransposeLabel = [&window]
	{ return window.element("#info-line button[field=globalTranspose]").value("text", std::string()); };
	CHECK(globalTransposeLabel() == "Follow");
	window.click("#info-line button[field=globalTranspose]");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("globalTranspose") == "independent"; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass("#info-line button[field=globalTranspose]", "checked"); }));
	CHECK(window.app.waitUntil([&] { return globalTransposeLabel() == "Independent"; }));
	CHECK(window.app.waitUntil([&] { return step().empty(); }));
	REQUIRE(
		window.app.waitUntil([&] { return window.app.ui().dom(std::nullopt, mark, 0).at("elements").size() == 1; }));
	CHECK(window.element(mark).at("visible") == true);
	window.click("#info-line button[field=globalTranspose]");
	CHECK(window.app.waitUntil([&] { return window.app.ui().dom(std::nullopt, mark, 0).at("elements").empty(); }));
	CHECK(window.app.waitUntil([&] { return globalTransposeLabel() == "Follow"; }));
}

TEST_CASE("the info line sets an audio event's root key and Global Transpose", "[ui]")
{
	Window window;
	const gnubase::test::TempDir dir;
	test::Audio audio;
	audio.samples.assign(48000, 0.0F);
	const auto file = dir.path() / "silence.wav";
	test::writeWav(file, audio);
	const auto track = window.call("track.add", {{"type", "audio"}, {"channels", 1}}).at("track").at("id");
	const auto media = window.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
	const auto clip = window.call("clip.add", {{"media", media}}).at("clip").at("id");
	const auto event =
		window.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 48000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	const auto audioEvent = [&window, event]
	{
		const Json project = window.call("project.get");
		for (const Json& each : project.at("audioEvents"))
			if (each.at("id") == event)
				return each;
		return Json::object();
	};
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "empty"); }));
	// An audio event has a root key and Global Transpose as a MIDI part has (#1368, reference manual pp. 416, 421).
	CHECK_FALSE(window.hasClass("#info-line [info-field=rootKey]", "unavailable"));
	CHECK_FALSE(window.hasClass("#info-line [info-field=globalTranspose]", "unavailable"));
	window.click("#info-line select[field=rootKey]");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#info-line select[field=rootKey]");
	REQUIRE(window.app.waitUntil([&] { return audioEvent().at("rootKey") == "C"; }));
	const std::string mark = ".part-mark.independent[part=" + std::to_string(event) + "]";
	CHECK(window.app.ui().dom(std::nullopt, mark, 0).at("elements").empty());
	window.click("#info-line button[field=globalTranspose]");
	REQUIRE(window.app.waitUntil([&] { return audioEvent().at("globalTranspose") == "independent"; }));
	// Independent events show the mark in their upper right corner, as parts do (p. 421).
	CHECK(window.app.waitUntil([&] { return window.app.ui().dom(std::nullopt, mark, 0).at("elements").size() == 1; }));
	CHECK(window.app.waitUntil(
		[&]
		{
			return window.element("#info-line button[field=globalTranspose]").value("text", std::string()) ==
				"Independent";
		}));
}

TEST_CASE("the info line sets an audio event's own Transpose and shows what the project root key adds", "[ui]")
{
	Window window;
	const gnubase::test::TempDir dir;
	test::Audio audio;
	audio.samples.assign(48000, 0.0F);
	const auto file = dir.path() / "silence.wav";
	test::writeWav(file, audio);
	const auto track = window.call("track.add", {{"type", "audio"}, {"channels", 1}}).at("track").at("id");
	const auto media = window.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
	const auto clip = window.call("clip.add", {{"media", media}}).at("clip").at("id");
	const auto event =
		window.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 48000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	const auto audioEvent = [&window, event]
	{
		const Json project = window.call("project.get");
		for (const Json& each : project.at("audioEvents"))
			if (each.at("id") == event)
				return each;
		return Json::object();
	};
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "empty"); }));
	// An audio event's Transpose (#1555, reference manual p. 420), within the stretcher's range of 24 semitones.
	CHECK_FALSE(window.hasClass("#info-line [info-field=transpose]", "unavailable"));
	CHECK(window.element("#info-line valuefield[field=transpose]").at("attributes").at("max") == "24");
	// A double click edits it as text, as the volume.
	window.app.ui().click(
		std::nullopt, {"#info-line valuefield[field=transpose]", 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "5");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return audioEvent().at("transpose") == 5; }));
	// The step the project root key adds, in brackets: an event in C in a project in D.
	window.call("audioEvent.set", {{"event", event}, {"rootKey", "C"}});
	window.call("project.set", {{"rootKey", "D"}});
	CHECK(window.app.waitUntil(
		[&] { return window.element("#info-line [field=rootKeyTranspose]").value("text", std::string()) == "(+2)"; }));
	CHECK(window.app.waitUntil(
		[&]
		{
			return window.element("#info-line valuefield[field=transpose]").at("attributes").value("value", "") == "5";
		}));
}

TEST_CASE("the info line sets the root key and Global Transpose of an audio part's events", "[ui]")
{
	Window window;
	const gnubase::test::TempDir dir;
	test::Audio audio;
	audio.samples.assign(48000, 0.0F);
	const auto file = dir.path() / "silence.wav";
	test::writeWav(file, audio);
	const auto track = window.call("track.add", {{"type", "audio"}, {"channels", 1}}).at("track").at("id");
	const auto media = window.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
	const auto clip = window.call("clip.add", {{"media", media}}).at("clip").at("id");
	Json events = Json::array();
	for (const int64_t at : {0, 48000})
		events.push_back(
			window.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", at}, {"length", 24000}})
				.at("event")
				.at("id"));
	const auto part = window.call("event.toPart", {{"events", events}}).at("events")[0].get<uint64_t>();
	const auto keys = [&window]
	{
		std::vector<std::string> result;
		const Json project = window.call("project.get");
		for (const Json& each : project.at("audioEvents"))
			result.push_back(
				each.at("rootKey").get<std::string>() + " " + each.at("globalTranspose").get<std::string>());
		return result;
	};
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "empty"); }));
	// An audio part has no root key of its own (reference manual p. 417): the info line shows and sets its events'.
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line [info-field=rootKey]", "unavailable"); }));
	CHECK_FALSE(window.hasClass("#info-line [info-field=globalTranspose]", "unavailable"));
	window.click("#info-line select[field=rootKey]");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#info-line select[field=rootKey]");
	REQUIRE(window.app.waitUntil([&] { return keys() == std::vector<std::string>{"C follow", "C follow"}; }));
	const std::string mark = ".part-mark.independent[part=" + std::to_string(part) + "]";
	const auto marked = [&] { return window.app.ui().dom(std::nullopt, mark, 0).at("elements").size() == 1; };
	CHECK_FALSE(marked());
	window.click("#info-line button[field=globalTranspose]");
	REQUIRE(window.app.waitUntil([&] { return keys() == std::vector<std::string>{"C independent", "C independent"}; }));
	// A part whose events are Independent shows the mark (p. 421, #1644).
	CHECK(window.app.waitUntil(marked));
	// One undo step for both.
	window.call("history.undo");
	CHECK(window.app.waitUntil([&] { return keys() == std::vector<std::string>{"C follow", "C follow"}; }));
	CHECK(window.app.waitUntil([&] { return !marked(); }));
	// Events whose root keys or Global Transpose differ show an empty value; "-" clears their keys (#1644).
	const Json second = events[1];
	window.call("audioEvent.set", {{"event", second}, {"rootKey", "E"}, {"globalTranspose", "independent"}});
	REQUIRE(window.app.waitUntil(
		[&] { return window.element("#info-line select[field=rootKey]").value("value", std::string()) == "*"; }));
	CHECK(window.element("#info-line button[field=globalTranspose]").value("text", std::string()).empty());
	// Its title names no value either (#1719).
	CHECK(window.element("#info-line button[field=globalTranspose]").at("attributes").value("title", "") ==
		"Global Transpose");
	CHECK_FALSE(window.hasClass("#info-line button[field=globalTranspose]", "checked"));
	CHECK_FALSE(marked()); // only one of them is Independent
	window.click("#info-line select[field=rootKey]");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#info-line select[field=rootKey]");
	CHECK(window.app.waitUntil([&] { return keys() == std::vector<std::string>{" follow", " independent"}; }));
}

TEST_CASE("the info line's own values don't change a MIDI part and an audio event selected together", "[ui]")
{
	Window window;
	const gnubase::test::TempDir dir;
	test::Audio audio;
	audio.samples.assign(48000, 0.0F);
	const auto file = dir.path() / "silence.wav";
	test::writeWav(file, audio);
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = window.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 4 * g_quarter}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	window.call("midiPart.set", {{"part", part}, {"rootKey", "G"}});
	const auto track = window.call("track.add", {{"type", "audio"}, {"channels", 1}}).at("track").at("id");
	const auto media = window.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
	const auto clip = window.call("clip.add", {{"media", media}}).at("clip").at("id");
	const auto event =
		window.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 48000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	window.call("audioEvent.set", {{"event", event}, {"rootKey", "C"}});
	const auto keys = [&window, part, event]
	{
		std::string result = window.part(part).value("rootKey", std::string("?"));
		const Json project = window.call("project.get");
		for (const Json& each : project.at("audioEvents"))
			if (each.at("id") == event)
				result += " " + each.at("rootKey").get<std::string>();
		return result;
	};
	REQUIRE(keys() == "G C");
	REQUIRE(window.app.waitUntil(
		[&]
		{
			const Json project = window.display.project();
			return project.at("midiParts").size() == 1 && project.at("audioEvents").size() == 1;
		}));
	// The info line shows the first one's key, which a change of its list while it shows them doesn't give the other
	// one (#1719).
	window.display.select({part, event});
	REQUIRE(window.app.waitUntil(
		[&] { return !window.element("#info-line select[field=rootKey]").value("value", std::string()).empty(); }));
	CHECK_FALSE(window.app.waitUntil([&] { return keys() != "G C"; }, std::chrono::milliseconds(500)));
	// Nor after an undo, which shows them again.
	window.call("audioEvent.set", {{"event", event}, {"transpose", 3}});
	window.call("history.undo");
	CHECK_FALSE(window.app.waitUntil([&] { return keys() != "G C"; }, std::chrono::milliseconds(500)));
}

TEST_CASE("the event display says why the engine refused a glue of parts that follow transpose differently", "[ui]")
{
	Window window;
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto add = [&window, midi](int64_t at, const std::string& key)
	{
		const auto part = window.call("midiPart.add", {{"track", midi}, {"position", at}, {"length", 4 * g_quarter}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
		window.call("midiPart.set", {{"part", part}, {"rootKey", key}});
		return part;
	};
	// In a project in E, the part in C plays 4 semitones up and the one in D 2.
	window.call("project.set", {{"rootKey", "E"}});
	const uint64_t first = add(0, "C");
	const uint64_t second = add(4 * g_quarter, "D");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));
	CHECK(window.element("#edit-refused").at("visible") == false);
	window.display.select({first, second});
	window.display.actions().at("edit.glue")();
	// The engine's reason shows for a moment over the lanes (#1367); the parts stay as they were.
	REQUIRE(window.app.waitUntil([&] { return window.element("#edit-refused").at("visible") == true; }));
	CHECK(window.element("#edit-refused").value("text", std::string()).find("follow global transpose differently") !=
		std::string::npos);
	CHECK(window.call("project.get").at("midiParts").size() == 2);
}

TEST_CASE("the transpose track's context menu sets the root key for unassigned events", "[ui]")
{
	Window window;
	const auto transpose = window.call("track.add", {{"type", "transpose"}}).at("track").at("id").get<uint64_t>();
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = window.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 4 * g_quarter}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	const std::string item = R"(#track-list-menu > [command="transpose.assignRootKey"])";
	const auto menu = [&window, &item](uint64_t track)
	{
		window.app.ui().key(std::nullopt, "Escape");
		window.app.ui().click(std::nullopt,
			{"#track-list listitem[value=\"" + std::to_string(track) + "\"] .track-name", 0, std::nullopt},
			ui::MouseButton::Right, 1, {});
		window.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(window.element("#track-list-menu").at("visible") == true);
		return !window.element(item).at("attributes").contains("disabled");
	};
	// The transpose track needs a project root key before its events can be assigned one.
	REQUIRE(window.app.waitUntil([&] { return window.display.laneTracks().size() == 2; }));
	CHECK_FALSE(menu(transpose));
	window.call("project.set", {{"rootKey", "E"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("rootKey") == "E"; }));
	// Only for the transpose track.
	CHECK_FALSE(menu(midi));
	REQUIRE(menu(transpose));
	window.click(item);
	CHECK(window.app.waitUntil([&] { return window.part(part).at("rootKey") == "E"; }));
}

TEST_CASE("the transpose track's lane draws, edits and erases transpose events", "[ui]")
{
	Window window;
	const auto track = window.call("track.add", {{"type", "transpose"}}).at("track").at("id").get<uint64_t>();
	const auto events = [&window] { return window.call("transpose.get").at("events"); };
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".transpose-row", "hidden"); }));

	// The draw tool adds an event at the nearest bar and opens it: Enter takes the value typed.
	window.display.setTool(ui::Tool::Draw);
	window.click(".transpose-row");
	REQUIRE(window.app.waitUntil([&] { return events().size() == 1; }));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".transpose-field", "shown"); }));
	CHECK(events()[0].at("position").get<int64_t>() % (4 * g_quarter) == 0);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "5");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return events()[0].at("value") == 5; }));
	CHECK(
		window.app.waitUntil([&] { return window.element(".transpose-event").value("text", std::string()) == "+5"; }));

	// The track list's controls: Mute Transpose Events, Keep Transpose in Octave Range and Lock.
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	for (const auto& [control, key] : {std::pair{"transposeMute", "mute"}, std::pair{"keepInOctave", "keepInOctave"},
			 std::pair{"transposeLock", "lock"}})
	{
		window.click(row + " button[control=" + control + "]");
		CHECK(window.app.waitUntil([&window, key = key] { return window.call("transpose.get").at(key) == true; }));
	}
	CHECK(window.app.waitUntil([&] { return window.hasClass(row + " button[control=keepInOctave]", "checked"); }));
	window.call("transpose.setTrack", {{"lock", false}});

	// The erase tool removes the event clicked.
	window.display.setTool(ui::Tool::Erase);
	window.click(".transpose-event");
	CHECK(window.app.waitUntil([&] { return events().empty(); }));
}

namespace
{
	// A transpose track with an event at the project start, with the pointer tool.
	uint64_t addEvent(Window& window)
	{
		window.call("track.add", {{"type", "transpose"}});
		const auto event = window.call("transpose.add", {{"position", 0}, {"value", 3}}).at("event").at("id");
		REQUIRE(window.app.waitUntil(
			[&] { return window.app.ui().dom(std::nullopt, ".transpose-event", 0).at("elements").size() == 1; }));
		window.display.setTool(ui::Tool::ObjectSelection);
		return event.get<uint64_t>();
	}
}

TEST_CASE("dragging a transpose event moves it to the nearest bar", "[ui]")
{
	Window window;
	addEvent(window);
	// Three fifths of a bar (2 seconds at 120 BPM) to the right: onto the second bar's line.
	const Json box = window.element(".transpose-event").at("box");
	const double x = box.at("x").get<double>() + (box.at("width").get<double>() / 2.0);
	const double y = box.at("y").get<double>() + (box.at("height").get<double>() / 2.0);
	const double across = 1.2 / window.display.view().scale * 0.5; // pixels, at a dp ratio of 0.5
	const double bar = 2.0 / window.display.view().scale * 0.5;    // pixels
	const ui::Point to{static_cast<int>(std::lround(x + across)), static_cast<int>(std::lround(y))};
	const auto followed = [&]
	{
		return std::abs(window.element(".transpose-event").at("box").at("x").get<double>() -
				   (box.at("x").get<double>() + bar)) < 2.0;
	};
	const auto position = [&window] { return window.call("transpose.get").at("events").at(0).at("position"); };
	// While the button is down, the event follows the mouse to the nearest bar; the release moves it (#1696).
	window.app.ui().press(std::nullopt, {".transpose-event", 0, std::nullopt}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, {"", 0, to});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.app.waitUntil(followed));
	CHECK(position() == 0);
	window.app.ui().release(std::nullopt, {"", 0, to}, ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return position() == 4 * g_quarter; }));
	CHECK(followed());
}

TEST_CASE("a double click on a transpose event opens its value for typing", "[ui]")
{
	Window window;
	addEvent(window);
	window.app.ui().click(std::nullopt, {".transpose-event", 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".transpose-field", "shown"); }));
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "-7");
	window.app.ui().key(std::nullopt, "Enter");
	CHECK(window.app.waitUntil([&] { return window.call("transpose.get").at("events").at(0).at("value") == -7; }));
	CHECK(window.call("transpose.get").at("events").at(0).at("position") == 0);
}

TEST_CASE("two presses on a transpose event open it only less than half a second apart", "[ui]")
{
	Window window;
	addEvent(window);
	// RmlUi's clock, which times the presses, stands still but for advance().
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });

	clock->advance(1.0);
	window.click(".transpose-event");
	clock->advance(0.6);
	window.click(".transpose-event");
	CHECK(!window.hasClass(".transpose-field", "shown"));
	clock->advance(0.4);
	window.click(".transpose-event");
	CHECK(window.app.waitUntil([&] { return window.hasClass(".transpose-field", "shown"); }));
}
