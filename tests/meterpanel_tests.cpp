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
#include "ui/renderer.h"
#include "ui/shell.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

	// The texts of a page's scale marks (reference manual p. 534), from its top.
	std::vector<std::string> marks(test::UiApp& app, const std::string& page)
	{
		std::vector<std::string> texts;
		for (const Json& mark : elements(app, page + " .meter-scale span"))
			texts.push_back(mark.value("text", std::string()));
		return texts;
	}

	// A setting's control in the meter display, which a test sets as the user would.
	Rml::ElementFormControl* control(test::UiApp& app, const std::string& key)
	{
		for (int i = 0; i < app.main().context().GetNumDocuments(); ++i)
			if (auto* found = rmlui_dynamic_cast<Rml::ElementFormControl*>(
					app.main().context().GetDocument(i)->QuerySelector("[setting=\"" + key + "\"]")))
				return found;
		return nullptr;
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
	// A MIDI channel's meter has no bars until a note played (meter.get's MIDI channels).
	const std::string midiStrip = ".mixer-channel[channel=" + std::to_string(midi) + "] ";
	CHECK(app.ui().dom(std::nullopt, midiStrip + ".mixer-meter", 0).at("elements").size() == 1);
	CHECK(app.ui().dom(std::nullopt, midiStrip + ".mixer-meter meter", 0).at("elements").empty());

	// The strip's meter, a bar per side with its peak since the last frame and its peak mark, shows the frame
	// that the master meter shows: one reader takes the peaks for both.
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

	// A VCA fader's meter shows its channels summed (p. 516), here the one channel.
	const auto vca = app.call("track.add", {{"type", "vca"}}).at("channel").at("id").get<uint64_t>();
	app.call("linkGroup.add", {{"channels", {channel}}, {"vca", vca}});
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil(
		[&app, vca]
		{
			const Json bars =
				app.ui()
					.dom(std::nullopt, ".mixer-channel[channel=" + std::to_string(vca) + "] .mixer-meter meter", 0)
					.at("elements");
			return bars.size() == 2 && bars[0].at("attributes").value("value", std::string()) == "-6.0";
		}));

	// The MIDI channel's meter shows the velocity of the note that sounds (p. 468).
	app.call("channel.set", {{"channel", midi}, {"monitor", true}});
	app.call("midi.inject", {{"channel", midi}, {"messages", {{{"position", 0}, {"bytes", {0x90, 60, 127}}}}}});
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil(
		[&app, &midiStrip]
		{
			const Json bars = app.ui().dom(std::nullopt, midiStrip + ".mixer-meter meter", 0).at("elements");
			return bars.size() == 1 && bars[0].at("attributes").value("value", std::string()) == "0.0";
		}));
}

TEST_CASE("a channel meter's bars are on the +3 dB digital scale, with its marks beside them", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.appUi().docking().open("mixer");
	const std::string strip = ".mixer-channel[channel=" + std::to_string(channel) + "] ";
	const auto mixer = [&app, &strip](const std::string& selector)
	{ return app.ui().dom(std::nullopt, strip + selector, 0).at("elements"); };
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&mixer] { return mixer(".mixer-meter meter").size() == 2; }));

	// The channel meters use the +3 dB digital scale (reference manual pp. 467-468, 1614): -60 to +3 dB.
	const Json bar = mixer(".mixer-meter meter").at(0);
	CHECK(bar.at("attributes").at("min") == "-60");
	CHECK(bar.at("attributes").at("max") == "3");
	const Json marks = mixer(".mixer-meter-scale span");
	std::vector<std::string> texts;
	for (const Json& mark : marks)
		texts.push_back(mark.value("text", std::string()));
	CHECK(texts == std::vector<std::string>{"0", "-6", "-12", "-18", "-24", "-30", "-40", "-50"});
	// Each mark's middle is at its level's height on the bar: 0 dB 3 dB under its top, -30 dB 33 dB under it.
	const Json& box = bar.at("box");
	const auto height = [&box](double db)
	{ return box.at("y").get<double>() + (box.at("height").get<double>() * (3.0 - db) / 63.0); };
	const auto middle = [](const Json& mark)
	{ return mark.at("box").at("y").get<double>() + (mark.at("box").at("height").get<double>() / 2.0); };
	CHECK(std::abs(middle(marks.at(0)) - height(0.0)) < 1.5);
	CHECK(std::abs(middle(marks.at(5)) - height(-30.0)) < 1.5);
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
	// The bar falls back to the lower level (meter.fallback) while the audio plays on.
	app.call("channel.set", {{"channel", channel}, {"volume", -20.0}});
	REQUIRE(app.waitUntil(
		[&app, &bar]
		{
			app.call("audio.render", {{"frames", 480}});
			return bar().value("value", std::string()) == "-26.0";
		}));
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
	const Json items = app.ui().dom(std::nullopt, "#mixer-meter-menu menuitem[meter][checked]", 0).at("elements");
	REQUIRE(items.size() == 1);
	CHECK(items[0].at("attributes").at("meter") == "holdForever");

	// The menu follows the settings that MCP, Lua or another mixer's menu change.
	app.call("settings.set", {{"key", "meter.holdForever"}, {"value", false}});
	app.call("settings.set", {{"key", "meter.holdPeaks"}, {"value", true}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json checked =
				app.ui().dom(std::nullopt, "#mixer-meter-menu menuitem[meter][checked]", 0).at("elements");
			return checked.size() == 1 && checked[0].at("attributes").at("meter") == "holdPeaks";
		}));
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bar] { return bar().value("peak", std::string()) == "-26.0"; }));
}

TEST_CASE("a channel meter's Global Meter Settings menu sets where the channel meters measure", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel =
		app.call("track.add", {{"type", "audio"}, {"channels", 1}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}, {"volume", -10.0}});
	app.appUi().docking().open("mixer");
	const std::string strip = ".mixer-channel[channel=" + std::to_string(channel) + "] ";
	const auto bars = [&app, &strip]
	{ return app.ui().dom(std::nullopt, strip + ".mixer-meter meter", 0).at("elements"); };
	const auto checked = [&app]
	{
		const Json items =
			app.ui().dom(std::nullopt, "#mixer-meter-menu menuitem[meter-position][checked]", 0).at("elements");
		return items.size() == 1 ? items[0].at("attributes").at("meter-position").get<std::string>() : "";
	};
	REQUIRE(app.waitUntil([&checked] { return checked() == "postPanner"; }));
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bars] { return bars().size() == 2; })); // both sides of the mono channel's panner

	// Input (p. 467): the input level, one bar for the mono channel, before the fader: the stereo sine's -6 dBFS
	// sides at -3 dB each.
	app.ui().click(std::nullopt, {strip + ".mixer-meter", 0, std::nullopt}, ui::MouseButton::Right, 1, {});
	app.ui().click(std::nullopt, {"#mixer-meter-menu menuitem[meter-position=input]", 0, std::nullopt},
		ui::MouseButton::Left, 1, {});
	CHECK(app.call("settings.get", {{"key", "meter.position"}}).at("value") == "input");
	REQUIRE(app.waitUntil([&checked] { return checked() == "input"; }));
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil(
		[&bars]
		{
			const Json shown = bars();
			return shown.size() == 1 && shown[0].at("attributes").value("value", std::string()) == "-3.0";
		}));

	// The menu follows the setting that MCP, Lua or another mixer's menu change.
	app.call("settings.set", {{"key", "meter.position"}, {"value", "postFader"}});
	REQUIRE(app.waitUntil([&checked] { return checked() == "postFader"; }));
	REQUIRE(app.waitUntil( // the bar falls back to the lower level while the audio plays on (meter.fallback)
		[&app, &bars]
		{
			app.call("audio.render", {{"frames", 480}});
			const Json shown = bars();
			return shown.size() == 1 && shown[0].at("attributes").value("value", std::string()) == "-13.0";
		}));
}

TEST_CASE("a channel meter's bars fall back from a peak at meter.fallback", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.call("settings.set", {{"key", "meter.fallback"}, {"value", 10.0}}); // dB per second
	app.appUi().docking().open("mixer");
	const std::string strip = ".mixer-channel[channel=" + std::to_string(channel) + "] ";
	const auto bar = [&app, &strip]
	{
		const Json bars = app.ui().dom(std::nullopt, strip + ".mixer-meter meter", 0).at("elements");
		const std::optional<float> value =
			bars.empty() ? std::nullopt : ui::widgets::parse(bars[0].at("attributes").value("value", std::string()));
		return value.value_or(0.0F);
	};
	// The sine plays on until the bar shows its peak, so every read after a render finds the peak again.
	REQUIRE(app.waitUntil(
		[&app, &bar]
		{
			app.call("audio.render", {{"frames", 480}});
			return bar() == -6.0F;
		}));

	// Silence after the peak (p. 1614): the bar falls 10 dB a second, not at once, to silence below its scale.
	const auto start = std::chrono::steady_clock::now();
	app.call("channel.set", {{"channel", channel}, {"mute", true}});
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bar] { return bar() < -7.0F; }));
	CHECK(std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50));
	REQUIRE(app.waitUntil([&bar] { return bar() < -9.0F; }));
	CHECK(std::chrono::steady_clock::now() - start > std::chrono::milliseconds(250));
	app.call("settings.set", {{"key", "meter.fallback"}, {"value", 100.0}});
	REQUIRE(app.waitUntil([&bar] { return bar() == -200.0F; }));
}

TEST_CASE("the meter settings pane sets Reset on Start and the master meter's RMS settings", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.appUi().docking().open("meter");
	const auto setting = [&app](const std::string& key)
	{ return app.call("settings.get", {{"key", key}}).at("value"); };
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-master .meter-configure"); }));
	CHECK_FALSE(shown(app, ".meter-settings"));

	// Configure Meter Settings opens the pane (p. 538), and closes it again.
	click(app, ".meter-master .meter-configure");
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-settings"); }));
	click(app, "[setting=\"meter.resetOnStart\"]");
	click(app, "[setting=\"meter.rmsAes17\"]");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.rmsAes17") == true; }));
	CHECK(setting("meter.resetOnStart") == true);
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json field = elements(app, "[setting=\"meter.rmsResolution\"]");
			return !field.empty() && field[0].at("attributes").value("value", std::string()) == "300";
		}));
	app.ui().wheel(std::nullopt, {".meter-display [setting=\"meter.rmsResolution\"]", 0, std::nullopt}, 0.0F, -10.0F);
	REQUIRE(app.waitUntil([&setting] { return setting("meter.rmsResolution") > 300; }));
	click(app, ".meter-master .meter-configure");
	REQUIRE(app.waitUntil([&app] { return !shown(app, ".meter-settings"); }));

	// The loudness page's Configure Loudness Settings opens the same pane, which follows the settings that MCP, Lua or
	// the preferences change.
	click(app, ".meter-tab[value=loudness]");
	click(app, ".meter-loudness .meter-configure");
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-settings"); }));
	app.call("settings.set", {{"key", "meter.rmsAes17"}, {"value", false}});
	app.call("settings.set", {{"key", "meter.rmsResolution"}, {"value", 100}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json field = elements(app, "[setting=\"meter.rmsResolution\"]");
			return !field.empty() && field[0].at("attributes").value("value", std::string()) == "100";
		}));
	CHECK_FALSE(elements(app, "[setting=\"meter.rmsAes17\"]").at(0).at("attributes").contains("checked"));
	CHECK(elements(app, "[setting=\"meter.resetOnStart\"]").at(0).at("attributes").contains("checked"));
}

TEST_CASE("the loudness meter's unit, scale, Hold on Stop and Measure Loudness follow their settings", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.appUi().docking().open("meter");
	const auto setting = [&app](const std::string& key)
	{ return app.call("settings.get", {{"key", key}}).at("value"); };
	const auto bar = [&app](const char* attribute)
	{ return elements(app, ".meter-loudness meter.momentary").at(0).at("attributes").value(attribute, std::string()); };
	app.call("transport.play");
	app.call("audio.render", {{"frames", 48000}});
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-tab[value=loudness]"); }));
	click(app, ".meter-tab[value=loudness]");
	click(app, ".meter-loudness .meter-configure");
	REQUIRE(app.waitUntil([&app]
		{ return text(app, ".integrated") == "-6.0 LUFS" && !elements(app, ".meter-measure.checked").empty(); }));
	CHECK(bar("max") == "-14"); // EBU +9 (reference manual p. 536)

	// Unit in the settings pane (p. 539): LU, relative to -23 LUFS.
	Rml::ElementFormControl* unit = nullptr;
	for (int i = 0; unit == nullptr && i < app.main().context().GetNumDocuments(); ++i)
		unit = rmlui_dynamic_cast<Rml::ElementFormControl*>(
			app.main().context().GetDocument(i)->QuerySelector("[setting=\"meter.loudnessUnit\"]"));
	REQUIRE(unit != nullptr);
	unit->SetValue("lu");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.loudnessUnit") == "lu"; }));
	REQUIRE(app.waitUntil([&app] { return text(app, ".integrated") == "17.0 LU"; }));
	CHECK(text(app, ".momentary-max") == "17.0 LU");
	CHECK(text(app, ".true-peak-max") == "-6.0 dBTP");

	// EBU Scale follows the setting: EBU +18 is -59 to -5 LUFS (p. 536).
	app.call("settings.set", {{"key", "meter.loudnessScale"}, {"value", "ebu18"}});
	REQUIRE(app.waitUntil([&bar] { return bar("max") == "-5"; }));
	CHECK(bar("min") == "-59");
	CHECK(unit->GetValue() == "lu");

	// Hold on Stop (p. 540) and Measure Loudness (p. 537) switch their settings.
	click(app, "[setting=\"meter.holdOnStop\"]");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.holdOnStop") == false; }));
	click(app, ".meter-measure");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.measureLoudness") == false; }));
	REQUIRE(app.waitUntil([&app] { return elements(app, ".meter-measure.checked").empty(); }));
	app.call("settings.set", {{"key", "meter.measureLoudness"}, {"value", true}});
	REQUIRE(app.waitUntil([&app] { return !elements(app, ".meter-measure.checked").empty(); }));
}

TEST_CASE("the loudness readouts turn red above their thresholds", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.appUi().docking().open("meter");
	app.call("transport.play");
	app.call("audio.render", {{"frames", 48000}});
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-tab[value=loudness]"); }));
	click(app, ".meter-tab[value=loudness]");
	// -6 LUFS: above the default thresholds, but -6 dBTP isn't (reference manual pp. 537 and 539).
	const auto over = [&app]
	{
		std::vector<std::string> readouts;
		for (const char* readout : {"momentary-max", "short-term-max", "integrated", "true-peak-max"})
			if (!elements(app, std::string(".") + readout + ".over").empty())
				readouts.emplace_back(readout);
		return readouts;
	};
	REQUIRE(app.waitUntil([&] { return text(app, ".integrated") == "-6.0 LUFS" && over().size() == 2; }));
	CHECK(over() == std::vector<std::string>{"momentary-max", "integrated"}); // no short-term loudness yet

	// A reference and a tolerance above the value turn its readout back; the pane sets them.
	app.call("settings.set", {{"key", "meter.integratedReference"}, {"value", -10.0}});
	app.call("settings.set", {{"key", "meter.integratedTolerance"}, {"value", 4.0}});
	REQUIRE(app.waitUntil([&] { return over() == std::vector<std::string>{"momentary-max"}; }));
	app.call("settings.set", {{"key", "meter.truePeakReference"}, {"value", -10.0}});
	REQUIRE(app.waitUntil([&] { return over() == std::vector<std::string>{"momentary-max", "true-peak-max"}; }));
	click(app, ".meter-loudness .meter-configure");
	REQUIRE(app.waitUntil([&app] { return shown(app, "[setting=\"meter.truePeakTolerance\"]"); }));
	app.ui().wheel(
		std::nullopt, {".meter-display [setting=\"meter.truePeakTolerance\"]", 0, std::nullopt}, 0.0F, -10.0F);
	REQUIRE(app.waitUntil([&app]
		{ return app.call("settings.get", {{"key", "meter.truePeakTolerance"}}).at("value").get<double>() > 0.0; }));
	app.call("settings.set", {{"key", "meter.truePeakTolerance"}, {"value", 6.0}});
	REQUIRE(app.waitUntil([&] { return over() == std::vector<std::string>{"momentary-max"}; }));

	// Past 3 s there is a short-term loudness: -6 LUFS, above the default -18 LUFS; then a threshold above it.
	app.call("audio.render", {{"frames", 48000 * 3}});
	REQUIRE(app.waitUntil([&] { return text(app, ".short-term-max") == "-6.0 LUFS"; }));
	REQUIRE(app.waitUntil([&] { return over() == std::vector<std::string>{"momentary-max", "short-term-max"}; }));
	app.call("settings.set", {{"key", "meter.shortTermReference"}, {"value", -10.0}});
	app.call("settings.set", {{"key", "meter.shortTermTolerance"}, {"value", 4.0}});
	REQUIRE(app.waitUntil([&] { return over() == std::vector<std::string>{"momentary-max"}; }));
}

TEST_CASE("the meter settings pane shows its settings and which button opened it", "[ui]")
{
	test::UiApp app({1000, 680}); // CI's macOS runner opens windows at most 1024 by 684
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.appUi().docking().open("meter");
	app.call("settings.set", {{"key", "meter.resetOnStart"}, {"value", true}});
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-master .meter-configure"); }));
	click(app, ".meter-master .meter-configure");
	REQUIRE(app.waitUntil(
		[&app]
		{
			return shown(app, ".meter-settings") &&
				elements(app, "[setting=\"meter.resetOnStart\"]").at(0).at("attributes").contains("checked");
		}));
	app.shell().step(std::chrono::milliseconds(0));

	// The display only, so that the rest of the window doesn't touch this image: Configure Meter Settings checked,
	// its label in the button, and Reset on Start's check box on, AES 17's off.
	const ui::Image display = app.ui().screenshot(std::nullopt, {".meter-display", 0, std::nullopt});
	test::UiApp::checkGolden("meter-settings", display);
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

TEST_CASE("the PPM scales put the meters' marks and headroom at their levels, broadcast scales at the alignment level",
	"[unit]")
{
	// Digital: 0 dBFS at the top, its headroom (p. 534); the channel meters' +3 dB digital scale (pp. 467-468).
	const ui::MeterScale digital = ui::meterScale("digital");
	CHECK(digital.min == -60.0F);
	CHECK(digital.max == 0.0F);
	CHECK_FALSE(digital.aligned);
	CHECK(digital.marks.front() == std::pair<float, std::string>{0.0F, "0"});
	const ui::MeterScale channels = ui::meterScale("digital3");
	CHECK(channels.max == 3.0F);
	CHECK(channels.headroom == 0.0F);
	// DIN reads the alignment level at -9 dB, so its 0 dB, the permitted maximum level, is 9 dB above it.
	const ui::MeterScale din = ui::meterScale("din", -18.0F);
	CHECK(din.aligned);
	CHECK(din.min == -59.0F);
	CHECK(din.max == -4.0F);
	CHECK(din.headroom == -9.0F);
	// EBU's TEST mark is the alignment level, its headroom 9 dB above; British 4 is the alignment level, 4 dB a mark.
	CHECK(ui::meterScale("ebu", -20.0F).headroom == -11.0F);
	const ui::MeterScale british = ui::meterScale("british", -18.0F);
	CHECK(british.marks.front() == std::pair<float, std::string>{-6.0F, "7"});
	CHECK(british.marks.back() == std::pair<float, std::string>{-30.0F, "1"});
	CHECK(british.headroom == -10.0F);
	// K-14: 0 at -14 dBFS whatever the alignment level, its headroom 4 dB above; unknown names are Digital.
	const ui::MeterScale k14 = ui::meterScale("k14", -9.0F);
	CHECK_FALSE(k14.aligned);
	CHECK(k14.max == 0.0F);
	CHECK(k14.headroom == -10.0F);
	CHECK(k14.marks.front().second == "+14");
	CHECK(ui::meterScale("vu").max == 0.0F);
}

TEST_CASE("the master meter's scale follows meter.scale and meter.alignmentLevel", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.appUi().docking().open("meter");
	const auto setting = [&app](const std::string& key)
	{ return app.call("settings.get", {{"key", key}}).at("value"); };
	const auto bars = [&app]
	{
		const Json bars = elements(app, ".meter-master .meter-bars meter");
		return bars.empty() ? Json() : bars[0].at("attributes");
	};
	const auto disabled = [&app]
	{ return elements(app, "[setting=\"meter.alignmentLevel\"]").at(0).at("attributes").contains("disabled"); };
	app.call("audio.render", {{"frames", 4800}});

	// Digital by default (p. 534), with no alignment level, and its marks beside the bars.
	REQUIRE(app.waitUntil([&] { return !bars().is_null() && marks(app, ".meter-master").size() == 9; }));
	CHECK(bars().at("min") == "-60");
	CHECK(bars().at("max") == "0");
	CHECK(marks(app, ".meter-master").front() == "0");
	CHECK(marks(app, ".meter-master").back() == "-60");
	CHECK(disabled());
	CHECK(setting("meter.alignmentLevel") == -18.0); // the lists send nothing until they show their settings

	// EBU at the default alignment level of -18 dBFS: +12 is -6 dBFS, and the headroom's line at +9.
	Rml::ElementFormControl* scale = control(app, "meter.scale");
	REQUIRE(scale != nullptr);
	scale->SetValue("ebu");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.scale") == "ebu"; }));
	REQUIRE(app.waitUntil([&bars] { return bars().at("max") == "-6"; }));
	CHECK(bars().at("min") == "-30");
	CHECK(marks(app, ".meter-master").front() == "+12");
	CHECK(elements(app, ".meter-master .meter-headroom").size() == 1);
	CHECK_FALSE(disabled());

	// Another alignment level moves the scale; the list sets it as a number.
	Rml::ElementFormControl* alignment = control(app, "meter.alignmentLevel");
	REQUIRE(alignment != nullptr);
	alignment->SetValue("-20");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.alignmentLevel") == -20.0; }));
	REQUIRE(app.waitUntil([&bars] { return bars().at("max") == "-8"; }));

	// K-14 has no alignment level: 0 dBFS at its top, whatever the setting.
	app.call("settings.set", {{"key", "meter.scale"}, {"value", "k14"}});
	REQUIRE(app.waitUntil([&bars] { return bars().at("max") == "0"; }));
	CHECK(marks(app, ".meter-master").front() == "+14");
	CHECK(scale->GetValue() == "k14");
	CHECK(disabled());
}

TEST_CASE("the loudness page switches between LU and LUFS, and its scale follows the unit and EBU Scale", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.appUi().docking().open("meter");
	const auto setting = [&app](const std::string& key)
	{ return app.call("settings.get", {{"key", key}}).at("value"); };
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-tab[value=loudness]"); }));
	click(app, ".meter-tab[value=loudness]");

	// EBU +9 in LUFS (p. 536): -14 to -41 LUFS, every 3 LU.
	REQUIRE(app.waitUntil([&app] { return shown(app, ".meter-unit") && text(app, ".meter-unit") == "LUFS"; }));
	const std::vector<std::string> lufs = marks(app, ".meter-loudness");
	REQUIRE(lufs.size() == 10);
	CHECK(lufs.front() == "-14");
	CHECK(lufs.back() == "-41");

	// The switch on the page itself (p. 537) sets the unit: the scale in LU, relative to -23 LUFS.
	click(app, ".meter-unit");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.loudnessUnit") == "lu"; }));
	REQUIRE(app.waitUntil([&app] { return text(app, ".meter-unit") == "LU"; }));
	CHECK(marks(app, ".meter-loudness").front() == "+9");
	CHECK(marks(app, ".meter-loudness").back() == "-18");

	// EBU +18: +18 to -36 LU, every 6 LU; back in LUFS, -5 to -59 LUFS.
	app.call("settings.set", {{"key", "meter.loudnessScale"}, {"value", "ebu18"}});
	REQUIRE(app.waitUntil([&app] { return marks(app, ".meter-loudness").front() == "+18"; }));
	CHECK(marks(app, ".meter-loudness").back() == "-36");
	click(app, ".meter-unit");
	REQUIRE(app.waitUntil([&setting] { return setting("meter.loudnessUnit") == "lufs"; }));
	REQUIRE(app.waitUntil([&app] { return marks(app, ".meter-loudness").front() == "-5"; }));
	CHECK(marks(app, ".meter-loudness").back() == "-59");
}

TEST_CASE("the master meter's bars fall back from a peak at meter.fallback", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = app.call("track.add", {{"type", "audio"}}).at("channel").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", channel}, {"monitor", true}});
	app.call("settings.set", {{"key", "meter.fallback"}, {"value", 1.0}}); // dB per second
	app.appUi().docking().open("meter");
	const auto bar = [&app]
	{
		const Json bars = elements(app, ".meter-master .meter-bars meter");
		const std::optional<float> value =
			bars.empty() ? std::nullopt : ui::widgets::parse(bars[0].at("attributes").value("value", std::string()));
		return value.value_or(0.0F);
	};
	REQUIRE(app.waitUntil(
		[&app, &bar]
		{
			app.call("audio.render", {{"frames", 480}});
			return bar() == -6.0F;
		}));

	// Silence after the peak (p. 1614): the bar falls 1 dB a second, through the levels between, not at once.
	app.call("channel.set", {{"channel", channel}, {"mute", true}});
	app.call("audio.render", {{"frames", 4800}});
	REQUIRE(app.waitUntil([&bar] { return bar() < -6.0F; }));
	CHECK(bar() > -60.0F);
	app.call("settings.set", {{"key", "meter.fallback"}, {"value", 100.0}});
	REQUIRE(app.waitUntil([&bar] { return bar() == -200.0F; }));
}
