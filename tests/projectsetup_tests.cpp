// SPDX-License-Identifier: GPL-3.0-or-later
// The project setup (#78, docs/features/project-setup.md): the project's settings in the engine, and the dialog that
// edits them in the app's UI, driven with ui::Automation as MCP's UI tools drive it. Also the status line's record
// format with the record bit depth (#454).
#include "uiapp.h"

#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/timeformat.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json run(engine::Client& client, const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(engine::Client& client, const std::string& command, Json parameters)
	{
		const engine::Result result = client.call(command, std::move(parameters));
		// Not REQUIRE_FALSE: Catch2's Normal | FalseTest trips clang-analyzer's EnumCastOutOfRange check
		REQUIRE(!result.ok());
		return result.error;
	}

	// The project setup's settings in project.get.
	Json setup(const Json& project)
	{
		Json settings = Json::object();
		for (const char* key :
			{"recordFileType", "recordBitDepth", "frameRate", "startTime", "length", "barOffset", "timeFormat"})
			settings[key] = project.at(key);
		return settings;
	}

	// A function, not a global: building a Json may throw, which a static initializer can't catch.
	Json changedSetup()
	{
		return {{"recordFileType", "broadcastWav"}, {"recordBitDepth", "float32"}, {"frameRate", "29.97dfps"},
			{"startTime", 3600.0}, {"length", 300.0}, {"barOffset", -1}, {"timeFormat", "timecode"}};
	}

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
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

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Replaces the text of a text field.
	void type(test::UiApp& app, const std::string& selector, const std::string& text)
	{
		click(app, selector);
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
	}

	// Opens a drop-down list, moves the selection that many options down (up when negative) with the arrow keys, and
	// closes it. Its options aren't in the DOM that the UI tools see.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < std::abs(down); ++i)
			app.ui().key(std::nullopt, down > 0 ? "Down" : "Up");
		click(app, select);
	}

	bool open(test::UiApp& app)
	{
		return !elements(app, "#project-setup").empty();
	}

	// Opens the dialog through its UI command, as the main menu does.
	void openDialog(test::UiApp& app)
	{
		app.keys().run("window.projectSetup", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return open(app); }));
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool closed(test::UiApp& app)
	{
		return app.waitUntil([&app] { return !open(app); });
	}
}

TEST_CASE("the project setup's settings are set, announced, undone and saved", "[integration]")
{
	engine::Engine engine;
	engine::Client client(engine);
	engine::Client listener(engine);
	CHECK(setup(run(client, "project.get")) ==
		Json({{"recordFileType", "wav"}, {"recordBitDepth", "int24"}, {"frameRate", "25"}, {"startTime", 0.0},
			{"length", 600.0}, {"barOffset", 0}, {"timeFormat", "barsBeats"}}));

	run(client, "project.set", changedSetup());
	const Json changed = run(client, "project.get");
	CHECK(setup(changed) == changedSetup());
	Json announced;
	while (const auto message = listener.poll())
		if (const auto* event = std::get_if<engine::Event>(&*message);
			event != nullptr && event->name == "project.changed")
			announced = event->data;
	REQUIRE(announced.is_object());
	CHECK(setup(announced) == changedSetup());
	// The record format reaches the audio engine, for the status line (#454) and recording.
	CHECK(engine.renderSnapshots().acquire().recordBitDepth == "float32");
	CHECK(run(client, "audio.get").at("recordBitDepth") == "float32");

	run(client, "history.undo");
	CHECK(setup(run(client, "project.get")).at("recordBitDepth") == "int24");
	CHECK(run(client, "audio.get").at("recordBitDepth") == "int24");
	run(client, "history.redo");
	CHECK(run(client, "project.get") == changed);

	const test::TempDir dir;
	const std::string file = (dir.path() / "setup.dawproject").u8string();
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	CHECK(setup(run(client, "project.get")).at("frameRate") == "25");
	run(client, "project.load", {{"path", file}});
	CHECK(setup(run(client, "project.get")) == changedSetup());
}

TEST_CASE("project.set refuses project setup settings that don't go together", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	const Json before = run(client, "project.get");
	CHECK_THAT(reject(client, "project.set", {{"recordFileType", "flac"}, {"recordBitDepth", "float32"}}),
		ContainsSubstring("FLAC"));
	CHECK_THAT(reject(client, "project.set", {{"recordFileType", "aiff"}, {"recordBitDepth", "float32"}}),
		ContainsSubstring("AIFF"));
	CHECK_THAT(reject(client, "project.set", {{"recordFileType", "flac"}, {"recordBitDepth", "int32"}}),
		ContainsSubstring("FLAC"));
	CHECK_THAT(reject(client, "project.set", {{"startTime", -1.0}}), ContainsSubstring("startTime"));
	CHECK_THAT(reject(client, "project.set", {{"length", 0.0}}), ContainsSubstring("length"));
	CHECK_THAT(reject(client, "project.set", {{"barOffset", 100000}}), ContainsSubstring("barOffset"));
	reject(client, "project.set", {{"frameRate", "26"}});
	reject(client, "project.set", {{"recordBitDepth", "8"}});
	CHECK(run(client, "project.get") == before);

	// Both at once is fine.
	run(client, "project.set", {{"recordFileType", "flac"}, {"recordBitDepth", "int16"}});
	CHECK(run(client, "project.get").at("recordFileType") == "flac");
}

TEST_CASE("the time displays follow the project's frame rate, start time and bar offset", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "project.set", {{"frameRate", "30"}, {"startTime", 3600.0}, {"barOffset", -1}});
	const ui::TimeContext context = ui::projectTime(run(client, "project.get"));
	CHECK(context.frameRate.framesPerSecond == 30);
	// Positions count from the start time and bar offset; lengths don't.
	CHECK(ui::formatTime(ui::TimeFormat::Timecode, 0.5, context) == "01:00:00:15");
	CHECK(ui::formatTime(ui::TimeFormat::Seconds, 1.5, context) == "1:00:01.500");
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 0.0, context) == "0.1.1.0");
	CHECK(ui::formatTime(ui::TimeFormat::Samples, 1.0, context) == "48000");
	CHECK(ui::formatLength(ui::TimeFormat::Timecode, 10.0, 11.0, context) == "00:00:01:00");
	CHECK(ui::parseTime(ui::TimeFormat::Timecode, "01:00:01:00", context) == 1.0);
	CHECK(ui::parseTime(ui::TimeFormat::BarsBeats, "1", context) == 2.0); // bar 2 at 120 BPM in 4/4
	CHECK(ui::parseLength(ui::TimeFormat::Seconds, "2", 10.0, context) == 2.0);

	ui::TimeContext read;
	REQUIRE(ui::readTimeContext(run(client, "project.get"), read));
	CHECK(read.startTime == 3600.0);
	CHECK(read.barOffset == -1);
}

TEST_CASE("the project setup dialog shows the project's settings and changes them through the engine", "[ui]")
{
	test::UiApp app({900, 700});
	openDialog(app);
	CHECK(element(app, "#setup-sample-rate").at("value") == "48000");
	CHECK(element(app, "#setup-bit-depth").at("value") == "int24");
	CHECK(element(app, "#setup-file-type").at("value") == "wav");
	CHECK(element(app, "#setup-frame-rate").at("value") == "25");
	CHECK(element(app, "#setup-start").at("value") == "00:00:00:00");
	CHECK(element(app, "#setup-length").at("value") == "00:10:00:00");
	CHECK(element(app, "#setup-bar-offset").at("value") == "0");
	CHECK(element(app, "#setup-time-format").at("value") == "barsBeats");
	CHECK(element(app, "#setup-pan-law").at("value") == "-3dB");
	CHECK(element(app, "#setup-max-volume").at("value") == "6");

	choose(app, "#setup-bit-depth", 2);  // 32 bit float
	choose(app, "#setup-frame-rate", 3); // 30, after 29.97 and 29.97 dfps
	type(app, "#setup-start", "01:00:00:00");
	type(app, "#setup-length", "5:00:00");
	type(app, "#setup-bar-offset", "-1");
	choose(app, "#setup-pan-law", 1); // -4.5 dB
	choose(app, "#setup-max-volume", 1);
	click(app, "#setup-ok");
	CHECK(closed(app));
	const Json project = app.call("project.get");
	CHECK(project.at("recordBitDepth") == "float32");
	CHECK(project.at("frameRate") == "30");
	CHECK(project.at("startTime") == 3600.0);
	CHECK(project.at("length") == 300.0);
	CHECK(project.at("barOffset") == -1);
	CHECK(project.at("panLaw") == "-4.5dB");
	CHECK(project.at("maxVolume") == 12.0);
	// One undo step.
	CHECK(app.call("history.get").at("steps").back().at("action") == "Project Setup");

	// Cancel drops what changed; what the engine refuses keeps the dialog open with the message.
	openDialog(app);
	CHECK(element(app, "#setup-start").at("value") == "01:00:00:00");
	type(app, "#setup-bar-offset", "7");
	click(app, "#setup-cancel");
	CHECK(closed(app));
	CHECK(app.call("project.get").at("barOffset") == -1);
	openDialog(app);
	type(app, "#setup-length", "soon");
	click(app, "#setup-ok");
	CHECK(open(app));
	CHECK_THAT(text(app, "#project-setup-message"), ContainsSubstring("Length"));
}

TEST_CASE("the project setup dialog conforms the audio files to a new sample rate", "[ui]")
{
	// Reference manual p. 128: audio files must be converted to a new project sample rate to play back properly.
	test::UiApp app({900, 700});
	app.appUi().showProject(); // the pool, which shows how the conform job goes, is in the project window
	const test::TempDir dir;
	const std::filesystem::path wav = dir.path() / "tone.wav";
	test::writeWav(wav, {std::vector<float>(4800, 0.25F), 1, 48000});
	app.call("media.add", {{"path", wav.u8string()}});

	// The files are conformed into the project's media folder: an unsaved project keeps the dialog open, unchanged.
	openDialog(app);
	CHECK(element(app, "#setup-conform").at("attributes").contains("checked"));
	choose(app, "#setup-sample-rate", -1); // 44100, before 48000
	click(app, "#setup-ok");
	CHECK(open(app));
	CHECK_THAT(text(app, "#project-setup-message"), ContainsSubstring("save the project first"));
	CHECK(app.call("project.get").at("sampleRate") == 48000.0);
	click(app, "#setup-cancel");
	CHECK(closed(app));

	// The pool's Conform Files job converts them once the sample rate is set; the pool opens to show it.
	app.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	openDialog(app);
	choose(app, "#setup-sample-rate", -1);
	click(app, "#setup-ok");
	CHECK(closed(app));
	CHECK(app.call("project.get").at("sampleRate") == 44100.0);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("media")[0].at("sampleRate") == 44100.0; },
		std::chrono::seconds(30)));
	CHECK(app.call("project.get").at("media")[0].at("length") == 4410);
	CHECK(app.waitUntil(
		[&app]
		{
			const Json line = elements(app, "#pool-message");
			return !line.empty() && line[0].value("text", "") == "Conformed 1 file to the project's sample rate.";
		}));
	// The sample rate and the files are one undo step, and change back together.
	const Json steps = app.call("history.get").at("steps");
	CHECK(steps.back().at("action") == "Project Setup");
	app.call("history.undo");
	CHECK(app.call("project.get").at("sampleRate") == 48000.0);
	CHECK(app.call("project.get").at("media")[0].at("sampleRate") == 48000.0);
	CHECK(app.call("history.get").at("position") == steps.size() - 1);

	// Without Conform, the files keep their sample rate.
	openDialog(app);
	click(app, "#setup-conform");
	choose(app, "#setup-sample-rate", 1); // 88200, after 48000
	click(app, "#setup-ok");
	CHECK(closed(app));
	CHECK(app.call("project.get").at("sampleRate") == 88200.0);
	CHECK(app.call("project.get").at("media")[0].at("sampleRate") == 48000.0);
}

TEST_CASE("the project setup dialog needs no saved project when no file it would conform is there", "[ui]")
{
	test::UiApp app({900, 700});
	const test::TempDir dir;
	// A missing file at 32 kHz, which Conform Files leaves out, and one at the new rate already.
	const Json missing = app.call("media.add",
		{{"path", (dir.path() / "gone.wav").u8string()}, {"sampleRate", 32000.0}, {"channels", 1}, {"length", 320}});
	app.call("clip.add", {{"media", missing.at("media").at("id")}});
	test::writeWav(dir.path() / "slow.wav", {std::vector<float>(4410, 0.25F), 1, 44100});
	app.call("media.add", {{"path", (dir.path() / "slow.wav").u8string()}});

	openDialog(app);
	choose(app, "#setup-sample-rate", -1); // 44100, before 48000
	click(app, "#setup-ok");
	CHECK(closed(app));
	CHECK(app.call("project.get").at("sampleRate") == 44100.0);
	CHECK(app.call("job.list").at("jobs").empty());
}

TEST_CASE("the status line's record format shows the project's record bit depth", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz - 24 bit"; }));
	app.call("project.set", {{"recordBitDepth", "float32"}});
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz - 32 bit float"; }));
	app.call("history.undo");
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz - 24 bit"; }));

	// A click on it opens the project setup dialog (reference manual p. 71).
	app.appUi().docking().setVisible("project", "statusLine", true);
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#record-format");
	CHECK(app.waitUntil([&app] { return open(app); }));
}
