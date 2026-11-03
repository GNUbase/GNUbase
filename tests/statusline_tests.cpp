// SPDX-License-Identifier: GPL-3.0-or-later
// The project window's status line (#407, docs/features/status-line.md) on the dummy device: the record format with a
// sample rate mismatch, and a lost audio device, as they follow audio.changed; the remaining record time (#104).
#include "uiapp.h"

#include "engine/engine.h"
#include "testutils/testutils.h"
#include "ui/automation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <vector>

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
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz - 24 bit"; }));
	CHECK_FALSE(has(app, "#record-format", "mismatch"));
	CHECK(has(app, "#lost-device", "hidden"));

	// The dummy device doesn't run at 16 kHz, so it stays at 48 kHz and plays the project faster.
	app.call("project.set", {{"sampleRate", 16000.0}});
	CHECK(app.waitUntil([&] { return has(app, "#record-format", "mismatch"); }));
	CHECK(text(app, "#record-format") == "Record: 16 kHz - 24 bit");
	app.call("project.set", {{"sampleRate", 44100.0}}); // it does at 44.1 kHz
	CHECK(app.waitUntil([&] { return !has(app, "#record-format", "mismatch"); }));
	CHECK(text(app, "#record-format") == "Record: 44.1 kHz - 24 bit");

	// Unplugged: shown by its name, with the reason and its id as the tooltip, until it is plugged in again.
	app.call("audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(app.waitUntil([&] { return !has(app, "#lost-device", "hidden"); }));
	CHECK(text(app, "#lost-device") == "Audio Device Lost: Dummy device");
	const std::string title = element(app, "#lost-device").at("attributes").at("title").get<std::string>();
	CHECK_THAT(title, ContainsSubstring("unplugged"));
	CHECK_THAT(title, ContainsSubstring("(dummy)"));
	app.call("audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(app.waitUntil([&] { return has(app, "#lost-device", "hidden"); }, std::chrono::seconds(10)));
}

TEST_CASE(
	"recording.remainingTime divides the free disk space by what the armed tracks record a second", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters = Json::object())
	{
		const engine::Result result = client.call(command, parameters);
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	run("project.save", {{"path", (dir.path() / "time.dawproject").u8string()}});
	Json time = run("recording.remainingTime");
	CHECK(time.at("seconds").is_null()); // nothing armed
	CHECK(time.at("channels") == 0);
	CHECK(time.at("availableBytes").get<double>() > 0.0);
	CHECK(std::filesystem::u8path(time.at("folder").get<std::string>()) == dir.path() / "Audio");

	// A mono and a stereo track, 24 bit at 48 kHz: 9 bytes a frame. A track without an input bus doesn't record.
	std::vector<Json> channels;
	for (const int count : {1, 2, 2})
	{
		const Json added = run("track.add", {{"type", "audio"}, {"channels", count}});
		channels.push_back(added.at("channel").at("id"));
		run("channel.set", {{"channel", channels.back()}, {"recordEnable", true}});
	}
	run("channel.set", {{"channel", channels.back()}, {"input", 0}});
	run("project.set", {{"sampleRate", 48000.0}, {"recordBitDepth", "int24"}});
	time = run("recording.remainingTime");
	CHECK(time.at("channels") == 3);
	CHECK(time.at("seconds").get<double>() == Catch::Approx(time.at("availableBytes").get<double>() / (48000.0 * 9)));
	run("project.set", {{"recordBitDepth", "float64"}});
	time = run("recording.remainingTime");
	CHECK(time.at("seconds").get<double>() == Catch::Approx(time.at("availableBytes").get<double>() / (48000.0 * 24)));
}

TEST_CASE("the status line shows the remaining record time of the record-enabled tracks", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	CHECK(app.waitUntil([&] { return text(app, "#record-time") == "Max. Record Time: -"; }));
	const Json added = app.call("track.add", {{"type", "audio"}, {"channels", 1}});
	app.call("channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", true}});
	// Hours and minutes, or minutes and seconds under an hour; it follows within a second or so.
	const std::regex shown(R"(Max\. Record Time: (\d+ h \d\d min|\d+ min \d\d s))");
	CHECK(app.waitUntil([&] { return std::regex_match(text(app, "#record-time"), shown); }));
}
