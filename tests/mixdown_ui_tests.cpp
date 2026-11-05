// SPDX-License-Identifier: GPL-3.0-or-later
// File > Export in the app's UI (#115, docs/features/mixdown.md): the Export Audio Mixdown dialog and Quick Audio
// Export, driven with ui::Automation as MCP's UI tools drive them. tests/mixdown_tests.cpp checks the engine's export.
#include "uiapp.h"

#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "testutils/testutils.h"
#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
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

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void type(test::UiApp& app, const std::string& selector, const std::string& text)
	{
		click(app, selector);
		app.ui().key(std::nullopt, "Ctrl+A");
		app.ui().text(std::nullopt, text);
	}

	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < down; ++i)
			app.ui().key(std::nullopt, "Down");
		click(app, select);
	}

	bool open(test::UiApp& app)
	{
		return !elements(app, "#export-mixdown-dialog").empty();
	}

	// An audio track named Lead that plays a quarter-level stereo event from 0 to 4800.
	uint64_t addTrack(test::UiApp& app, const test::TempDir& dir)
	{
		const std::filesystem::path wav = dir.path() / "tone.wav";
		test::writeWav(wav, {std::vector<float>(9600, 0.25F), 2, 48000});
		const Json media = app.call("media.add", {{"path", wav.u8string()}});
		const Json clip = app.call("clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id");
		const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
		app.call("track.set", {{"track", track.at("track").at("id")}, {"name", "Lead"}});
		app.call("audioEvent.add",
			{{"track", track.at("track").at("id")}, {"clip", clip}, {"position", 0}, {"offset", 0}, {"length", 4800}});
		return track.at("channel").at("id").get<uint64_t>();
	}
}

TEST_CASE("File > Export > Audio Mixdown exports the checked channels over the locator range", "[ui]")
{
	test::UiApp app({1000, 700});
	const test::TempDir dir;
	const uint64_t channel = addTrack(app, dir);
	app.call("transport.set", {{"left", 1000}, {"right", 3000}});

	app.keys().run("file.exportMixdown", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	app.shell().step(std::chrono::milliseconds(0));
	// The main mix is checked, the track's channel is listed.
	const Json project = app.call("project.get");
	uint64_t mainMix = 0;
	for (const Json& bus : project.at("busses"))
		if (bus.at("mainMix") == true)
			mainMix = bus.at("id").get<uint64_t>();
	CHECK(element(app, "#mixdown-channel-" + std::to_string(mainMix)).at("attributes").contains("checked"));
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	CHECK(!element(app, "#mixdown-channel-" + std::to_string(channel)).at("attributes").contains("checked"));
	CHECK(element(app, "#mixdown-format").at("value") == "wav");
	CHECK(element(app, "#mixdown-bit-depth").at("value") == "int24");
	CHECK(element(app, "#mixdown-sample-rate").at("value") == "48000");

	// Nothing checked: the dialog says so and stays.
	click(app, "#mixdown-channel-" + std::to_string(mainMix));
	click(app, "#mixdown-export");
	CHECK_THAT(element(app, "#mixdown-message").value("text", std::string()), ContainsSubstring("Choose a channel"));
	REQUIRE(open(app));

	click(app, "#mixdown-channel-" + std::to_string(channel));
	type(app, "#mixdown-name", "take");
	type(app, "#mixdown-folder", (dir.path() / "out").u8string());
	choose(app, "#mixdown-format", 2); // FLAC, in 24 bits
	click(app, "#mixdown-export");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));

	const std::unique_ptr<engine::AudioFileReader> reader =
		engine::AudioFileReader::open((dir.path() / "out" / "take.flac").u8string());
	CHECK(reader->info().format == engine::AudioFormat::Flac);
	CHECK(reader->info().frames == 2000);
	CHECK(reader->info().channels == 2);
}

TEST_CASE("File > Export > Quick Audio Export writes the whole project into the WAV file chosen", "[ui]")
{
	test::UiApp app({1000, 700});
	const test::TempDir dir;
	addTrack(app, dir);
	app.keys().run("file.quickExport", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
	CHECK(app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {dir.path() / "song"}));
	app.shell().step(std::chrono::milliseconds(0));
	app.call("project.revision"); // after the export, which the command thread runs first
	const std::filesystem::path file = dir.path() / "song.wav";
	REQUIRE(std::filesystem::exists(file));
	const std::unique_ptr<engine::AudioFileReader> reader = engine::AudioFileReader::open(file.u8string());
	CHECK(reader->info().frames == 4800);
	CHECK(reader->info().sampleFormat == engine::SampleFormat::Int24);
}
