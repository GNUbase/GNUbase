// SPDX-License-Identifier: GPL-3.0-or-later
// The audio functions' dialogs (#147, docs/features/audio-functions.md) in the app's UI, driven with ui::Automation as
// MCP's UI tools drive them: Detect Silence, the Statistics window and the Spectrum Analyzer for the selected audio
// events. The commands themselves are in tests/audiofunctions_tests.cpp.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool open(test::UiApp& app, const std::string& selector)
	{
		return !elements(app, selector).empty();
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	std::string text(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("text", std::string());
	}

	void step(test::UiApp& app)
	{
		app.shell().step(std::chrono::milliseconds(0));
	}

	void click(test::UiApp& app, const std::string& selector, ui::MouseButton button = ui::MouseButton::Left)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, button, 1, {});
		step(app);
	}

	// Types a number into a value field: a double click edits it, Enter takes it.
	void type(test::UiApp& app, const std::string& selector, const std::string& value)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, value);
		app.ui().key(std::nullopt, "Enter");
		step(app);
	}

	// Opens a drop-down list, moves the selection one option with an arrow key, and closes it.
	void choose(test::UiApp& app, const std::string& select, const std::string& key)
	{
		click(app, select);
		app.ui().key(std::nullopt, key);
		click(app, select);
		step(app);
	}

	void run(test::UiApp& app, const std::string& command, const std::string& dialog)
	{
		app.keys().run(command, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, &dialog] { return open(app, dialog); }));
		step(app);
	}

	void closes(test::UiApp& app, const std::string& dialog)
	{
		CHECK(app.waitUntil([&app, &dialog] { return !open(app, dialog); }));
	}

	// Two events of a three second 48 kHz mono clip, at 0 and at 4 seconds on a linear audio track: a 1 kHz tone at
	// -6 dBFS from 0.5 to 1.5 seconds and one at -12 dBFS from 2 to 2.5 seconds.
	struct Tones
	{
		explicit Tones(test::UiApp& app)
		{
			test::Audio audio;
			audio.sampleRate = 48000;
			audio.samples.resize(size_t{3} * 48000);
			for (size_t i = 0; i < audio.samples.size(); ++i)
			{
				const double t = static_cast<double>(i) / 48000.0;
				double amplitude = 0.0;
				if (t >= 0.5 && t < 1.5)
					amplitude = 0.5;
				else if (t >= 2.0 && t < 2.5)
					amplitude = 0.25;
				audio.samples[i] = static_cast<float>(amplitude * std::sin(2.0 * g_pi * 1000.0 * t));
			}
			const std::string file = (dir.path() / "tones.wav").u8string();
			test::writeWav(file, audio);
			track =
				app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<uint64_t>();
			const Json media =
				app.call("media.add", {{"path", file}, {"sampleRate", 48000}, {"channels", 1}, {"length", 3 * 48000}})
					.at("media");
			const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id");
			for (const int64_t position : {int64_t{0}, int64_t{4} * 48000})
				events.push_back(
					app.call("audioEvent.add",
						   {{"track", track}, {"clip", clip}, {"position", position}, {"length", 3 * 48000}})
						.at("event")
						.at("id")
						.get<uint64_t>());
		}

		test::TempDir dir;
		uint64_t track = 0;
		std::vector<uint64_t> events;
	};

	ui::EventDisplay& showTones(test::UiApp& app, const Tones& tones, size_t selected)
	{
		ui::EventDisplay& display = app.appUi().showProject();
		REQUIRE(app.waitUntil(
			[&display] { return display.project().is_object() && display.project().at("audioEvents").size() == 2; }));
		display.select(std::vector<uint64_t>(tones.events.begin(), tones.events.begin() + static_cast<long>(selected)));
		return display;
	}

	size_t audioEvents(test::UiApp& app)
	{
		return app.call("project.get").at("audioEvents").size();
	}

	// One event of a whole media file on a new linear audio track.
	uint64_t addEvent(test::UiApp& app, const std::filesystem::path& file, int channels, int64_t length)
	{
		const uint64_t track =
			app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<uint64_t>();
		const Json media =
			app.call("media.add",
				   {{"path", file.u8string()}, {"sampleRate", 48000}, {"channels", channels}, {"length", length}})
				.at("media");
		const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id");
		return app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", length}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	}

	// Shows the project's only event in the event display, selected.
	void showEvent(test::UiApp& app, uint64_t event)
	{
		ui::EventDisplay& display = app.appUi().showProject();
		REQUIRE(app.waitUntil(
			[&display] { return display.project().is_object() && display.project().at("audioEvents").size() == 1; }));
		display.select({event});
	}

	// The Spectrum Analyzer's bar of a frequency: one per 1/24 octave from 20 Hz.
	int bar(double frequency)
	{
		return static_cast<int>(std::lround(24.0 * std::log2(frequency / 20.0))) + 1;
	}

	double barHeight(test::UiApp& app, int index)
	{
		return element(app, ".sa-bar[index=\"" + std::to_string(index) + "\"]").at("box").at("height").get<double>();
	}

	// The middle of a bar of #sa-graph, for the mouse.
	ui::Target barTarget(test::UiApp& app, int index)
	{
		const Json graph = element(app, "#sa-graph").at("box");
		const double width = graph.at("width").get<double>() / static_cast<double>(elements(app, ".sa-bar").size());
		return ui::Target{"", 0,
			ui::Point{static_cast<int>(std::lround(graph.at("x").get<double>() + ((index - 0.5) * width))),
				static_cast<int>(std::lround(graph.at("y").get<double>() + (graph.at("height").get<double>() / 2.0)))}};
	}
}

TEST_CASE(
	"Detect Silence shows the sections of the selected events and strips their silence, one after the other", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 2);
	run(app, "audio.detectSilence", "#detect-silence-dialog");
	CHECK_THAT(text(app, "#ds-event"), ContainsSubstring("(1 of 2)"));
	CHECK(text(app, "#ds-sections") == "-");

	// Analyze: two sections along the event, the first from a sixth of it (0.5 of 3 seconds) to half of it.
	click(app, "#ds-analyze");
	CHECK(text(app, "#ds-sections") == "2");
	const Json display = element(app, "#ds-display").at("box");
	const Json sections = elements(app, ".ds-section");
	REQUIRE(sections.size() == 2);
	const double left = sections[0].at("box").at("x").get<double>() - display.at("x").get<double>();
	CHECK(std::abs(left - (display.at("width").get<double>() / 6.0)) <= 2.0);
	CHECK(std::abs(sections[0].at("box").at("width").get<double>() - (display.at("width").get<double>() / 3.0)) <= 2.0);

	// Add as Regions and Strip Silence for the first event: its two sounds remain, as regions of the clip too.
	click(app, "#ds-add-regions");
	click(app, "#ds-process");
	REQUIRE(app.waitUntil([&app] { return audioEvents(app) == 3; }));
	CHECK(app.call("project.get").at("clips").at(0).at("regions").size() == 2);
	CHECK(app.call("history.get").at("steps").back().at("action") == "Detect silence");

	// The second event: Auto analyzes as the settings change; Linked makes the close threshold the open one, and an
	// open threshold of -10 dBFS leaves the quieter tone out.
	REQUIRE(open(app, "#detect-silence-dialog"));
	CHECK_THAT(text(app, "#ds-event"), ContainsSubstring("(2 of 2)"));
	click(app, "#ds-add-regions");
	click(app, "#ds-auto");
	CHECK(text(app, "#ds-sections") == "2");
	click(app, "#ds-linked");
	type(app, "#ds-open-threshold", "-10");
	CHECK(element(app, "#ds-close-threshold").at("attributes").at("value") == "-10");
	CHECK(text(app, "#ds-sections") == "1");

	click(app, "#ds-process");
	closes(app, "#detect-silence-dialog");
	CHECK(audioEvents(app) == 3);
	CHECK(app.call("history.get").at("steps").back().at("action") == "Detect silence");
}

TEST_CASE("Process All Selected Events strips the silence of every selected event at once, and Cancel nothing", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 2);
	run(app, "audio.detectSilence", "#detect-silence-dialog");
	click(app, "#ds-cancel");
	closes(app, "#detect-silence-dialog");
	CHECK(audioEvents(app) == 2);

	run(app, "audio.detectSilence", "#detect-silence-dialog");
	click(app, "#ds-process-all");
	click(app, "#ds-apply-fades");
	click(app, "#ds-process");
	closes(app, "#detect-silence-dialog");
	const Json project = app.call("project.get");
	REQUIRE(project.at("audioEvents").size() == 4);
	CHECK(project.at("audioEvents").at(0).at("fadeIn").at("length") == 480); // 10 ms
	CHECK(app.call("history.get").at("steps").back().at("action") == "Detect silence");
}

TEST_CASE("the Statistics window shows the statistics of the selected events per channel", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	run(app, "audio.statistics", "#statistics-dialog");
	const std::string shown = text(app, "#st-tables");
	for (const char* expected : {"tones", "Mono", "Peak Amplitude -6.02 dB", "Hz (B4", "Sample Rate 48000 Hz",
			 "Bit Depth 32 Bit Float", "Integrated Loudness", "LUFS", "Max. True Peak Level"})
	{
		INFO(shown);
		CHECK_THAT(shown, ContainsSubstring(expected));
	}
	click(app, "#st-close");
	closes(app, "#statistics-dialog");
}

TEST_CASE("the Spectrum Analyzer draws the spectrum, shows the values under the mouse and the difference to a "
		  "position right-clicked",
	"[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	run(app, "audio.spectrumAnalyzer", "#spectrum-analyzer-dialog");
	const Json bars = elements(app, ".sa-bar");
	REQUIRE(bars.size() == 246); // 1/24 octave from 20 Hz up to 24 kHz
	CHECK(barHeight(app, bar(1000.0)) > barHeight(app, bar(300.0)) + 50.0);

	app.ui().move(std::nullopt, barTarget(app, bar(1000.0)));
	step(app);
	const std::string value = text(app, "#sa-value");
	CHECK_THAT(value, ContainsSubstring("Mono"));
	CHECK_THAT(value, ContainsSubstring("Hz"));
	CHECK_THAT(value, ContainsSubstring(" B4 ")); // 987 Hz
	CHECK_THAT(value, !ContainsSubstring("D:"));

	// A right click on the tone, then the mouse at 300 Hz: far lower.
	app.ui().click(std::nullopt, barTarget(app, bar(1000.0)), ui::MouseButton::Right, 1, {});
	step(app);
	CHECK(elements(app, ".sa-bar.reference").size() == 1);
	app.ui().move(std::nullopt, barTarget(app, bar(300.0)));
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("D: -"));
	app.ui().click(std::nullopt, barTarget(app, bar(300.0)), ui::MouseButton::Left, 1, {});
	step(app);
	CHECK(elements(app, ".sa-bar.reference").empty());
	CHECK_THAT(text(app, "#sa-value"), !ContainsSubstring("D:"));

	click(app, "#sa-close");
	closes(app, "#spectrum-analyzer-dialog");
}

TEST_CASE("the Spectrum Analyzer shows the error of a clip whose file can't be read", "[ui]")
{
	test::UiApp app({1000, 760});
	const test::TempDir dir;
	test::Audio audio;
	audio.sampleRate = 48000;
	audio.samples.assign(48000, 0.25f);
	const std::filesystem::path file = dir.path() / "gone.wav";
	test::writeWav(file, audio);
	const uint64_t event = addEvent(app, file, 1, 48000);
	REQUIRE(std::filesystem::remove(file));
	showEvent(app, event);

	run(app, "audio.spectrumAnalyzer", "#spectrum-analyzer-dialog");
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("can't read"));
	CHECK(elements(app, ".sa-bar").empty());
	click(app, "#sa-close");
	closes(app, "#spectrum-analyzer-dialog");
}

TEST_CASE("the Spectrum Analyzer draws the channel of a stereo clip that #sa-channel chooses", "[ui]")
{
	test::UiApp app({1000, 760});
	const test::TempDir dir;
	// A second of stereo: 1 kHz at -6 dBFS on the left, 3 kHz at -6 dBFS on the right.
	test::Audio audio;
	audio.sampleRate = 48000;
	audio.channels = 2;
	audio.samples.resize(size_t{2} * 48000);
	for (size_t i = 0; i < 48000; ++i)
	{
		const double t = static_cast<double>(i) / 48000.0;
		audio.samples[2 * i] = static_cast<float>(0.5 * std::sin(2.0 * g_pi * 1000.0 * t));
		audio.samples[(2 * i) + 1] = static_cast<float>(0.5 * std::sin(2.0 * g_pi * 3000.0 * t));
	}
	const std::filesystem::path file = dir.path() / "stereo.wav";
	test::writeWav(file, audio);
	const uint64_t event = addEvent(app, file, 2, 48000);
	showEvent(app, event);
	run(app, "audio.spectrumAnalyzer", "#spectrum-analyzer-dialog");
	REQUIRE(elements(app, ".sa-bar").size() == 246); // 1/24 octave from 20 Hz up to 24 kHz

	// The left channel first: its tone towers over the right channel's.
	CHECK(barHeight(app, bar(1000.0)) > barHeight(app, bar(3000.0)) + 50.0);
	app.ui().move(std::nullopt, barTarget(app, bar(1000.0)));
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("Left"));

	// Choosing the right channel redraws its bars, and the mouse shows its name.
	choose(app, "#sa-channel", "Down");
	CHECK(barHeight(app, bar(3000.0)) > barHeight(app, bar(1000.0)) + 50.0);
	app.ui().move(std::nullopt, barTarget(app, bar(3000.0)));
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("Right"));

	click(app, "#sa-close");
	closes(app, "#spectrum-analyzer-dialog");
}

TEST_CASE("Create Sampler Track in the Audio menu makes a sampler track of the first selected event", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	app.keys().run("audio.createSamplerTrack", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 2; }));
	const Json project = app.call("project.get");
	const Json& track = project.at("tracks").at(1);
	CHECK(track.at("name") == "Sampler 01");
	for (const Json& channel : project.at("channels"))
		if (channel.at("id") == track.at("channel"))
		{
			CHECK(channel.at("instrument") == "sampler");
			CHECK(channel.at("sample") == project.at("clips").at(0).at("id"));
		}
	CHECK(app.call("history.get").at("steps").back().at("action") == "Create sampler track");
}
