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

		double scale() { return call("settings.get", {{"key", "ui.scale"}}).at("value").get<double>(); }
		void setScale(double scale) { call("settings.set", {{"key", "ui.scale"}, {"value", scale}}); }

		std::vector<std::string> names()
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
	Fixture f;
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
