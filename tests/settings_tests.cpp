// SPDX-License-Identifier: GPL-3.0-or-later
// User settings (#51): where they live, their files with defaults, validation, atomic writes, backups and migrations,
// and the settings.* commands.
#include "engine/engine.h"
#include "engine/settings/store.h"
#include "engine/settings/usersettings.h"
#include "mcp/server.h"
#include "platform/files.h"
#include "platform/rt/realtime.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Json;
using gnubase::test::readText;
using gnubase::test::TempDir;
namespace files = gnubase::files;
namespace settings = gnubase::engine::settings;
namespace fs = std::filesystem;

namespace
{
	files::Environment environment(std::map<std::string, std::string> variables)
	{
		return [variables = std::make_shared<const std::map<std::string, std::string>>(std::move(variables))](
				   const char* name) -> std::optional<fs::path>
		{
			if (const auto found = variables->find(name); found != variables->end())
				return fs::u8path(found->second);
			return std::nullopt;
		};
	}

	void writeText(const fs::path& file, const std::string& text)
	{
		std::ofstream(file, std::ios::binary) << text;
	}

	Json readJson(const fs::path& file)
	{
		return Json::parse(readText(file));
	}

	// A settings file at version 3 that went through two changes: version 2 renamed "volume" to "level", and
	// version 3 turned the "size" string "WIDTHxHEIGHT" into the numbers "width" and "height".
	settings::Format windowFormat()
	{
		settings::Format format;
		format.name = "window";
		format.version = 3;
		format.definitions = {
			{"level", "Level from 0 to 1.", Json::parse(R"({"type": "number", "minimum": 0, "maximum": 1})"), 0.5},
			{"width", "Width in dp.", Json::parse(R"({"type": "integer", "minimum": 100})"), 800},
			{"height", "Height in dp.", Json::parse(R"({"type": "integer", "minimum": 100})"), 600},
			{"title", "Window title.", Json::parse(R"({"type": "string"})"), "GNUbase"},
			{"mode", "Display mode.", Json::parse(R"({"enum": ["window", "fullscreen"]})"), "window"},
			{"remember", "Remember the position.", Json::parse(R"({"type": "boolean"})"), true},
		};
		format.migrations = {
			{1,
				[](Json& values)
				{
					if (values.contains("volume"))
					{
						values["level"] = values["volume"];
						values.erase("volume");
					}
				}},
			{2,
				[](Json& values)
				{
					if (!values.contains("size"))
						return;
					const std::string size = values["size"].get<std::string>();
					const size_t x = size.find('x');
					if (x == std::string::npos)
						throw std::runtime_error("size " + size + " has no x");
					values["width"] = std::stoll(size.substr(0, x));
					values["height"] = std::stoll(size.substr(x + 1));
					values.erase("size");
				}},
		};
		return format;
	}

	// The client's next message, which must be an event.
	gnubase::engine::Event nextEvent(gnubase::engine::Client& client)
	{
		const std::optional<gnubase::engine::Message> message = client.poll();
		const auto* event = message.has_value() ? std::get_if<gnubase::engine::Event>(&*message) : nullptr;
		REQUIRE(event != nullptr);
		return *event;
	}

	std::atomic<int> g_violations{0};

	void countViolation(const char* /*what*/) noexcept
	{
		++g_violations;
	}
}

TEST_CASE("the settings folder is per user and per OS, and GNUBASE_SETTINGS_DIR overrides it", "[unit]")
{
	using files::Os;
	const files::UserFolder noUserFolder;
	const auto settingsDirectory = [&noUserFolder](Os os, const files::Environment& variables)
	{ return files::settingsDirectory(os, variables, noUserFolder); };

	SECTION("Windows keeps them in the roaming application data")
	{
		CHECK(settingsDirectory(Os::Windows, environment({{"APPDATA", "C:\\Users\\ann\\AppData\\Roaming"}})) ==
			fs::path("C:\\Users\\ann\\AppData\\Roaming") / "GNUbase");
		CHECK(settingsDirectory(Os::Windows, environment({{"USERPROFILE", "C:\\Users\\ann"}})) ==
			fs::path("C:\\Users\\ann") / "AppData" / "Roaming" / "GNUbase");
	}
	SECTION("macOS keeps them in the user's Application Support folder")
	{
		CHECK(settingsDirectory(Os::MacOs, environment({{"HOME", "/Users/ann"}})) ==
			fs::path("/Users/ann") / "Library" / "Application Support" / "GNUbase");
	}
	SECTION("Linux follows the XDG base directories")
	{
		CHECK(
			settingsDirectory(Os::Linux, environment({{"XDG_CONFIG_HOME", "/home/ann/conf"}, {"HOME", "/home/ann"}})) ==
			fs::path("/home/ann/conf") / "gnubase");
		CHECK(settingsDirectory(Os::Linux, environment({{"HOME", "/home/ann"}})) ==
			fs::path("/home/ann") / ".config" / "gnubase");
		// The XDG specification ignores empty and relative paths.
		CHECK(settingsDirectory(Os::Linux, environment({{"XDG_CONFIG_HOME", ""}, {"HOME", "/home/ann"}})) ==
			fs::path("/home/ann") / ".config" / "gnubase");
		CHECK(settingsDirectory(Os::Linux, environment({{"XDG_CONFIG_HOME", "conf"}, {"HOME", "/home/ann"}})) ==
			fs::path("/home/ann") / ".config" / "gnubase");
	}
	SECTION("GNUBASE_SETTINGS_DIR overrides the folder on every OS, for tests and portable installs")
	{
		for (const Os os : {Os::Windows, Os::MacOs, Os::Linux})
		{
			CHECK(settingsDirectory(os,
					  environment({{"GNUBASE_SETTINGS_DIR", "portable/settings"}, {"APPDATA", "C:\\Roaming"},
						  {"HOME", "/home/ann"}, {"XDG_CONFIG_HOME", "/home/ann/conf"}})) ==
				fs::path("portable/settings"));
			CHECK(settingsDirectory(os,
					  environment({{"GNUBASE_SETTINGS_DIR", ""}, {"HOME", "/home/ann"}, {"APPDATA", "C:\\Roaming"}})) !=
				fs::path()); // empty means not set
		}
	}
	SECTION("without any of the variables, the folder the OS has for the user")
	{
		const files::UserFolder roaming = [] { return std::optional<fs::path>(R"(C:\Users\ann\AppData\Roaming)"); };
		CHECK(files::settingsDirectory(Os::Windows, environment({}), roaming) ==
			fs::path(R"(C:\Users\ann\AppData\Roaming)") / "GNUbase");
		const files::UserFolder home = [] { return std::optional<fs::path>("/home/ann"); };
		CHECK(files::settingsDirectory(Os::MacOs, environment({}), home) ==
			fs::path("/home/ann") / "Library" / "Application Support" / "GNUbase");
		CHECK(files::settingsDirectory(Os::Linux, environment({}), home) ==
			fs::path("/home/ann") / ".config" / "gnubase");
		CHECK(files::settingsDirectory(Os::Linux, environment({{"HOME", "/home/bob"}}), home) ==
			fs::path("/home/bob") / ".config" / "gnubase"); // the variable comes first
	}
	SECTION("without any folder for the user, none: the settings stay in memory, never in a shared folder")
	{
		const files::UserFolder none = [] { return std::optional<fs::path>(); };
		for (const Os os : {Os::Windows, Os::MacOs, Os::Linux})
		{
			CHECK(settingsDirectory(os, environment({})).empty());
			CHECK(files::settingsDirectory(os, environment({}), none).empty());
		}
	}
	SECTION("this process uses its own OS and environment")
	{
		CHECK_FALSE(files::settingsDirectory().empty());
		CHECK(files::osUserFolder().has_value());
#ifdef _WIN32
		CHECK(files::currentOs() == Os::Windows);
#elif defined(__APPLE__)
		CHECK(files::currentOs() == Os::MacOs);
#else
		CHECK(files::currentOs() == Os::Linux);
#endif
	}
}

TEST_CASE("files are written atomically: a failed write leaves the old file as it was", "[unit]")
{
	const TempDir dir;
	const fs::path file = dir.path() / "new folder" / "settings.json";
	files::writeAtomically(file, "first");
	CHECK(readText(file) == "first");
	files::writeAtomically(file, "second"); // replaces the file
	CHECK(readText(file) == "second");
	CHECK(std::distance(fs::directory_iterator(file.parent_path()), fs::directory_iterator()) == 1); // no temporary

	// A folder in the file's place can't be replaced: the write fails before anything changes.
	const fs::path blocked = dir.path() / "blocked.json";
	fs::create_directories(blocked);
	CHECK_THROWS(files::writeAtomically(blocked, "data"));
	CHECK(fs::is_directory(blocked));
	CHECK(std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator()) == 2); // no temporary files

	CHECK(files::readFile(file) == std::optional<std::string>("second"));
	CHECK_FALSE(files::readFile(dir.path() / "missing.json").has_value());
}

TEST_CASE("an atomic write through a symbolic link replaces the file it points to and keeps the link", "[unit]")
{
	const TempDir dir;
	const fs::path real = dir.path() / "dotfiles" / "settings.json";
	const fs::path link = dir.path() / "settings.json";
	files::writeAtomically(real, "first");
	std::error_code error;
	fs::create_symlink(real, link, error);
	if (error)
		SKIP("this account can't make symbolic links: " + error.message()); // Windows without developer mode
	files::writeAtomically(link, "second");
	CHECK(fs::is_symlink(link));
	CHECK(readText(real) == "second");
	CHECK(readText(link) == "second");
	CHECK(std::distance(fs::directory_iterator(real.parent_path()), fs::directory_iterator()) == 1); // no temporary
}

TEST_CASE("temporary files that a crash left behind are removed, and only those", "[unit]")
{
	const TempDir dir;
	const fs::path file = dir.path() / "settings.json";
	files::writeAtomically(file, "data");
	const fs::path stale = dir.path() / "settings.json.0123456789abcdef.tmp"; // the name writeAtomically uses
	const std::vector<fs::path> others = {dir.path() / "settings.json.notes.tmp",
		dir.path() / "settings.json.0123456789abcdeg.tmp", dir.path() / "other.json.0123456789abcdef.tmp"};
	writeText(stale, "dat");
	for (const fs::path& other : others)
		writeText(other, "mine");
	files::removeStaleTemporaries(file);
	CHECK_FALSE(fs::exists(stale));
	for (const fs::path& other : others)
		CHECK(fs::exists(other));
	CHECK(readText(file) == "data");
}

TEST_CASE("a file lock excludes other holders until it is released", "[unit]")
{
	const TempDir dir;
	const fs::path lockFile = dir.path() / "new folder" / "settings.lock";
	{
		const files::FileLock lock(lockFile, std::chrono::milliseconds(0));
		CHECK(fs::exists(lockFile));
		CHECK_THROWS_AS(files::FileLock(lockFile, std::chrono::milliseconds(50)), std::system_error);
	}
	const files::FileLock again(lockFile, std::chrono::milliseconds(0)); // released with the first one
	CHECK(fs::exists(lockFile));
}

TEST_CASE("settings start with their defaults and the file keeps only what differs from them", "[unit]")
{
	const TempDir dir;
	settings::Store store(windowFormat(), dir.path());
	CHECK(store.file() == dir.path() / "window.json");
	CHECK(store.loadReport().status == settings::LoadStatus::Missing);
	CHECK(store.value("level") == 0.5);
	CHECK(store.value("title") == "GNUbase");
	CHECK(store.values() ==
		Json::parse(R"({"level": 0.5, "width": 800, "height": 600, "title": "GNUbase", "mode": "window",
			"remember": true})"));
	CHECK_FALSE(fs::exists(store.file())); // nothing written until something changes

	CHECK(store.set("width", 1024));
	CHECK_FALSE(store.set("width", 1024)); // unchanged
	CHECK(readJson(store.file()) == Json::parse(R"({"version": 3, "values": {"width": 1024}})"));

	const settings::Store reloaded(windowFormat(), dir.path());
	CHECK(reloaded.loadReport().status == settings::LoadStatus::Loaded);
	CHECK(reloaded.loadReport().problems.empty());
	CHECK(reloaded.value("width") == 1024);
	CHECK(reloaded.value("height") == 600);

	settings::Store memory(windowFormat()); // no folder: kept in memory, never written
	CHECK(memory.loadReport().status == settings::LoadStatus::InMemory);
	CHECK(memory.file().empty());
	CHECK(memory.set("width", 1024));
	CHECK(memory.value("width") == 1024);
}

TEST_CASE("settings are typed and checked against their schema", "[unit]")
{
	const TempDir dir;
	settings::Store store(windowFormat(), dir.path());
	constexpr settings::Key<int64_t> width{"width"};
	constexpr settings::Key<double> level{"level"};
	constexpr settings::Key<std::string> title{"title"};
	constexpr settings::Key<bool> remember{"remember"};

	CHECK(store.get(width) == 800);
	CHECK(store.set(width, int64_t{1280}));
	CHECK(store.get(width) == 1280);
	CHECK(store.set(level, 0.75));
	CHECK(store.get(level) == 0.75);
	CHECK(store.set(title, std::string("Mixer")));
	CHECK(store.get(title) == "Mixer");
	CHECK(store.set(remember, false));
	CHECK_FALSE(store.get(remember));

	CHECK_THROWS_WITH(store.set("width", 50), "width must be at least 100");
	CHECK_THROWS_WITH(store.set("width", 800.5), "width must be integer");
	CHECK_THROWS_WITH(store.set("level", "loud"), "level must be number");
	CHECK_THROWS_WITH(store.set("mode", "tiny"), R"(mode must be one of ["window","fullscreen"])");
	CHECK_THROWS_WITH(store.set("depth", 3), "there is no setting depth");
	CHECK_THROWS_WITH(store.value("depth"), "there is no setting depth");
	CHECK_THROWS_AS(store.set("width", 50), settings::SettingsError);
	CHECK(store.get(width) == 1280); // a rejected value changes nothing
	CHECK(readJson(store.file()).at("values").at("width") == 1280);

	// A format whose defaults don't fit their own schemas, or whose keys repeat, is a programming error.
	settings::Format broken = windowFormat();
	broken.definitions[0].defaultValue = 2.0;
	CHECK_THROWS_AS(settings::Store(broken), std::logic_error);
	broken = windowFormat();
	broken.definitions.push_back(broken.definitions[0]);
	CHECK_THROWS_AS(settings::Store(broken), std::logic_error);
}

TEST_CASE("reset puts one setting or all of them back to their defaults", "[unit]")
{
	const TempDir dir;
	settings::Store store(windowFormat(), dir.path());
	store.set("width", 1024);
	store.set("title", "Mixer");
	store.set("remember", false);

	CHECK(store.reset("width"));
	CHECK_FALSE(store.reset("width")); // already the default
	CHECK(store.value("width") == 800);
	CHECK(readJson(store.file()).at("values") == Json::parse(R"({"title": "Mixer", "remember": false})"));

	CHECK(store.reset() == std::vector<std::string>{"remember", "title"}); // the ones that changed, sorted
	CHECK(store.values() == settings::Store(windowFormat()).values());
	CHECK(readJson(store.file()) == Json::parse(R"({"version": 3, "values": {}})"));
	CHECK_THROWS_WITH(store.reset("depth"), "there is no setting depth");
}

TEST_CASE("a corrupt or unreadable settings file falls back to the defaults and is kept as a backup", "[unit]")
{
	const TempDir dir;
	const fs::path file = dir.path() / "window.json";
	const fs::path backup = dir.path() / "window.corrupt.json";
	const auto load = [&dir] { return settings::Store(windowFormat(), dir.path()); };

	SECTION("not JSON at all")
	{
		const std::string text = R"({"version": 3, "values": {"width": 10)";
		writeText(file, text); // cut off, as by a crash
		const settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Reset);
		CHECK_THAT(store.loadReport().problems.at(0), ContainsSubstring("not valid JSON"));
		CHECK(store.loadReport().backup == backup);
		CHECK(store.values() == settings::Store(windowFormat()).values());
		CHECK(readText(backup) == text); // kept as it was
		CHECK_FALSE(fs::exists(file));
		CHECK(load().loadReport().status == settings::LoadStatus::Missing); // reported once, not on every start
	}
	SECTION("JSON, but not a settings file")
	{
		for (const char* text : {"[1, 2]", R"({"values": {}})", R"({"version": "3", "values": {}})",
				 R"({"version": 3, "values": [1]})", R"({"version": 0, "values": {}})"})
		{
			CAPTURE(text);
			writeText(file, text);
			const settings::Store store = load();
			CHECK(store.loadReport().status == settings::LoadStatus::Reset);
			CHECK(readText(backup) == text);
			CHECK(store.value("width") == 800);
		}
	}
	SECTION("unreadable")
	{
		fs::create_directories(file); // a folder can't be read as a file
		const settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Reset);
		CHECK_THAT(store.loadReport().problems.at(0), ContainsSubstring("cannot read"));
		CHECK(fs::is_directory(backup));
		CHECK(store.value("width") == 800);
	}
	SECTION("single values that are invalid or unknown fall back to their defaults and stay in the file")
	{
		const std::string text =
			R"({"version": 3, "values": {"width": 1024, "height": 5, "title": 7, "colour": "red"}})";
		writeText(file, text);
		settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Loaded);
		REQUIRE(store.loadReport().problems.size() == 3);
		CHECK_THAT(store.loadReport().problems[0], ContainsSubstring("colour; left in the file"));
		CHECK_THAT(store.loadReport().problems[1], ContainsSubstring("height must be at least 100"));
		CHECK_THAT(store.loadReport().problems[2], ContainsSubstring("title must be string"));
		CHECK(store.value("width") == 1024);
		CHECK(store.value("height") == 600);
		CHECK(store.value("title") == "GNUbase");
		CHECK(readText(file) == text); // nothing was discarded, so nothing was written
		CHECK(store.loadReport().backup.empty());

		// A change writes only its own setting; the values this GNUbase doesn't take stay for the one that wrote them.
		CHECK(store.set("width", 900));
		CHECK(
			readJson(file).at("values") == Json::parse(R"({"width": 900, "height": 5, "title": 7, "colour": "red"})"));
		CHECK_FALSE(store.set("height", 600)); // the value it has, but the user chose it: the invalid one goes
		CHECK(readJson(file).at("values") == Json::parse(R"({"width": 900, "title": 7, "colour": "red"})"));
		CHECK(store.reset() == std::vector<std::string>{"width"});
		CHECK(readJson(file).at("values") == Json::parse(R"({"colour": "red"})")); // unknown: not this one's to reset
	}
	SECTION("a stale temporary file from a crash during a write is removed")
	{
		writeText(file, R"({"version": 3, "values": {"width": 1024}})");
		const fs::path stale = dir.path() / "window.json.00c0ffee00c0ffee.tmp";
		writeText(stale, R"({"version": 3, "val)");
		settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Loaded);
		CHECK(store.value("width") == 1024);
		CHECK_FALSE(fs::exists(stale));
		CHECK(store.set("width", 900));
		CHECK(readJson(file).at("values").at("width") == 900);
	}
	SECTION("a corrupt file that can't be moved away is left as it is, and nothing overwrites it")
	{
		const std::string text = "{ not JSON";
		writeText(file, text);
		fs::create_directories(backup);
		writeText(backup / "keep", "a folder in the backup's place, so that the move fails");
		settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Reset);
		CHECK(store.loadReport().readOnly);
		CHECK_THAT(store.loadReport().problems.back(), ContainsSubstring("aren't saved"));
		CHECK(store.set("width", 1024)); // applies until GNUbase quits
		CHECK(store.value("width") == 1024);
		CHECK(readText(file) == text);
	}
}

// The acceptance test of #51: settings files of older versions migrate to the current one.
TEST_CASE("older settings files migrate to the current version through the chain of migrations", "[unit]")
{
	const TempDir dir;
	const fs::path file = dir.path() / "window.json";
	const auto load = [&dir] { return settings::Store(windowFormat(), dir.path()); };

	SECTION("from version 1 through 2 to 3")
	{
		const std::string text = R"({"version": 1, "values": {"volume": 0.8, "size": "1024x768", "title": "Mix"}})";
		writeText(file, text);
		const settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Migrated);
		CHECK(store.loadReport().fileVersion == 1);
		CHECK(store.loadReport().problems.empty());
		CHECK(store.value("level") == 0.8);
		CHECK(store.value("width") == 1024);
		CHECK(store.value("height") == 768);
		CHECK(store.value("title") == "Mix");
		// The file is now at version 3; the version 1 file stays next to it, for going back to an older GNUbase.
		CHECK(readJson(file) ==
			Json::parse(R"({"version": 3, "values": {"level": 0.8, "width": 1024, "height": 768, "title": "Mix"}})"));
		CHECK(store.loadReport().backup == dir.path() / "window.v1.json");
		CHECK(readText(dir.path() / "window.v1.json") == text);
		CHECK(load().loadReport().status == settings::LoadStatus::Loaded); // migrated once
	}
	SECTION("from version 2, only the last migration runs")
	{
		writeText(file, R"({"version": 2, "values": {"level": 0.25, "size": "640x480", "volume": 1}})");
		const settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Migrated);
		CHECK(store.loadReport().fileVersion == 2);
		CHECK(store.value("level") == 0.25);
		CHECK(store.value("width") == 640);
		// "volume" is no setting of version 2, so the first migration didn't rename it: it is dropped.
		REQUIRE(store.loadReport().problems.size() == 1);
		CHECK_THAT(store.loadReport().problems[0], ContainsSubstring("volume"));
	}
	SECTION("values that are invalid after migrating fall back to their defaults")
	{
		writeText(file, R"({"version": 1, "values": {"volume": 3, "size": "50x700"}})");
		const settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Migrated);
		CHECK(store.loadReport().problems.size() == 2);
		CHECK(store.value("level") == 0.5);
		CHECK(store.value("width") == 800);
		CHECK(store.value("height") == 700);
	}
	SECTION("a file that can't be kept as a backup isn't migrated on disk: the store is read-only")
	{
		const std::string text = R"({"version": 1, "values": {"volume": 0.8}})";
		writeText(file, text);
		fs::create_directories(dir.path() / "window.v1.json" / "folder"); // the copy fails
		settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Migrated);
		CHECK(store.loadReport().readOnly);
		CHECK_THAT(store.loadReport().problems.back(), ContainsSubstring("cannot keep a backup"));
		CHECK(store.value("level") == 0.8); // migrated in memory
		CHECK(store.set("width", 1024));
		CHECK(readText(file) == text); // the only copy of the original
	}
	SECTION("a migration that fails resets the file and keeps it as a backup")
	{
		const std::string text = R"({"version": 1, "values": {"size": "large", "title": "Mix"}})";
		writeText(file, text);
		const settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Reset);
		CHECK_THAT(store.loadReport().problems.at(0), ContainsSubstring("size large has no x"));
		CHECK(store.value("title") == "GNUbase");
		CHECK(readText(dir.path() / "window.v1.json") == text);
		CHECK_FALSE(fs::exists(file));
	}
	SECTION("a file older than the oldest migration is reset and kept")
	{
		settings::Format format = windowFormat();
		format.migrations.erase(format.migrations.begin()); // migrates only from version 2
		writeText(file, R"({"version": 1, "values": {"title": "Mix"}})");
		const settings::Store store(format, dir.path());
		CHECK(store.loadReport().status == settings::LoadStatus::Reset);
		CHECK_THAT(store.loadReport().problems.at(0), ContainsSubstring("too old"));
		CHECK(store.value("title") == "GNUbase");
		CHECK(fs::exists(dir.path() / "window.v1.json"));
	}
	SECTION("a file of a newer GNUbase is left where it is, and nothing is saved over it")
	{
		const std::string text = R"({"version": 4, "values": {"title": "Mix"}})";
		writeText(file, text);
		settings::Store store = load();
		CHECK(store.loadReport().status == settings::LoadStatus::Newer);
		CHECK(store.loadReport().fileVersion == 4);
		CHECK(store.loadReport().readOnly);
		CHECK_THAT(store.loadReport().problems.at(0), ContainsSubstring("newer"));
		CHECK(store.value("title") == "GNUbase");
		CHECK(store.set("title", "Older")); // applies until GNUbase quits, but isn't saved
		CHECK(store.value("title") == "Older");
		CHECK(store.reset() == std::vector<std::string>{"title"});
		CHECK(readText(file) == text);
		CHECK_FALSE(fs::exists(dir.path() / "window.v4.json"));
		// Started again, the older GNUbase still finds the newer file, and so does the newer one.
		CHECK(load().loadReport().status == settings::LoadStatus::Newer);
		settings::Format newer = windowFormat();
		newer.version = 4;
		newer.migrations.push_back({3, [](Json&) {}});
		CHECK(settings::Store(newer, dir.path()).value("title") == "Mix");
	}
	SECTION("the chain must lead from its first version to the current one without gaps")
	{
		settings::Format gap = windowFormat();
		gap.migrations.erase(gap.migrations.begin() + 1);
		CHECK_THROWS_AS(settings::Store(gap), std::logic_error);
		settings::Format unordered = windowFormat();
		std::swap(unordered.migrations[0], unordered.migrations[1]);
		CHECK_THROWS_AS(settings::Store(unordered), std::logic_error);
	}
}

TEST_CASE("an older GNUbase keeps the settings and values that only a newer one knows", "[unit]")
{
	const TempDir dir;
	// The newer release added a setting and a display mode at the same version: no migration needed.
	settings::Format newer = windowFormat();
	newer.definitions.push_back({"dock", "Dock the window.", Json::parse(R"({"type": "boolean"})"), false});
	newer.definitions[4].schema = Json::parse(R"({"enum": ["window", "fullscreen", "kiosk"]})");
	{
		settings::Store store(newer, dir.path());
		store.set("dock", true);
		store.set("mode", "kiosk");
	}
	{
		settings::Store older(windowFormat(), dir.path());
		CHECK(older.loadReport().status == settings::LoadStatus::Loaded);
		CHECK(older.value("mode") == "window"); // a value it doesn't take: the default
		CHECK(older.loadReport().problems.size() == 2);
		CHECK(older.set("width", 1024));
		CHECK(older.set("title", "Mix"));
		CHECK(older.reset("title"));
	}
	const settings::Store store(newer, dir.path());
	CHECK(store.loadReport().problems.empty());
	CHECK(store.value("dock") == true);
	CHECK(store.value("mode") == "kiosk");
	CHECK(store.value("width") == 1024);
}

TEST_CASE("several GNUbase processes share the settings file without losing each other's changes", "[unit]")
{
	const TempDir dir;
	settings::Store first(windowFormat(), dir.path());
	settings::Store second(windowFormat(), dir.path()); // started before the first one changed anything
	CHECK(first.set("width", 1024));
	CHECK(second.set("height", 700));
	CHECK(second.value("width") == 800); // each one uses what it read until it starts again
	CHECK(readJson(dir.path() / "window.json").at("values") == Json::parse(R"({"width": 1024, "height": 700})"));
	CHECK(first.reset("width"));
	CHECK(readJson(dir.path() / "window.json").at("values") == Json::parse(R"({"height": 700})"));

	// A change waits for the lock of another process, and fails if it doesn't get it.
	{
		const files::FileLock other(dir.path() / "window.lock", std::chrono::milliseconds(0));
		first.setLockTimeout(std::chrono::milliseconds(50));
		CHECK_THROWS_AS(first.set("title", "Mix"), settings::SettingsError);
		CHECK(first.value("title") == "GNUbase"); // nothing changed
	}
	CHECK(first.set("title", "Mix"));
	CHECK(settings::Store(windowFormat(), dir.path()).values().at("title") == "Mix");
}

// Every version of the user settings file that a GNUbase release wrote is in tests/data/settings, as
// settings.v<version>.json. When the version goes up, add the new file: older ones must keep migrating.
TEST_CASE("every earlier version of the user settings file still loads", "[unit]")
{
	const fs::path fixtures = fs::path(GNUBASE_TEST_DATA_DIR) / "settings";
	int found = 0;
	for (const fs::directory_entry& entry : fs::directory_iterator(fixtures))
	{
		CAPTURE(entry.path().filename().string());
		const TempDir dir;
		fs::copy_file(entry.path(), dir.path() / "settings.json");
		const settings::Store store(settings::userFormat(), dir.path());
		CHECK((store.loadReport().status == settings::LoadStatus::Loaded ||
			store.loadReport().status == settings::LoadStatus::Migrated));
		CHECK(store.loadReport().problems.empty());
		CHECK(store.values() != settings::Store(settings::userFormat()).values()); // the fixture changes something
		++found;
	}
	CHECK(found == settings::userFormat().version);
}

TEST_CASE("the user settings have typed keys with valid defaults", "[unit]")
{
	const settings::Store store(settings::userFormat());
	CHECK(store.file().empty());
	CHECK(store.get(settings::g_uiScale) == 1.0);
	CHECK(store.get(settings::g_uiTheme) == "gnubase");
	CHECK(store.get(settings::g_audioBackend).empty());
	CHECK(store.get(settings::g_audioDevice).empty());
	CHECK(store.get(settings::g_audioSampleRate) == 48000.0);
	CHECK(store.get(settings::g_audioBlockSize) == 256);
	for (const settings::Definition& definition : settings::userFormat().definitions)
	{
		CAPTURE(definition.key);
		CHECK(definition.description.size() > 10);
	}
}

TEST_CASE("settings are never read or written on an audio thread", "[unit]")
{
	settings::Store store(windowFormat());
	const gnubase::rt::ViolationHandler previous = gnubase::rt::setViolationHandler(&countViolation);
	g_violations = 0;
	{
		const gnubase::rt::ScopedRealtimeContext realtime;
		(void)store.value("width");
	}
	const int reading = g_violations.exchange(0);
	{
		const gnubase::rt::ScopedRealtimeContext realtime;
		(void)store.reset("width");
	}
	const int writing = g_violations.exchange(0);
	gnubase::rt::setViolationHandler(previous);
	CHECK(reading > 0);
	CHECK(writing > 0);
}

TEST_CASE("settings.get, settings.set and settings.reset change the user settings file", "[integration]")
{
	using gnubase::engine::Client;
	using gnubase::engine::Engine;
	using gnubase::engine::Event;
	const TempDir dir;
	const fs::path file = dir.path() / "settings.json";
	{
		Engine engine(gnubase::audio::makeBackends(), dir.path());
		Client client(engine);
		CHECK(client.call("settings.get", {{"key", "ui.scale"}}).value == Json::parse(R"({"key": "ui.scale",
			"value": 1.0})"));

		const gnubase::engine::Result set = client.call("settings.set", {{"key", "ui.scale"}, {"value", 1.5}});
		REQUIRE(set.ok());
		CHECK(set.value == Json::parse(R"({"key": "ui.scale", "value": 1.5})"));
		const Event changed = nextEvent(client);
		CHECK(changed.name == "settings.changed");
		CHECK(changed.data == Json::parse(R"({"values": {"ui.scale": 1.5}})"));
		CHECK(readJson(file) == Json::parse(R"({"version": 1, "values": {"ui.scale": 1.5}})"));

		const gnubase::engine::Result invalid = client.call("settings.set", {{"key", "ui.scale"}, {"value", 40}});
		CHECK(invalid.error == "ui.scale must be at most 4");
		// The keys are an enum in the schemas, so MCP clients see every setting.
		CHECK_THAT(client.call("settings.set", {{"key", "ui.zoom"}, {"value", 1}}).error,
			ContainsSubstring("parameters.key must be one of"));
		CHECK_THAT(client.call("settings.get", {{"key", "ui.zoom"}}).error, ContainsSubstring("\"ui.scale\""));
		CHECK(client.call("settings.set", {{"key", "audio.blockSize"}, {"value", 512}}).ok());
		while (client.poll().has_value())
			continue; // the events so far

		const Json list = client.call("settings.list").value;
		CHECK(list.at("file") == file.u8string());
		CHECK(list.at("version") == 1);
		CHECK(list.at("load").at("status") == "missing");
		CHECK(list.at("load").at("readOnly") == false);
		const Json& entries = list.at("settings");
		REQUIRE(entries.size() == settings::userFormat().definitions.size());
		const auto scale = std::find_if(
			entries.begin(), entries.end(), [](const Json& entry) { return entry.at("key") == "ui.scale"; });
		REQUIRE(scale != entries.end());
		CHECK(scale->at("value") == 1.5);
		CHECK(scale->at("default") == 1.0);
		CHECK(scale->at("schema").at("maximum") == 4);
		CHECK(scale->at("description").get<std::string>().size() > 10);
	}
	{
		Engine engine(gnubase::audio::makeBackends(), dir.path()); // the next start reads them back
		Client client(engine);
		CHECK(client.call("settings.get", {{"key", "ui.scale"}}).value.at("value") == 1.5);
		CHECK(client.call("settings.list").value.at("load").at("status") == "loaded");

		const gnubase::engine::Result one = client.call("settings.reset", {{"key", "ui.scale"}});
		CHECK(one.value == Json::parse(R"({"values": {"ui.scale": 1.0}})"));
		CHECK(nextEvent(client).data == Json::parse(R"({"values": {"ui.scale": 1.0}})"));
		const gnubase::engine::Result all = client.call("settings.reset");
		CHECK(all.value.at("values") == settings::Store(settings::userFormat()).values());
		CHECK(nextEvent(client).data == Json::parse(R"({"values": {"audio.blockSize": 256}})"));
		CHECK_FALSE(client.poll().has_value()); // nothing else changed
		CHECK(readJson(file).at("values") == Json::object());
	}
}

TEST_CASE("an engine without a settings folder keeps its settings in memory", "[unit]")
{
	gnubase::engine::Engine engine;
	gnubase::engine::Client client(engine);
	CHECK(client.call("settings.set", {{"key", "ui.theme"}, {"value", "dark"}}).ok());
	CHECK(client.call("settings.get", {{"key", "ui.theme"}}).value.at("value") == "dark");
	const Json list = client.call("settings.list").value;
	CHECK(list.at("file").is_null());
	CHECK(list.at("load").at("status") == "inMemory");
}

TEST_CASE("MCP clients read and change settings with the settings tools", "[integration]")
{
	const TempDir dir;
	gnubase::engine::Engine engine(gnubase::audio::makeBackends(), dir.path());
	gnubase::mcp::Server server(engine);
	const auto call = [&server](const std::string& tool, const Json& arguments)
	{
		return server
			.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
				{"params", {{"name", tool}, {"arguments", arguments}}}})
			.value_or(Json())
			.at("result");
	};
	const Json set = call("settings_set", {{"key", "audio.sampleRate"}, {"value", 96000}});
	CHECK(set.at("isError") == false);
	CHECK(set.at("structuredContent") == Json::parse(R"({"key": "audio.sampleRate", "value": 96000})"));
	CHECK(call("settings_get", {{"key", "audio.sampleRate"}}).at("structuredContent").at("value") == 96000);
	CHECK(call("settings_set", {{"key", "audio.sampleRate"}, {"value", "fast"}}).at("isError") == true);
	CHECK(call("settings_reset", Json::object()).at("isError") == false);
	CHECK(readJson(dir.path() / "settings.json").at("values") == Json::object());
}
