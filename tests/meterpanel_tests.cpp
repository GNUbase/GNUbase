// SPDX-License-Identifier: GPL-3.0-or-later
// The right zone's meter display (#590, docs/features/metering.md) in the app's project window, driven with
// ui::Automation as MCP's UI tools drive it: the master meter and the loudness meter show what the engine measures
// of the main mix, read once a frame, and each page's reset starts its own values over; the mixer's channel meters
// (#636) share the frames they read.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/meterpanel.h"
#include "ui/shell.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

	void click(test::UiApp& app, const std::string& selector, const std::vector<std::string>& modifiers = {})
	{
		app.ui().click(
			std::nullopt, {".meter-display " + selector, 0, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
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
	REQUIRE(
		app.waitUntil([&app] { return text(app, ".peak-max") == "-6.0 dB" && text(app, ".rms-max") != "-inf dB"; }));

	// Ctrl-click resets RMS Max. or Peak Max. alone (p. 535); a plain click doesn't.
	click(app, ".rms-max");
	app.call("meter.get"); // after what the click would have sent
	for (int step = 0; step < 3; ++step)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(text(app, ".rms-max") != "-inf dB");
	click(app, ".rms-max", {"ctrl"});
	REQUIRE(app.waitUntil([&app] { return text(app, ".rms-max") == "-inf dB"; }));
	CHECK(text(app, ".peak-max") == "-6.0 dB");
	click(app, ".peak-max", {"ctrl"});
	REQUIRE(app.waitUntil([&app] { return text(app, ".peak-max") == "-inf dB"; }));
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(
		app.waitUntil([&app] { return text(app, ".peak-max") == "-6.0 dB" && text(app, ".rms-max") != "-inf dB"; }));

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

TEST_CASE("the mixer's channel meters and the meter display read the same frames", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	const auto midi = app.call("track.add", {{"type", "midi"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.appUi().docking().open("meter");
	app.appUi().docking().open("mixer");
	const std::string strip = ".mixer-channel[channel=" + std::to_string(channel) + "] ";
	const auto mixer = [&app, &strip](const std::string& selector)
	{ return app.ui().dom(std::nullopt, strip + selector, 0).at("elements"); };
	// Nothing played yet: no bars, and the peak level shows silence.
	REQUIRE(app.waitUntil([&mixer] { return mixer(".mixer-peak").size() == 1; }));
	CHECK(mixer(".mixer-meter meter").empty());
	CHECK(mixer(".mixer-peak").at(0).value("text", std::string()) == "-inf");
	// MIDI channels have no meter yet.
	CHECK(app.ui()
			.dom(std::nullopt, ".mixer-channel[channel=" + std::to_string(midi) + "] .mixer-meter", 0)
			.at("elements")
			.empty());

	// The strip's meter, a bar per side with its peak since the last frame and its maximum as the bar's mark, shows
	// the frame that the master meter shows: one reader takes the peaks for both.
	app.call("transport.play");
	app.call("audio.render", {{"frames", 48000}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json bars = elements(app, ".meter-master meter");
			return bars.size() == 2 && bars[0].at("attributes").value("value", std::string()) == "-6.0";
		}));
	Json bars = mixer(".mixer-meter meter");
	REQUIRE(bars.size() == 2);
	CHECK(bars[0].at("attributes").at("value") == "-6.0");
	CHECK(bars[1].at("attributes").at("value") == "-6.0");
	CHECK(bars[1].at("attributes").at("peak") == "-6.0");
	CHECK(mixer(".mixer-peak").at(0).value("text", std::string()) == "-6.0");

	// A click on the peak level resets the channels' maxima (p. 468), not the master meter's.
	app.ui().click(std::nullopt, {strip + ".mixer-peak", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&mixer] { return mixer(".mixer-peak").at(0).value("text", std::string()) == "-inf"; }));
	CHECK(text(app, ".peak-max") == "-6.0 dB");
}

TEST_CASE("a channel meter's Global Meter Settings menu holds its peaks for a time, forever or not at all, and resets "
		  "the meters",
	"[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.call("settings.set", {{"key", "meter.peakHoldTime"}, {"value", 30000}});
	app.appUi().docking().open("mixer");
	const std::string strip = ".mixer-channel[channel=" + std::to_string(channel) + "] ";
	const auto bar = [&app, &strip]
	{
		const Json bars = app.ui().dom(std::nullopt, strip + ".mixer-meter meter", 0).at("elements");
		return bars.empty() ? Json::object() : bars[0].at("attributes");
	};
	const auto menu = [&app, &strip](const std::string& item)
	{
		app.ui().click(std::nullopt, {strip + ".mixer-meter", 0, std::nullopt}, ui::MouseButton::Right, 1, {});
		app.ui().click(std::nullopt, {"#mixer-meter-menu menuitem[meter=" + item + "]", 0, std::nullopt},
			ui::MouseButton::Left, 1, {});
	};
	const auto setting = [&app](const std::string& key)
	{ return app.call("settings.get", {{"key", key}}).at("value"); };

	// The mark holds the peak for the peak hold time (p. 1614) while the level falls, then falls with it.
	app.call("transport.play");
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bar] { return bar().value("peak", std::string()) == "-6.0"; }));
	app.call("channel.set", {{"channel", channel}, {"volume", -20.0}});
	app.call("audio.render", {{"frames", 4800}}); // the fader glides there: frames after these show only -26 dB
	app.waitUntil([] { return false; }, std::chrono::milliseconds(100));
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bar] { return bar().value("value", std::string()) == "-26.0"; }));
	CHECK(bar().value("peak", std::string()) == "-6.0");
	app.call("settings.set", {{"key", "meter.peakHoldTime"}, {"value", 500}});
	REQUIRE(app.waitUntil([&bar] { return bar().value("peak", std::string()) == "-200"; }));

	// Hold Forever: the mark is the maximum until a reset (p. 467), past the peak hold time.
	menu("holdForever");
	CHECK(setting("meter.holdForever") == true);
	REQUIRE(app.waitUntil([&bar] { return bar().value("peak", std::string()) == "-6.0"; }));
	app.waitUntil([] { return false; }, std::chrono::milliseconds(700));
	CHECK(bar().value("peak", std::string()) == "-6.0");

	// Reset Meters resets the channels' maxima, as a click on the peak level does.
	menu("reset");
	REQUIRE(app.waitUntil([&bar] { return bar().value("peak", std::string()) == "-200"; }));
	CHECK(app.ui().dom(std::nullopt, strip + ".mixer-peak", 0).at("elements").at(0).value("text", std::string()) ==
		"-inf");

	// Hold Peaks off: no mark at all.
	menu("holdPeaks");
	CHECK(setting("meter.holdPeaks") == false);
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bar] { return bar().value("value", std::string()) == "-26.0"; }));
	CHECK_FALSE(bar().contains("peak"));
	// The menu's items show the settings.
	const Json items = app.ui().dom(std::nullopt, "#mixer-meter-menu menuitem[checked]", 0).at("elements");
	REQUIRE(items.size() == 1);
	CHECK(items[0].at("attributes").at("meter") == "holdForever");
}

TEST_CASE("the meter reader reads only while a view shows meters", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	ui::MeterReader reader(engine, binding);
	CHECK_FALSE(binding.update()); // no view shows meters: the shell needn't step every frame

	std::shared_ptr<std::vector<uint64_t>> shows = reader.show();
	CHECK(binding.update());
	CHECK(binding.update());

	shows.reset();
	CHECK_FALSE(binding.update()); // the reader ends its watch once no view shows meters
	CHECK_FALSE(binding.update());
}
