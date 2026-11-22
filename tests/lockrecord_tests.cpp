// SPDX-License-Identifier: GPL-3.0-or-later
// Lock Record (#104, reference manual p. 356, docs/features/recording.md) in the app's UI: the Transport menu locks
// and unlocks recording, and stopping or punching out a locked recording asks first.
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
	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		REQUIRE(app.waitUntil([&app, &selector] { return shown(app, selector); }));
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void transportMenu(test::UiApp& app, const std::string& item)
	{
		click(app, "#menu-bar button[menu=transport-menu]");
		click(app, item);
	}

	Json transport(test::UiApp& app)
	{
		return app.call("transport.get");
	}
}

TEST_CASE("stopping a locked recording asks first, and Unlock Record stops it at once again", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	transportMenu(app, "#lock-record");
	REQUIRE(app.waitUntil([&] { return transport(app).at("lockRecord").get<bool>(); }));
	app.call("transport.record");

	// Stop asks; Cancel goes on recording.
	app.ui().key(std::nullopt, "Num0");
	REQUIRE(app.waitUntil([&] { return shown(app, "#lock-record-dialog"); }));
	click(app, "#lock-record-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&] { return !shown(app, "#lock-record-dialog"); }));
	CHECK(transport(app).at("recording") == true);

	// Record asks to punch out; Punch Out ends the recording, and playback goes on (#645).
	app.ui().key(std::nullopt, "Num*");
	REQUIRE(app.waitUntil([&] { return shown(app, "#lock-record-dialog"); }));
	CHECK(app.ui().dom(std::nullopt, "#lock-record-confirm", 0).at("elements")[0].value("text", "") == "Punch Out");
	click(app, "#lock-record-confirm");
	REQUIRE(app.waitUntil([&] { return !transport(app).at("recording").get<bool>(); }));
	CHECK(transport(app).at("playing") == true);
	app.call("transport.record");

	// Start/Stop asks to stop; Stop stops, and the lock stays.
	app.ui().key(std::nullopt, "Space");
	REQUIRE(app.waitUntil([&] { return shown(app, "#lock-record-dialog"); }));
	CHECK(app.ui().dom(std::nullopt, "#lock-record-confirm", 0).at("elements")[0].value("text", "") == "Stop");
	click(app, "#lock-record-confirm");
	REQUIRE(app.waitUntil([&] { return !transport(app).at("playing").get<bool>(); }));
	CHECK(!shown(app, "#lock-record-dialog"));
	CHECK(transport(app).at("lockRecord") == true);

	// Unlocked, Stop stops without asking.
	transportMenu(app, "#unlock-record");
	REQUIRE(app.waitUntil([&] { return !transport(app).at("lockRecord").get<bool>(); }));
	app.call("transport.record");
	app.ui().key(std::nullopt, "Num0");
	REQUIRE(app.waitUntil([&] { return !transport(app).at("playing").get<bool>(); }));
	CHECK(!shown(app, "#lock-record-dialog"));
}
