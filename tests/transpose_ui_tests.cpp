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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

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
	CHECK_FALSE(window.hasClass("#info-line [info-field=rootKey]", "hidden"));
	// The drop-down list's next key after none is C.
	window.click("#info-line select[field=rootKey]");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#info-line select[field=rootKey]");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("rootKey") == "C"; }));
	window.click("#info-line button[field=globalTranspose]");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("globalTranspose") == "independent"; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass("#info-line button[field=globalTranspose]", "checked"); }));
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
	window.app.ui().key(std::nullopt, "Ctrl+A");
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
