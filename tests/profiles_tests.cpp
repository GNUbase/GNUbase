// SPDX-License-Identifier: GPL-3.0-or-later
// Profiles (#163, docs/features/profiles.md): named sets of the user settings in the settings folder, added,
// duplicated, renamed, deleted, exported and imported, and activated when GNUbase starts again.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	// GNUbase with its user settings in a folder, which starts again as often as a test likes.
	struct Fixture
	{
		Fixture() { start(); }

		void start()
		{
			client.reset();
			engine.reset();
			engine = std::make_unique<engine::Engine>(audio::makeBackends(), dir.path());
			client = std::make_unique<engine::Client>(*engine);
		}

		Json call(const std::string& command, Json parameters = Json::object()) const
		{
			const engine::Result result = client->call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters) const
		{
			const engine::Result result = client->call(command, std::move(parameters));
			REQUIRE(!result.ok());
			return result.error;
		}

		double scale() const { return call("settings.get", {{"key", "ui.scale"}}).at("value").get<double>(); }
		void setScale(double scale) const { call("settings.set", {{"key", "ui.scale"}, {"value", scale}}); }

		std::vector<std::string> names() const
		{
			std::vector<std::string> result;
			const Json list = call("profile.list");
			for (const Json& profile : list.at("profiles"))
				result.push_back(profile.at("name").get<std::string>());
			return result;
		}

		test::TempDir dir;
		std::unique_ptr<engine::Engine> engine;
		std::unique_ptr<engine::Client> client;
	};
}

TEST_CASE("profiles keep sets of the user settings and become active when GNUbase starts again", "[unit]")
{
	Fixture f;
	CHECK(f.call("profile.list") ==
		Json{{"profiles", {{{"name", "Default"}, {"active", true}}}}, {"active", "Default"}, {"next", ""}});
	f.setScale(1.5);

	// New has the factory settings, Duplicate the settings of the profile it copies.
	f.call("profile.add", {{"name", "Studio"}});
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Laptop"}});
	CHECK(f.names() == std::vector<std::string>{"Default", "Laptop", "Studio"});
	CHECK_THAT(f.fails("profile.add", {{"name", "Studio"}}), ContainsSubstring("there is a profile"));
	CHECK_THAT(f.fails("profile.add", {{"name", "a/b"}}), ContainsSubstring("characters"));
	CHECK_THAT(f.fails("profile.add", {{"name", ""}}), ContainsSubstring("1 to 100"));

	// Activating one waits for the next start.
	const Json activated = f.call("profile.activate", {{"name", "Studio"}});
	CHECK(activated.at("active") == "Default");
	CHECK(activated.at("next") == "Studio");
	CHECK(f.scale() == 1.5);
	f.start();
	CHECK(f.call("profile.list").at("active") == "Studio");
	CHECK(f.call("profile.list").at("next") == "");
	CHECK(f.scale() == 1.0); // the factory settings
	f.setScale(2.0);

	// The other profiles keep their settings meanwhile.
	f.call("profile.activate", {{"name", "Laptop"}});
	f.start();
	CHECK(f.scale() == 1.5);
	f.call("profile.activate", {{"name", "Studio"}});
	f.start();
	CHECK(f.scale() == 2.0);

	// Rename, also the active one; the active one can't be deleted.
	f.call("profile.rename", {{"name", "Studio"}, {"newName", "Home"}});
	f.call("profile.rename", {{"name", "Laptop"}, {"newName", "Travel"}});
	CHECK(f.names() == std::vector<std::string>{"Default", "Home", "Travel"});
	CHECK(f.call("profile.list").at("active") == "Home");
	CHECK_THAT(f.fails("profile.remove", {{"name", "Home"}}), ContainsSubstring("active profile"));
	f.call("profile.activate", {{"name", "Travel"}});
	f.call("profile.remove", {{"name", "Travel"}});
	CHECK(f.names() == std::vector<std::string>{"Default", "Home"});
	CHECK(f.call("profile.list").at("next") == "");
	f.start();
	CHECK(f.scale() == 2.0);
}

TEST_CASE("profiles are exported to a file and imported on another computer", "[unit]")
{
	const Fixture f;
	f.setScale(1.25);
	const test::TempDir other;
	const std::string file = (other.path() / "studio.json").u8string();
	f.call("profile.export", {{"name", "Default"}, {"file", file}});

	// Another computer, with settings of its own.
	Fixture computer;
	// Under the name it has in the file, which is taken here, or under another one.
	CHECK_THAT(computer.fails("profile.import", {{"file", file}}), ContainsSubstring("there is a profile Default"));
	computer.call("profile.import", {{"file", file}, {"name", "Studio"}});
	CHECK(computer.names() == std::vector<std::string>{"Default", "Studio"});
	CHECK_THAT(computer.fails("profile.import", {{"file", file}, {"name", "Studio"}}), ContainsSubstring("there is"));
	CHECK_THAT(computer.fails("profile.import", {{"file", (other.path() / "nothing.json").u8string()}}),
		ContainsSubstring("can't be read"));
	computer.call("profile.activate", {{"name", "Studio"}});
	computer.start();
	CHECK(computer.scale() == 1.25);
}

TEST_CASE("without a settings folder there are no profiles", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	const engine::Result result = client.call("profile.list");
	CHECK(!result.ok());
	CHECK_THAT(result.error, ContainsSubstring("in memory only"));
}

TEST_CASE("profile names that differ only in case are taken, as the folders aren't case-sensitive everywhere", "[unit]")
{
	const Fixture f;
	f.call("profile.add", {{"name", "Studio"}});
	CHECK_THAT(f.fails("profile.add", {{"name", "default"}}), ContainsSubstring("there is a profile"));
	CHECK_THAT(f.fails("profile.add", {{"name", "STUDIO"}}), ContainsSubstring("there is a profile"));
	CHECK_THAT(f.fails("profile.duplicate", {{"name", "Default"}, {"newName", "studio"}}),
		ContainsSubstring("there is a profile"));
	CHECK_THAT(f.fails("profile.rename", {{"name", "Studio"}, {"newName", "DEFAULT"}}),
		ContainsSubstring("there is a profile"));
	const test::TempDir other;
	const std::string file = (other.path() / "studio.json").u8string();
	f.call("profile.export", {{"name", "Default"}, {"file", file}});
	CHECK_THAT(
		f.fails("profile.import", {{"file", file}, {"name", "studio"}}), ContainsSubstring("there is a profile"));
	CHECK(f.names() == std::vector<std::string>{"Default", "Studio"});

	// A profile renamed to itself in another case keeps its settings.
	f.call("profile.rename", {{"name", "Studio"}, {"newName", "studio"}});
	f.call("profile.rename", {{"name", "Default"}, {"newName", "DEFAULT"}});
	CHECK(f.names() == std::vector<std::string>{"DEFAULT", "studio"});
}

TEST_CASE("profile names that Windows keeps for devices are refused", "[unit]")
{
	const Fixture f;
	for (const char* name : {"CON", "prn", "Aux", "NUL", "com1", "COM9", "lpt1", "LPT9", "nul.txt", "CON .x"})
		CHECK_THAT(f.fails("profile.add", {{"name", name}}), ContainsSubstring("device"));
	for (const char* name : {"CONSOLE", "COM10", "LPT0", "Auxiliary", "my nul"})
		f.call("profile.add", {{"name", name}});
}

namespace
{
	// Whether the file system of the folder folds the case of letters such as Ä, as NTFS and APFS do.
	bool foldsCase(const std::filesystem::path& folder)
	{
		std::filesystem::create_directories(folder / std::filesystem::u8path("\xC3\x84"));
		return std::filesystem::exists(folder / std::filesystem::u8path("\xC3\xA4"));
	}
}

TEST_CASE("profile names that the file system takes for another profile's folder are taken", "[unit]")
{
	const Fixture f;
	const test::TempDir probe;
	const bool folds = foldsCase(probe.path());
	f.call("profile.add",
		{{"name",
			"\xC3\x9C"
			"ber"}});                                                             // Über
	f.call("profile.rename", {{"name", "Default"}, {"newName", "\xC3\x84rger"}}); // Ärger, the active one
	for (const char* name : {"\xC3\xBC"
							 "ber",
			 "\xC3\xA4rger"}) // über, ärger
	{
		const engine::Result added = f.client->call("profile.add", {{"name", name}});
		CHECK(added.ok() == !folds);
		if (!added.ok())
			CHECK_THAT(added.error, ContainsSubstring("there is a profile"));
	}
}

TEST_CASE("a profile switch that fails leaves the settings as they were", "[unit]")
{
	Fixture f;
	f.setScale(1.5);
	f.call("profile.add", {{"name", "Studio"}});
	// A file of the next profile that can't be read: the switch fails before it changes anything.
	std::filesystem::create_directories(f.dir.path() / "profiles" / "Studio" / "settings.json" / "folder");
	f.call("profile.activate", {{"name", "Studio"}});
	f.start();
	CHECK(f.call("profile.list").at("active") == "Default");
	CHECK(f.scale() == 1.5);
	CHECK(std::filesystem::is_directory(f.dir.path() / "profiles" / "Studio" / "settings.json"));
	CHECK(!std::filesystem::exists(f.dir.path() / "profiles" / "Default"));
}

TEST_CASE("a profile switch to a folder that is the active profile's on this file system is refused", "[unit]")
{
	Fixture f;
	const test::TempDir probe;
	if (!foldsCase(probe.path()))
		SKIP("the file system tells the folders apart");
	f.call("profile.rename", {{"name", "Default"}, {"newName", "\xC3\x84rger"}}); // Ärger
	f.setScale(2.0);
	f.call("profile.duplicate", {{"name", "\xC3\x84rger"}, {"newName", "Copy"}});
	f.setScale(1.5);
	// A folder in another case that came from elsewhere, such as a copy from a computer that tells them apart.
	const std::filesystem::path other = f.dir.path() / "profiles" / std::filesystem::u8path("\xC3\xA4rger");
	std::filesystem::rename(f.dir.path() / "profiles" / "Copy", other);
	f.call("profile.activate", {{"name", "\xC3\xA4rger"}});
	f.start();
	CHECK(f.call("profile.list").at("active") == "\xC3\x84rger");
	CHECK(f.scale() == 1.5);
	CHECK(std::filesystem::exists(other / "settings.json"));
}
