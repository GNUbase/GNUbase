// SPDX-License-Identifier: GPL-3.0-or-later
// The right zone's meter display (#590, docs/features/metering.md) in the app's project window, driven with
// ui::Automation as MCP's UI tools drive it: the master meter and the loudness meter show what the engine measures
// of the main mix, read once a frame, and each page's reset starts its own values over.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, ".meter-display " + selector, 0).at("elements");
	}

	std::string text(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return found.empty() ? std::string() : found[0].value("text", std::string());
	}

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return !found.empty() && found[0].at("visible").get<bool>();
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {".meter-display " + selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	}
}

TEST_CASE("the meter display shows the main mix's levels and loudness, and each page resets its own", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	// A stereo track that monitors a 1 kHz sine at -6 dBFS on both sides.
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});

	// The Meter tab of the right zone (reference manual p. 535) opens on the master meter, before anything played.
	app.appUi().docking().open("meter");
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-master") && text(app, ".peak-max") == "-inf dB"; }));
	CHECK_FALSE(shown(app, ".meter-loudness"));
	CHECK(text(app, ".integrated") == "--");

	// A second of playback: a true peak bar per channel of the main mix with its RMS level, and the maxima (pp. 533
	// and 535). The bars show the true peak since the last frame, so the first frame after playing shows it.
	app.call("transport.play");
	app.call("audio.render", {{"frames", 48000}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json bars = elements(app, ".meter-master meter");
			return text(app, ".peak-max") == "-6.0 dB" && bars.size() == 2 &&
				bars[1].at("attributes").value("value", std::string()) == "-6.0";
		}));
	CHECK(text(app, ".rms-max") == "-9.0 dB");
	const Json bars = elements(app, ".meter-master meter");
	REQUIRE(bars.size() == 2);
	CHECK(bars[0].at("attributes").at("value") == "-6.0");
	CHECK(bars[1].at("attributes").at("rms") == "-9.0");
	CHECK(bars[1].at("attributes").at("peak") == "-6.0");

	// The master meter's reset clears RMS Max. and Peak Max. (p. 535) and keeps the loudness measurement.
	click(app, ".meter-master .meter-reset");
	REQUIRE(app.waitUntil([&app] { return text(app, ".peak-max") == "-inf dB"; }));
	CHECK(text(app, ".rms-max") == "-inf dB");
	CHECK(text(app, ".integrated") == "-6.0 LUFS");
	CHECK(text(app, ".time") == "0:01");
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&app] { return text(app, ".peak-max") == "-6.0 dB"; }));

	// The loudness meter's page (pp. 536-538), from the button at the bottom.
	click(app, ".meter-tab[value=loudness]");
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-loudness") && !shown(app, ".meter-master"); }));
	CHECK(text(app, ".integrated") == "-6.0 LUFS");
	CHECK(text(app, ".momentary-max") == "-6.0 LUFS");
	CHECK(text(app, ".true-peak-max") == "-6.0 dBTP");
	CHECK(text(app, ".time") == "0:01");
	CHECK(elements(app, ".meter-loudness meter.momentary").at(0).at("attributes").at("value") == "-6.0");

	// Reset Loudness starts the loudness measurement over (p. 538) and keeps the master meter's maxima.
	click(app, ".meter-loudness .meter-reset");
	REQUIRE(app.waitUntil([&app] { return text(app, ".integrated") == "--"; }));
	CHECK(text(app, ".time") == "0:00");
	CHECK(text(app, ".true-peak-max") == "-inf dBTP");
	CHECK(text(app, ".peak-max") == "-6.0 dB");
	click(app, ".meter-tab[value=master]");
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-master"); }));
}

TEST_CASE("the master meter's peak marks and Peak Max. hold the true peak", "[ui]")
{
	// An 8 kHz sine at 48 kHz: its true peak is -6.0 dBTP, its samples reach -7.3 dBFS at most.
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 8000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.appUi().docking().open("meter");
	// Past the overshoot of the sine's sudden start between the samples.
	app.call("audio.render", {{"frames", 4800}});
	app.call("meter.reset", {{"meters", "main"}});
	app.call("audio.render", {{"frames", 48000}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json bars = elements(app, ".meter-master meter");
			return text(app, ".peak-max") == "-6.0 dB" && bars.size() == 2 &&
				bars[0].at("attributes").value("value", std::string()) == "-6.0";
		}));
	const Json bars = elements(app, ".meter-master meter");
	CHECK(bars[0].at("attributes").at("peak") == "-6.0");
	CHECK(bars[1].at("attributes").at("peak") == "-6.0");
}
