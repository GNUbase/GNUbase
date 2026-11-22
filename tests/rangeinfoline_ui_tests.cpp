// SPDX-License-Identifier: GPL-3.0-or-later
// The info line's range fields (#1910, reference manual pp. 287-288, docs/features/inspector.md) in the app's project
// window, driven with ui::Automation as MCP's UI tools drive them: with the range selection tool it shows the selection
// range's start, end and length and its top and bottom track, and its edits set the range.
#include "testutils/testutils.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/eventdisplay.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_second = 48000;

	struct Window
	{
		Window() : app{{1200, 600}}, display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			app.appUi().docking().setVisible("project", "infoLine", true);
			app.shell().step(std::chrono::milliseconds(0));
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

		std::string value(const std::string& selector)
		{
			return element(selector).value("attributes", Json::object()).value("value", std::string());
		}

		bool shows(const std::string& field, const std::string& text)
		{
			return app.waitUntil([&] { return value("#info-line textdisplay[field=" + field + "]") == text; });
		}

		void click(const std::string& selector, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Double-clicks a display, types into it and presses Enter.
		void type(const std::string& field, const std::string& text)
		{
			click("#info-line textdisplay[field=" + field + "]", 2);
			app.ui().key(std::nullopt, "Mod+A");
			app.ui().text(std::nullopt, text);
			app.ui().key(std::nullopt, "Enter");
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Opens a drop-down list, moves its choice by a key and takes it.
		void choose(const std::string& field, const std::string& key)
		{
			click("#info-line select[field=" + field + "]");
			app.ui().key(std::nullopt, key);
			click("#info-line select[field=" + field + "]");
		}

		uint64_t addTrack(const std::string& name)
		{
			return app.call("track.add", {{"type", "midi"}, {"name", name}}).at("track").at("id").get<uint64_t>();
		}

		bool hasRange(int64_t start, int64_t end, const std::vector<uint64_t>& tracks)
		{
			return app.waitUntil(
				[&]
				{
					const std::optional<ui::RangeSelection>& range = display.range();
					return range && range->start == start && range->end == end && range->tracks == tracks;
				});
		}

		test::UiApp app;
		ui::EventDisplay& display;
	};
}

TEST_CASE("the info line shows the selection range and its edits set the range's edges and tracks", "[ui]")
{
	Window window;
	const uint64_t a = window.addTrack("Alpha");
	const uint64_t b = window.addTrack("Beta");
	const uint64_t c = window.addTrack("Gamma");
	const uint64_t part = window.app.call("midiPart.add", {{"track", a}, {"position", 0}, {"length", 960}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	window.app.call("project.set", {{"timeFormat", "seconds"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.shownTracks().size() == 3; }));
	window.display.select({part});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "empty"); }));
	CHECK(window.hasClass("#info-line [info-field=topTrack]", "unavailable"));

	window.display.setTool(ui::Tool::RangeSelection);
	window.display.setRange(ui::RangeSelection{g_second, 3 * g_second, {b}});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#info-line", "range"); }));
	CHECK(window.shows("start", "0:00:01.000"));
	CHECK(window.shows("end", "0:00:03.000"));
	CHECK(window.shows("length", "0:00:02.000"));
	CHECK(window.element("#info-line [info-field=start] .info-label").at("text") == "Range Start");
	CHECK(window.element("#info-line [info-field=length] .info-label").at("text") == "Range Length");
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	for (const char* field : {"start", "end", "length", "topTrack", "bottomTrack"})
		CHECK(!window.hasClass("#info-line [info-field=" + std::string(field) + "]", "unavailable"));
	for (const char* field : {"name", "mute", "lock", "transpose"})
		CHECK(window.hasClass("#info-line [info-field=" + std::string(field) + "]", "unavailable"));

	// Range Start moves the start, the end stays; past the end it changes nothing.
	window.type("start", "0:00:02");
	REQUIRE(window.hasRange(2 * g_second, 3 * g_second, {b}));
	CHECK(window.shows("length", "0:00:01.000"));
	window.type("start", "0:00:05");
	CHECK(window.shows("start", "0:00:02.000"));
	CHECK(window.hasRange(2 * g_second, 3 * g_second, {b}));
	// Range Length and Range End move the end; an end before the start changes nothing.
	window.type("length", "0:00:04");
	REQUIRE(window.hasRange(2 * g_second, 6 * g_second, {b}));
	CHECK(window.shows("end", "0:00:06.000"));
	const size_t steps = window.app.call("history.get").at("steps").size();
	window.type("end", "0:00:01");
	CHECK(window.shows("end", "0:00:06.000"));
	window.type("end", "0:00:05");
	REQUIRE(window.hasRange(2 * g_second, 5 * g_second, {b}));
	// No edit is an undo step: the range isn't part of the project.
	CHECK(window.app.call("history.get").at("steps").size() == steps);

	// Top Track and Bottom Track take the tracks between them and the other edge in or out, not past it.
	window.choose("bottomTrack", "Down");
	REQUIRE(window.hasRange(2 * g_second, 5 * g_second, {b, c}));
	window.choose("topTrack", "Up");
	REQUIRE(window.hasRange(2 * g_second, 5 * g_second, {a, b, c}));
	window.choose("topTrack", "Down");
	REQUIRE(window.hasRange(2 * g_second, 5 * g_second, {b, c}));
	window.choose("bottomTrack", "Up");
	REQUIRE(window.hasRange(2 * g_second, 5 * g_second, {b}));
	window.choose("bottomTrack", "Up"); // above the top track
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.hasRange(2 * g_second, 5 * g_second, {b}));

	// Another tool takes the range away, and selected events show with their own labels.
	window.display.setTool(ui::Tool::ObjectSelection);
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "range"); }));
	window.display.select({part});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass("#info-line", "empty"); }));
	CHECK(window.element("#info-line [info-field=start] .info-label").at("text") == "Start");
	CHECK(window.element("#info-line [info-field=length] .info-label").at("text") == "Length");
	CHECK(window.hasClass("#info-line [info-field=bottomTrack]", "unavailable"));
}

TEST_CASE("Paste at Origin is in the Edit menu's Functions submenu", "[ui]")
{
	// The reference manual, p. 270: Edit > Functions > Paste at Origin.
	Window window;
	window.element("#functions-submenu menuitem[command=\"edit.pasteAtOrigin\"]");
	CHECK(window.app.ui()
			  .dom(std::nullopt, "#edit-menu menuitem[command=\"edit.pasteAtOrigin\"]", 0)
			  .at("elements")
			  .size() == 1);
}
