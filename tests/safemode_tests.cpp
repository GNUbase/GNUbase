// SPDX-License-Identifier: GPL-3.0-or-later
// The safe start mode (#163, docs/features/profiles.md, reference manual pp. 1584-1586): the command line's options,
// the user settings it disables or deletes, and third-party plugins deactivated.
#include "app/options.h"
#include "engine/engine.h"
#include "engine/plugins/hostmanager.h"
#include "platform/files.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using app::SafeSettings;

TEST_CASE("the command line chooses the safe start mode's options", "[unit]")
{
	const app::Options plain = app::parseOptions({});
	CHECK(!plain.safeMode);
	CHECK(!plain.noThirdPartyPlugins);
	CHECK(plain.settingsMode == SafeSettings::Use);
	CHECK(app::parseOptions({"--safe-mode"}).safeMode);
	CHECK(app::parseOptions({"--no-third-party-plugins"}).noThirdPartyPlugins);
	CHECK(app::parseOptions({"--factory-settings"}).settingsMode == SafeSettings::Disable);
	CHECK(app::parseOptions({"--delete-settings"}).settingsMode == SafeSettings::Delete);
	CHECK_THROWS_AS(app::parseOptions({"--factory-settings", "--delete-settings"}), app::UsageError);
	CHECK_THAT(app::usage(), ContainsSubstring("--safe-mode"));
	CHECK_THAT(app::usage(), ContainsSubstring("--no-third-party-plugins"));
}

TEST_CASE("the safe start mode disables or deletes the user settings", "[unit]")
{
	const test::TempDir dir;
	const auto write = [&dir](const char* name) { files::writeAtomically(dir.path() / name, "{}"); };
	write("settings.json");
	write("quantize-presets.json");
	write("recent-projects.json");
	app::Options options = app::parseOptions({"--settings", dir.path().u8string()});

	// As usual: the folder, as it is.
	CHECK(app::safeSettingsDirectory(options) == dir.path());
	CHECK(std::filesystem::exists(dir.path() / "settings.json"));

	// Disabled: the engine keeps the profile's settings in memory, the files stay.
	options.settingsMode = SafeSettings::Disable;
	CHECK(app::safeSettingsDirectory(options) == dir.path());
	CHECK(app::factorySettings(options));
	CHECK(std::filesystem::exists(dir.path() / "settings.json"));

	// Deleted: the files a profile has go, the recent projects stay.
	options.settingsMode = SafeSettings::Delete;
	CHECK(app::safeSettingsDirectory(options) == dir.path());
	CHECK(!std::filesystem::exists(dir.path() / "settings.json"));
	CHECK(!std::filesystem::exists(dir.path() / "quantize-presets.json"));
	CHECK(std::filesystem::exists(dir.path() / "recent-projects.json"));
}

TEST_CASE("the factory settings leave the profile's files alone and keep the recent projects and templates", "[unit]")
{
	const test::TempDir dir;
	{
		engine::Engine engine(audio::makeBackends(), dir.path());
		engine::Client client(engine);
		REQUIRE(client.call("settings.set", {{"key", "ui.scale"}, {"value", 1.5}}).ok());
		REQUIRE(client.call("project.saveTemplate", {{"name", "Band"}}).ok());
	}
	const std::string settings = files::readFile(dir.path() / "settings.json").value_or("");

	engine::Engine engine(audio::makeBackends(), dir.path(), midi::makeBackends(), true);
	engine::Client client(engine);
	CHECK(client.call("settings.get", {{"key", "ui.scale"}}).value.at("value") == 1.0);
	CHECK(client.call("project.templates").value.dump().find("Band") != std::string::npos);
	REQUIRE(client.call("settings.set", {{"key", "ui.scale"}, {"value", 2.0}}).ok());
	CHECK(files::readFile(dir.path() / "settings.json").value_or("") == settings);
	CHECK(!client.call("profile.list").ok());
}

TEST_CASE("deleting the settings in the safe start mode drops a profile switch, for the factory settings", "[unit]")
{
	const test::TempDir dir;
	{
		engine::Engine engine(audio::makeBackends(), dir.path());
		engine::Client client(engine);
		REQUIRE(client.call("settings.set", {{"key", "ui.scale"}, {"value", 1.5}}).ok());
		REQUIRE(client.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}}).ok());
		REQUIRE(client.call("profile.activate", {{"name", "Studio"}}).ok());
	}
	const app::Options options = app::parseOptions({"--settings", dir.path().u8string(), "--delete-settings"});
	engine::Engine engine(audio::makeBackends(), app::safeSettingsDirectory(options));
	engine::Client client(engine);
	CHECK(client.call("settings.get", {{"key", "ui.scale"}}).value.at("value") == 1.0);
	const engine::Json profiles = client.call("profile.list").value;
	CHECK(profiles.at("active") == "Default");
	CHECK(profiles.at("next") == "");
}

TEST_CASE("third-party plugins deactivated in the safe start mode don't load", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	CHECK(client.call("pluginhost.get").value.at("deactivated") == false);
	engine.pluginHosts().setDeactivated(true);
	CHECK(client.call("pluginhost.get").value.at("deactivated") == true);
	std::future<std::unique_ptr<plugin::remote::RemotePlugin>> loading =
		engine.pluginHosts().load({"clap:test.plugin", "test.clap", "Test"});
	CHECK_THROWS_WITH(loading.get(), ContainsSubstring("safe mode"));
}
