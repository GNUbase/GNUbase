// SPDX-License-Identifier: GPL-3.0-or-later
// The audio functions' dialogs (#147, docs/features/audio-functions.md) in the app's UI, driven with ui::Automation as
// MCP's UI tools drive them: Detect Silence, the Statistics window and the Spectrum Analyzer for the selected audio
// events. The commands themselves are in tests/audiofunctions_tests.cpp.
#include "uiapp.h"

#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/sampleeditor.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

	ui::WindowId audioWindow(test::UiApp& app)
	{
		for (const ui::Window* window : app.shell().windows())
			if (window->id() != app.main().id())
				for (int index = 0; index < window->context().GetNumDocuments(); ++index)
					if (window->context().GetDocument(index)->QuerySelector(
							"#detect-silence-dialog, #spectrum-analyzer-dialog, #statistics-dialog") != nullptr)
						return window->id();
		return app.main().id();
	}

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(audioWindow(app), selector, 0).at("elements");
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
		app.ui().click(audioWindow(app), {selector, 0, std::nullopt}, button, 1, {});
		step(app);
	}

	// Types a number into a value field: a double click edits it, Enter takes it.
	void type(test::UiApp& app, const std::string& selector, const std::string& value)
	{
		app.ui().click(audioWindow(app), {selector, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		app.ui().key(audioWindow(app), "Mod+A");
		app.ui().text(audioWindow(app), value);
		app.ui().key(audioWindow(app), "Enter");
		step(app);
	}

	// Opens a drop-down list, moves the selection one option with an arrow key, and closes it.
	void choose(test::UiApp& app, const std::string& select, const std::string& key)
	{
		click(app, select);
		app.ui().key(audioWindow(app), key);
		click(app, select);
		step(app);
	}

	void run(test::UiApp& app, const std::string& command, const std::string& dialog, bool wait = true)
	{
		app.keys().run(command, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, &dialog] { return open(app, dialog); }));
		step(app);
		if (wait && dialog == "#statistics-dialog")
			REQUIRE(app.waitUntil([&] { return !elements(app, ".st-event").empty(); }));
		if (wait && dialog == "#spectrum-analyzer-dialog")
			REQUIRE(app.waitUntil(
				[&]
				{
					return !elements(app, ".sa-bar").empty() ||
						text(app, "#sa-value").find("can't read") != std::string::npos;
				}));
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
		REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 2; }));
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
		REQUIRE(app.waitUntil([&display] { return display.project().at("audioEvents").size() == 1; }));
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

TEST_CASE("pool audio functions analyze unused clips without creating project events", "[ui]")
{
	const std::string function = GENERATE("statistics", "spectrumAnalyzer");
	test::TempDir dir;
	test::UiApp app({720, 640});
	app.appUi().showProject();
	const auto path = dir.path() / "pool.wav";
	test::writeWav(path, {std::vector<float>(48000, 0.25f), 1, 48000});
	const Json media = app.call("media.add", {{"path", path.u8string()}}).at("media").at("id");
	const Json clip = app.call("clip.add", {{"media", media}, {"name", "Unused clip"}}).at("clip").at("id");
	app.keys().run("window.pool", Json::object(), app.main(), nullptr);
	const std::string row = "#pool listitem[value=c" + clip.dump() + "]";
	REQUIRE(app.waitUntil([&] { return !app.ui().dom(std::nullopt, row, 0).at("elements").empty(); }));
	app.ui().click(std::nullopt, {row + " span.used", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.keys().canRun("pool." + function, app.main(), nullptr));
	REQUIRE(app.waitUntil(
		[&]
		{
			return !app.ui()
						.dom(std::nullopt, "#pool-analysis", 0)
						.at("elements")[0]
						.at("attributes")
						.contains("disabled");
		}));
	const Json before = app.call("project.get");
	app.ui().click(std::nullopt, {"#pool-analysis", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	step(app);
	app.ui().click(std::nullopt, {"menuitem[pool-function='pool." + function + "']", 0, std::nullopt},
		ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&] { return audioWindow(app) != app.main().id(); }));
	CHECK(!app.ui().dom(app.main().id(), "#pool-analysis-menu", 0).at("elements")[0].at("visible").get<bool>());
	if (function == "statistics")
		REQUIRE(app.waitUntil(
			[&]
			{
				const Json names = elements(app, ".st-name");
				return !names.empty() && names[0].value("text", "") == "Unused clip";
			}));
	else
		REQUIRE(app.waitUntil([&] { return !elements(app, ".sa-bar").empty(); }));
	CHECK(app.call("project.get") == before);
}

TEST_CASE("Detect Silence draws peaks and previews with threshold handles and Mute Gaps", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	run(app, "audio.detectSilence", "#detect-silence-dialog");
	REQUIRE(app.waitUntil([&] { return !elements(app, ".ds-peak").empty(); }));
	click(app, "#ds-analyze");
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "2"; }));
	const auto handle = element(app, "#ds-open-handle").at("box");
	CHECK(handle.at("height").get<double>() > 0);
	const auto display = element(app, "#ds-display").at("box");
	const ui::Target threshold{"", 0,
		ui::Point{static_cast<int>(handle.at("x").get<double>() + 4),
			static_cast<int>(display.at("y").get<double>() + (display.at("height").get<double>() * 0.25))}};
	app.ui().drag(audioWindow(app), {"#ds-open-handle", 0, std::nullopt}, threshold);
	step(app);
	const double openThreshold =
		std::stod(element(app, "#ds-open-threshold").at("attributes").at("value").get<std::string>());
	INFO(display.dump());
	INFO(handle.dump());
	INFO(element(app, "#ds-wave").dump());
	CHECK(std::abs(openThreshold + 6.02) < 0.5);
	// Put the detection settings back, then preview the two sections.
	type(app, "#ds-open-threshold", "-20");
	click(app, "#ds-analyze");
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "2"; }));
	click(app, "#ds-mute-gaps");
	// A click invokes the same press and release path as holding the waveform.
	click(app, "#ds-wave");
	const auto log = app.call("engine.commandLog").at("commands");
	bool started = false;
	for (const auto& entry : log)
		if (entry.at("command") == "audition.start")
		{
			started = true;
			CHECK(entry.at("parameters").at("sections").size() == 2);
		}
	CHECK(started);
	bool stopped = false;
	for (const auto& entry : log)
		stopped = stopped || entry.at("command") == "audition.stop";
	CHECK(stopped);
	// Closing a window while the button is held also stops its preview.
	app.ui().press(audioWindow(app), {"#ds-wave", 0, std::nullopt}, ui::MouseButton::Left);
	app.ui().close(audioWindow(app), false);
	REQUIRE(app.waitUntil([&] { return !open(app, "#detect-silence-dialog"); }));
	size_t closeStops = 0;
	const Json closeLog = app.call("engine.commandLog").at("commands");
	for (const auto& entry : closeLog)
		if (entry.at("command") == "audition.stop")
			++closeStops;
	CHECK(closeStops == 2);
}

TEST_CASE("Detect Silence zooms and scrolls its waveform without changing analysis or preview timing", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	app.call("audioEvent.set", {{"event", tones.events[0]}, {"offset", 12000}, {"length", 120000}});
	showTones(app, tones, 1);
	run(app, "audio.detectSilence", "#detect-silence-dialog");
	REQUIRE(app.waitUntil([&] { return !elements(app, ".ds-peak").empty(); }));
	click(app, "#ds-analyze");
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "2"; }));
	const Json jobs = app.call("job.list").at("jobs");
	const double width = element(app, "#ds-wave").at("box").at("width").get<double>();
	const double sectionWidth = elements(app, ".ds-section")[0].at("box").at("width").get<double>();
	REQUIRE(open(app, "#ds-zoom"));
	const Json viewBox = element(app, "#ds-view").at("box");
	const Json zoomBox = element(app, "#ds-zoom").at("box");
	CHECK(zoomBox.at("x").get<double>() >= viewBox.at("x").get<double>() + viewBox.at("width").get<double>());
	CHECK(zoomBox.at("y").get<double>() < viewBox.at("y").get<double>() + viewBox.at("height").get<double>());
	app.ui().wheel(audioWindow(app), {"#ds-view", 0, std::nullopt}, 0.0F, -1.0F, {"ctrl"});
	REQUIRE(app.waitUntil([&] { return element(app, "#ds-wave").at("box").at("width").get<double>() > width * 1.5; }));
	CHECK(elements(app, ".ds-section")[0].at("box").at("width").get<double>() > sectionWidth * 1.5);
	CHECK(text(app, "#ds-sections") == "2");
	const ui::Point pointer{static_cast<int>(viewBox.at("x").get<double>() + (viewBox.at("width").get<double>() * 0.3)),
		static_cast<int>(viewBox.at("y").get<double>() + 70)};
	for (const std::string modifier : {"ctrl", "meta"})
		for (const float delta : {-1.0F, 1.0F})
		{
			const Json before = element(app, "#ds-wave").at("box");
			const double fraction = (pointer.x - before.at("x").get<double>()) / before.at("width").get<double>();
			app.ui().wheel(audioWindow(app), {"", 0, pointer}, 0.0F, delta, {modifier});
			REQUIRE(
				app.waitUntil([&] { return element(app, "#ds-wave").at("box").at("width") != before.at("width"); }));
			const Json after = element(app, "#ds-wave").at("box");
			CHECK(std::abs(pointer.x - after.at("x").get<double>() - (fraction * after.at("width").get<double>())) < 2);
		}
	app.ui().wheel(audioWindow(app), {"#ds-view", 0, std::nullopt}, 0.0F, 2.0F);
	REQUIRE(app.waitUntil(
		[&]
		{
			return element(app, "#ds-wave").at("box").at("x").get<double>() <
				element(app, "#ds-view").at("box").at("x").get<double>();
		}));
	const auto view = element(app, "#ds-view").at("box");
	// Using the native horizontal scrollbar must not audition the audio beneath it.
	app.ui().click(audioWindow(app),
		{"", 0,
			ui::Point{static_cast<int>(view.at("x").get<double>() + (view.at("width").get<double>() / 2)),
				static_cast<int>(view.at("y").get<double>() + view.at("height").get<double>() - 2)}},
		ui::MouseButton::Left, 1, {});
	const Json scrollLog = app.call("engine.commandLog").at("commands");
	for (const auto& entry : scrollLog)
		CHECK(entry.at("command") != "audition.start");
	const auto wave = element(app, "#ds-wave").at("box");
	const auto threshold = element(app, "#ds-open-handle").at("box");
	CHECK(std::abs(threshold.at("y").get<double>() - wave.at("y").get<double>() -
			  (0.45 * wave.at("height").get<double>())) < 1);
	CHECK(wave.at("x").get<double>() < view.at("x").get<double>());
	const Json peaks = elements(app, ".ds-peak");
	REQUIRE(!peaks.empty());
	CHECK(peaks.size() <= 256);
	for (const auto& peak : peaks)
	{
		const auto& box = peak.at("box");
		CHECK(box.at("x").get<double>() >= view.at("x").get<double>() - 2);
		CHECK(box.at("x").get<double>() + box.at("width").get<double>() <=
			view.at("x").get<double>() + view.at("width").get<double>() + 2);
	}

	click(app, "#ds-mute-gaps");
	click(app, "#ds-view");
	const auto log = app.call("engine.commandLog").at("commands");
	bool previewed = false;
	for (const auto& entry : log)
		if (entry.at("command") == "audition.start")
		{
			previewed = true;
			const auto& parameters = entry.at("parameters");
			const double fraction =
				(view.at("x").get<double>() + (view.at("width").get<double>() / 2) - wave.at("x").get<double>()) /
				wave.at("width").get<double>();
			CHECK(std::abs(parameters.at("position").get<double>() - (12000 + (fraction * (120000 - 1)))) < 1000);
			CHECK(parameters.at("end") == 132000);
			CHECK(parameters.at("sections").size() == 2);
		}
	CHECK(previewed);
	// The slider can restore the full view; zooming never starts a second analysis job.
	click(app, "#ds-zoom");
	for (int i = 0; i < 16; ++i)
		app.ui().key(audioWindow(app), "Left");
	REQUIRE(app.waitUntil(
		[&] { return std::abs(element(app, "#ds-wave").at("box").at("width").get<double>() - width) < 1; }));
	CHECK(app.call("job.list").at("jobs") == jobs);
	click(app, "#ds-cancel");
}

TEST_CASE("Sample editor audio analysis uses its range instead of the project selection", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	app.call("sampleEditor.open", {{"event", tones.events[1]}});
	app.call("sampleEditor.set", {{"selectionStart", 96000}, {"selectionEnd", 120000}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return app.appUi().sampleEditorWindow() != nullptr &&
				app.appUi().sampleEditorWindow()->event() == tones.events[1];
		}));
	const ui::WindowId editor = app.appUi().sampleEditorWindowId();
	REQUIRE(app.waitUntil(
		[&]
		{
			const Json shown = app.ui().dom(editor, ".se-info [field=selectionStart]", 0);
			return shown.at("elements")[0].value("text", std::string()) == "96000";
		}));
	for (const std::string command : {"audio.detectSilence", "audio.spectrumAnalyzer", "audio.statistics"})
	{
		INFO(command);
		app.ui().click(editor, {"[audio-function=\"" + command + "\"]", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		step(app);
		REQUIRE(app.waitUntil([&] { return audioWindow(app) != app.main().id(); }));
		if (command == "audio.detectSilence")
		{
			click(app, "#ds-analyze");
			REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "1"; }));
			click(app, "#ds-cancel");
		}
		else
		{
			if (command == "audio.statistics")
			{
				REQUIRE(app.waitUntil([&] { return text(app, "#st-tables").find("-12.04 dB") != std::string::npos; }));
				CHECK(text(app, "#st-tables").find("-6.02 dB") == std::string::npos);
			}
			else
			{
				REQUIRE(app.waitUntil([&] { return !elements(app, ".sa-bar").empty(); }));
				const double height = element(app, "#sa-graph").at("box").at("height").get<double>();
				CHECK(barHeight(app, bar(1000)) / height < 0.91);
				CHECK(barHeight(app, bar(1000)) / height > 0.88);
			}
			click(app, command == "audio.statistics" ? "#st-close" : "#sa-close");
		}
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
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "2" && text(app, "#ds-message").empty(); }));
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
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-event").find("(2 of 2)") != std::string::npos; }));
	click(app, "#ds-add-regions");
	click(app, "#ds-auto");
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "2" && text(app, "#ds-message").empty(); }));
	click(app, "#ds-linked");
	type(app, "#ds-open-threshold", "-10");
	CHECK(element(app, "#ds-close-threshold").at("attributes").at("value") == "-10");
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "1" && text(app, "#ds-message").empty(); }));

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

	app.ui().move(audioWindow(app), barTarget(app, bar(1000.0)));
	step(app);
	const std::string value = text(app, "#sa-value");
	CHECK_THAT(value, ContainsSubstring("Mono"));
	CHECK_THAT(value, ContainsSubstring("Hz"));
	CHECK_THAT(value, ContainsSubstring(" B4 ")); // 987 Hz
	CHECK_THAT(value, !ContainsSubstring("D:"));

	// A right click on the tone, then the mouse at 300 Hz: far lower.
	app.ui().click(audioWindow(app), barTarget(app, bar(1000.0)), ui::MouseButton::Right, 1, {});
	step(app);
	CHECK(elements(app, ".sa-bar.reference").size() == 1);
	app.ui().move(audioWindow(app), barTarget(app, bar(300.0)));
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("D: -"));
	app.ui().click(audioWindow(app), barTarget(app, bar(300.0)), ui::MouseButton::Left, 1, {});
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
	app.ui().move(audioWindow(app), barTarget(app, bar(1000.0)));
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("Left"));

	// Choosing the right channel redraws its bars, and the mouse shows its name.
	choose(app, "#sa-channel", "Down");
	CHECK(barHeight(app, bar(3000.0)) > barHeight(app, bar(1000.0)) + 50.0);
	app.ui().move(audioWindow(app), barTarget(app, bar(3000.0)));
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("Right"));

	// All channels keep separate curves; the nearest curve supplies the value and reference across channels.
	choose(app, "#sa-channel", "Down");
	REQUIRE(elements(app, ".sa-bar").size() == 492);
	const std::string left = R"(.sa-bar[channel="1"][index=")" + std::to_string(bar(1000.0)) + "\"]";
	const std::string right = R"(.sa-bar[channel="2"][index=")" + std::to_string(bar(3000.0)) + "\"]";
	click(app, left, ui::MouseButton::Right);
	app.ui().move(audioWindow(app), {right, 0, std::nullopt});
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("Right"));
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("D:"));
	CHECK(elements(app, ".sa-bar.reference[channel=\"1\"]").size() == 1);

	const double width = element(app, right).at("box").at("width").get<double>();
	click(app, "#sa-zoom");
	app.ui().key(audioWindow(app), "Right");
	app.ui().key(audioWindow(app), "Right");
	step(app);
	CHECK(element(app, right).at("box").at("width").get<double>() > width * 2.0);
	CHECK(elements(app, ".sa-bar.reference[channel=\"1\"]").size() == 1);

	app.ui().move(audioWindow(app), {right, 0, std::nullopt});
	step(app);
	CHECK_THAT(text(app, "#sa-value"), ContainsSubstring("Right"));
	CHECK(element(app, "#sa-value").at("attributes").at("index") == std::to_string(bar(3000.0)));

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

TEST_CASE("analysis dialogs stay responsive while their jobs run and cancel them on close", "[ui]")
{
	for (const std::string kind : {"clip.statistics", "clip.spectrum", "clip.detectSilence", "event.stripSilence"})
	{
		INFO(kind);
		test::UiApp app({1000, 760});
		const Tones tones(app);
		showTones(app, tones, 1);
		test::HeldJob held(app.engine(), kind);
		const bool statistics = kind == "clip.statistics";
		const bool spectrum = kind == "clip.spectrum";
		std::string selector = "#detect-silence-dialog";
		std::string command = "audio.detectSilence";
		std::string closeButton = "#ds-cancel";
		if (statistics)
		{
			selector = "#statistics-dialog";
			command = "audio.statistics";
			closeButton = "#st-close";
		}
		else if (spectrum)
		{
			selector = "#spectrum-analyzer-dialog";
			command = "audio.spectrumAnalyzer";
			closeButton = "#sa-close";
		}
		run(app, command, selector, false);
		if (!statistics && !spectrum)
			click(app, kind == "clip.detectSilence" ? "#ds-analyze" : "#ds-process");
		REQUIRE(held.waitUntilHeld());
		REQUIRE(app.waitUntil(
			[&]
			{
				return text(app,
						   statistics     ? "#st-tables"
							   : spectrum ? "#sa-value"
										  : "#ds-message")
						   .find("0%") != std::string::npos;
			}));
		CHECK(audioEvents(app) == 2);
		if (statistics || spectrum)
			app.shell().requestClose(audioWindow(app));
		else
			click(app, closeButton);
		closes(app, selector);
		CHECK(held.release());
		REQUIRE(app.waitUntil(
			[&] { return app.call("job.get", {{"job", held.job()}}).at("job").at("state") == "cancelled"; }));
		CHECK(audioEvents(app) == 2);
	}
}

TEST_CASE("Detect Silence Auto replaces a pending analysis without showing its obsolete result", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	test::HeldJob held(app.engine(), "clip.detectSilence");
	run(app, "audio.detectSilence", "#detect-silence-dialog");
	click(app, "#ds-auto");
	REQUIRE(held.waitUntilHeld());
	type(app, "#ds-open-threshold", "-10");
	REQUIRE(app.waitUntil([&] { return text(app, "#ds-sections") == "1" && text(app, "#ds-message").empty(); }));
	CHECK(held.release());
	REQUIRE(
		app.waitUntil([&] { return app.call("job.get", {{"job", held.job()}}).at("job").at("state") == "cancelled"; }));
	CHECK(text(app, "#ds-sections") == "1");
	click(app, "#ds-cancel");
	closes(app, "#detect-silence-dialog");
}

TEST_CASE("audio functions open independent windows and close with the project", "[ui]")
{
	test::UiApp app({1000, 760});
	const Tones tones(app);
	showTones(app, tones, 1);
	std::vector<ui::WindowId> windows;
	for (const char* command : {"audio.detectSilence", "audio.spectrumAnalyzer", "audio.statistics"})
	{
		app.keys().run(command, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&] { return app.shell().windows().size() == windows.size() + 2; }));
		const ui::Window* window = app.shell().windows().back();
		windows.push_back(window->id());
		CHECK(window->id() != app.main().id());
		REQUIRE(window->context().GetNumDocuments() == 1);
		CHECK(!window->context().GetDocument(0)->IsModal());
		app.keys().run(command, Json::object(), app.main(), nullptr);
		step(app);
		CHECK(app.shell().windows().size() == windows.size() + 1);
	}
	app.shell().requestClose(windows.back());
	step(app);
	CHECK(app.shell().find(windows.back()) == nullptr);
	app.call("project.new");
	REQUIRE(app.waitUntil([&] { return app.shell().windows().size() == 1; }));
}
