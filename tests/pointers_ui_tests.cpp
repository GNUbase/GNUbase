// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the mouse pointer by context (#469, #1857, docs/features/pointers.md): hovering an element through
// ui::Automation shows the pointer that says what a drag or click there does, as MCP's ui_windows reports the one the
// window system was asked for. The event lanes' other tool pointers are in uiapp_tests.cpp and eventediting_tests.cpp,
// the sample and audio part editors' in their own tests.
#include "uiapp.h"

#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	ui::Target point(int x, int y)
	{
		return {"", 0, ui::Point{x, y}};
	}

	// The middle of the one element the selector matches, in the window's pixels.
	ui::Target middle(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		const Json& box = found[0].at("box");
		return point(box.at("x").get<int>() + (box.at("width").get<int>() / 2),
			box.at("y").get<int>() + (box.at("height").get<int>() / 2));
	}

	// The pointer that shows, by the name the style sheets give it: empty for the OS's arrow.
	std::string pointer(test::UiApp& app)
	{
		app.shell().step(std::chrono::milliseconds(0));
		return app.ui().windows().at("cursor").get<std::string>();
	}

	std::string pointerOver(test::UiApp& app, const ui::Target& target)
	{
		app.ui().move(std::nullopt, target);
		return pointer(app);
	}
}

TEST_CASE("the time warp tool shows a clock with arrows over the event lanes", "[ui]")
{
	test::UiApp app({800, 300});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.call("track.add", {{"type", "midi"}});
	app.shell().step(std::chrono::milliseconds(0));
	const ui::Target lanes = middle(app, "event-lanes.events");
	app.keys().run("tool.timeWarp", Json::object(), app.main(), nullptr);
	CHECK(pointerOver(app, lanes) == "tool-timewarp");
}
