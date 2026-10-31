// SPDX-License-Identifier: GPL-3.0-or-later
// The project window's status line (#407, docs/features/status-line.md) on the dummy device: the record format with a
// sample rate mismatch, and a lost audio device, as they follow audio.changed.
#include "uiapp.h"

#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool has(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json names = element(app, selector).value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	std::string text(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("text", std::string());
	}
}

TEST_CASE("the status line highlights a sample rate mismatch and shows a lost audio device", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz"; }));
	CHECK_FALSE(has(app, "#record-format", "mismatch"));
	CHECK(has(app, "#lost-device", "hidden"));

	// The dummy device doesn't run at 16 kHz, so it stays at 48 kHz and plays the project faster.
	app.call("project.set", {{"sampleRate", 16000.0}});
	CHECK(app.waitUntil([&] { return has(app, "#record-format", "mismatch"); }));
	CHECK(text(app, "#record-format") == "Record: 16 kHz");
	app.call("project.set", {{"sampleRate", 44100.0}}); // it does at 44.1 kHz
	CHECK(app.waitUntil([&] { return !has(app, "#record-format", "mismatch"); }));
	CHECK(text(app, "#record-format") == "Record: 44.1 kHz");

	// Unplugged: shown with the reason as its tooltip, until it is plugged in again.
	app.call("audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(app.waitUntil([&] { return !has(app, "#lost-device", "hidden"); }));
	CHECK_THAT(text(app, "#lost-device"), ContainsSubstring("Audio Device Lost"));
	CHECK_THAT(
		element(app, "#lost-device").at("attributes").at("title").get<std::string>(), ContainsSubstring("unplugged"));
	app.call("audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(app.waitUntil([&] { return has(app, "#lost-device", "hidden"); }, std::chrono::seconds(10)));
}
