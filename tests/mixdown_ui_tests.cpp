// SPDX-License-Identifier: GPL-3.0-or-later
// File > Export in the app's UI (#115, docs/features/mixdown.md): the Export Audio Mixdown dialog and Quick Audio
// Export, driven with ui::Automation as MCP's UI tools drive them. tests/mixdown_tests.cpp checks the engine's export.
#include "uiapp.h"

#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "testutils/testutils.h"
#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
	}

	// Opens the select, moves down that many options (up for a negative number) and closes it.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < std::abs(down); ++i)
			app.ui().key(std::nullopt, down < 0 ? "Up" : "Down");
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
	type(app, "#mixdown-name", "");
	click(app, "#mixdown-export");
	CHECK_THAT(element(app, "#mixdown-message").value("text", std::string()), ContainsSubstring("Enter a name"));
	REQUIRE(open(app));
	type(app, "#mixdown-name", "take");
	type(app, "#mixdown-folder", (dir.path() / "out").u8string());
	choose(app, "#mixdown-format", 2); // FLAC, in 24 bits
	click(app, "#mixdown-export");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
	// The export renders as a background job, which holds the file's name with an empty file until it ends (#1794).
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json jobs = app.call("job.list").at("jobs");
			return !jobs.empty() &&
				std::none_of(jobs.begin(), jobs.end(), [](const Json& job) { return job.at("state") == "running"; });
		}));
	REQUIRE(std::filesystem::exists(dir.path() / "out" / "take.flac"));

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
	std::vector<std::filesystem::path> opened;
	app.appUi().openExternallyWith([&opened](const std::filesystem::path& path, bool) { opened.push_back(path); });
	const auto quickExport = [&app, &dir]
	{
		app.keys().run("file.quickExport", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
		CHECK(app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {dir.path() / "song"}));
		app.shell().step(std::chrono::milliseconds(0));
	};
	// Without events, the export shows why it failed (#718).
	quickExport();
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#project-error-dialog").empty(); }));
	CHECK_THAT(element(app, "#project-error-message").value("text", std::string()), ContainsSubstring("no events"));
	click(app, "#project-error-dialog button[result=ok]");
	CHECK(opened.empty());

	addTrack(app, dir);
	quickExport();
	// Then the file's folder opens, as in the reference (manual p. 1479). The job renders the whole 30 s tail before it
	// trims it, which takes longer than waitUntil's default 5 s under TSan.
	REQUIRE(app.waitUntil([&opened] { return !opened.empty(); }, std::chrono::seconds(60)));
	const std::filesystem::path file = dir.path() / "song.wav";
	CHECK(std::filesystem::equivalent(opened.front(), dir.path()));
	REQUIRE(std::filesystem::exists(file));
	const std::unique_ptr<engine::AudioFileReader> reader = engine::AudioFileReader::open(file.u8string());
	CHECK(reader->info().frames == 4800);
	CHECK(reader->info().sampleFormat == engine::SampleFormat::Int24);
}

TEST_CASE("the Export Audio Mixdown dialog offers dither only for 16 and 24 bits", "[ui]")
{
	test::UiApp app({1000, 700});
	app.keys().run("file.exportMixdown", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	app.shell().step(std::chrono::milliseconds(0));
	const auto disabled = [&app] { return element(app, "#mixdown-dither").at("attributes").contains("disabled"); };
	CHECK(!disabled());                   // 24 bits
	choose(app, "#mixdown-bit-depth", 1); // 32 bits, which the engine doesn't dither (#718)
	CHECK(disabled());
	choose(app, "#mixdown-bit-depth", 1); // 32-bit float
	CHECK(disabled());
	choose(app, "#mixdown-bit-depth", -3); // 16 bits
	CHECK(!disabled());
	click(app, "#mixdown-cancel");
}

TEST_CASE("background render progress can cancel and track rows show their render lock", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	const test::TempDir dir;
	const uint64_t channel = addTrack(app, dir);
	uint64_t track = 0;
	const Json project = app.call("project.get");
	for (const Json& item : project.at("tracks"))
		if (item.at("channel") == channel)
			track = item.at("id").get<uint64_t>();
	REQUIRE(track != 0);
	const std::string command =
		GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"), std::string("mixdown.export"));
	test::HeldJob held(app.engine(), command);
	Json parameters = {{"tracks", {track}}, {"tail", 0}};
	if (command == "mixdown.export")
		parameters = {{"folder", dir.path().u8string()}, {"name", "mix"}, {"start", 0}, {"end", 4800}};
	const Json job = app.call(command, parameters).at("job");
	REQUIRE(held.waitUntilHeld());
	REQUIRE(app.waitUntil([&] { return !elements(app, "#render-progress-dialog").empty(); }));
	const std::string locked = "#track-list listitem.rendering[value=\"" + std::to_string(track) + "\"]";
	if (command != "mixdown.export")
	{
		const bool shown = app.waitUntil([&] { return !elements(app, locked).empty(); });
		INFO(elements(app, "#track-list listitem").dump());
		INFO(app.call("track.renderLocks").dump());
		REQUIRE(shown);
	}
	click(app, "#render-progress-dialog button");
	REQUIRE(app.waitUntil(
		[&]
		{
			return app.call("job.get", {{"job", job.at("id")}}).at("job").at("state") == "running" &&
				elements(app, "#render-progress-dialog").empty();
		}));
	CHECK(held.release());
	REQUIRE(app.waitUntil(
		[&] { return app.call("job.get", {{"job", job.at("id")}}).at("job").at("state") == "cancelled"; }));
	REQUIRE(app.waitUntil([&] { return elements(app, locked).empty(); }));
	app.call("track.remove", {{"track", track}});
}
