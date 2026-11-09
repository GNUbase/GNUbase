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

	// Disabled: the settings are kept in memory, the files stay.
	options.settingsMode = SafeSettings::Disable;
	CHECK(app::safeSettingsDirectory(options).empty());
	CHECK(std::filesystem::exists(dir.path() / "settings.json"));

	// Deleted: the files a profile has go, the recent projects stay.
	options.settingsMode = SafeSettings::Delete;
	CHECK(app::safeSettingsDirectory(options) == dir.path());
	CHECK(!std::filesystem::exists(dir.path() / "settings.json"));
	CHECK(!std::filesystem::exists(dir.path() / "quantize-presets.json"));
	CHECK(std::filesystem::exists(dir.path() / "recent-projects.json"));
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
