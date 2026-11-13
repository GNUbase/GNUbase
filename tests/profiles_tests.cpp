// SPDX-License-Identifier: GPL-3.0-or-later
// Profiles (#163, docs/features/profiles.md): named sets of the user settings in the settings folder, added,
// duplicated, renamed, deleted, exported and imported, and activated when GNUbase starts again.
#include "engine/engine.h"
#include "engine/settings/profiles.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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
		Json{{"profiles", {{{"name", "Default"}, {"active", true}}}}, {"active", "Default"}, {"next", ""},
			{"unfinished", false}});
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
	for (const char* name : {"CON", "prn", "Aux", "NUL", "com1", "COM9", "lpt1", "LPT9", "nul.txt", "CON .x",
			 "COM\xC2\xB9", "lpt\xC2\xB2", "LPT\xC2\xB3.x", "CONIN$", "conout$.txt"}) // COM¹, lpt², LPT³
		CHECK_THAT(f.fails("profile.add", {{"name", name}}), ContainsSubstring("device"));
	for (const char* name : {"CONSOLE", "COM10", "LPT0", "Auxiliary", "my nul", "COM\xE2\x81\xB4", "CONIN"}) // COM⁴
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

TEST_CASE("a profile switch that can't save the active profile's files leaves them as they were", "[unit]")
{
	Fixture f;
	f.setScale(1.5);
	f.call("profile.add", {{"name", "Studio"}});
	// The active profile's folder is a file: its files can't go there, after the switch copied both profiles' files.
	std::ofstream(f.dir.path() / "profiles" / "Default").close();
	f.call("profile.activate", {{"name", "Studio"}});
	f.start();
	CHECK(f.call("profile.list").at("active") == "Default");
	CHECK(f.scale() == 1.5);
	CHECK(!std::filesystem::exists(f.dir.path() / "profile-switch"));
	CHECK(std::filesystem::is_directory(f.dir.path() / "profiles" / "Studio"));
}

namespace
{
	// Stops the engine with the profile next chosen, then switches with replacing, which may make a step fail.
	void switchWith(
		Fixture& f, const std::string& next, const std::function<void(const std::filesystem::path& file)>& replacing)
	{
		f.call("profile.activate", {{"name", next}});
		f.client.reset();
		f.engine.reset();
		engine::settings::switchProfile(f.dir.path(), replacing);
	}

	std::string contents(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}
}

TEST_CASE("a profile switch that fails after it replaced files in the settings folder puts them back", "[unit]")
{
	Fixture f;
	f.setScale(2.0);
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}});
	f.setScale(1.5);
	const std::filesystem::path dir = f.dir.path();
	std::string before;
	bool failed = false;
	// The second file can't be replaced once Studio's settings.json is in the settings folder.
	switchWith(f, "Studio",
		[&](const std::filesystem::path& file)
		{
			if (file == dir / "settings.json" && before.empty())
			{
				before = contents(file);
			}
			else if (file == dir / "quantize-presets.json" && !failed)
			{
				failed = true;
				CHECK(contents(dir / "settings.json") == contents(dir / "profiles" / "Studio" / "settings.json"));
				throw std::runtime_error("can't be replaced");
			}
		});
	CHECK(failed);
	CHECK(contents(dir / "settings.json") == before);
	CHECK(!std::filesystem::exists(dir / "profile-switch"));
	CHECK(!std::filesystem::exists(dir / "profiles" / "Default"));
	CHECK(std::filesystem::exists(dir / "profiles" / "Studio" / "settings.json"));
	// The switch waits for the next start.
	f.start();
	CHECK(f.call("profile.list").at("active") == "Studio");
	CHECK(f.scale() == 2.0);
}

TEST_CASE("a profile switch that fails and can't put the active profile's files back keeps its copies", "[unit]")
{
	Fixture f;
	f.setScale(2.0);
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}});
	f.setScale(1.5);
	const std::filesystem::path dir = f.dir.path();
	switchWith(f, "Studio",
		[&dir](const std::filesystem::path& file)
		{
			if (file == dir / "quantize-presets.json")
				throw std::runtime_error("can't be replaced");
		});
	CHECK(std::filesystem::exists(dir / "profile-switch" / "active" / "settings.json"));
	// The next start puts them back from there, then switches.
	f.start();
	CHECK(f.call("profile.list").at("active") == "Studio");
	CHECK(f.scale() == 2.0);
	CHECK(!std::filesystem::exists(dir / "profile-switch"));
	f.call("profile.activate", {{"name", "Default"}});
	f.start();
	CHECK(f.scale() == 1.5);
}

namespace
{
	// What a switch that GNUbase didn't finish leaves: the copies of both profiles' files, and in the settings folder
	// the next profile's settings.json without the rest. The settings were the active profile's.
	void interruptSwitch(Fixture& f, const std::string& next)
	{
		namespace fs = std::filesystem;
		f.call("profile.activate", {{"name", next}});
		f.client.reset();
		f.engine.reset();
		const fs::path dir = f.dir.path();
		const fs::path staging = dir / "profile-switch";
		fs::create_directories(staging / "active");
		fs::create_directories(staging / "next");
		for (const char* file : engine::settings::g_profileFiles)
		{
			if (fs::exists(dir / file))
				fs::copy_file(dir / file, staging / "active" / file);
			if (fs::exists(dir / "profiles" / next / file))
				fs::copy_file(dir / "profiles" / next / file, staging / "next" / file);
		}
		std::ofstream(staging / "active-profile") << "Default";
		fs::create_directories(dir / "profiles" / "Default");
		fs::remove(dir / "settings.json");
		if (fs::exists(staging / "next" / "settings.json"))
			fs::copy_file(staging / "next" / "settings.json", dir / "settings.json");
	}
}

TEST_CASE("a profile switch that GNUbase didn't finish is undone when it starts again, then switches", "[unit]")
{
	Fixture f;
	f.setScale(2.0);
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}});
	f.setScale(1.5);
	interruptSwitch(f, "Studio");
	f.start();
	CHECK(f.call("profile.list").at("active") == "Studio");
	CHECK(f.scale() == 2.0);
	CHECK(!std::filesystem::exists(f.dir.path() / "profile-switch"));
	// The active profile's files are complete in its folder, not half of each profile's.
	f.call("profile.activate", {{"name", "Default"}});
	f.start();
	CHECK(f.scale() == 1.5);
}

TEST_CASE("an unfinished profile switch whose files can't be put back keeps its copies", "[unit]")
{
	Fixture f;
	f.setScale(1.5);
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}});
	interruptSwitch(f, "Studio");
	// A settings file that can't be replaced.
	std::filesystem::remove(f.dir.path() / "quantize-presets.json");
	std::filesystem::create_directories(f.dir.path() / "quantize-presets.json" / "folder");
	f.start();
	// The switch waits, and the profiles say so.
	const Json list = f.call("profile.list");
	CHECK(list.at("active") == "Default");
	CHECK(list.at("next") == "Studio");
	CHECK(list.at("unfinished") == true);
	CHECK(std::filesystem::exists(f.dir.path() / "profile-switch" / "active" / "settings.json"));
	// Renaming the active profile would make the next start take its files for another profile's.
	CHECK_THAT(
		f.fails("profile.rename", {{"name", "Default"}, {"newName", "Home"}}), ContainsSubstring("didn't finish"));
	f.call("profile.rename", {{"name", "Studio"}, {"newName", "Laptop"}});
	CHECK(f.call("profile.list").at("next") == "Laptop");
}

TEST_CASE("the safe start mode's Delete program preferences drops an unfinished profile switch", "[unit]")
{
	Fixture f;
	f.call("profile.add", {{"name", "Studio"}});
	interruptSwitch(f, "Studio");
	engine::settings::cancelProfileSwitch(f.dir.path());
	CHECK(!std::filesystem::exists(f.dir.path() / "profile-switch"));
	f.start();
	CHECK(f.call("profile.list").at("active") == "Default");
}

namespace
{
	// The folders that the switches and profile.remove put aside to delete them, which are left if that failed.
	std::vector<std::filesystem::path> removedFolders(const std::filesystem::path& dir)
	{
		std::vector<std::filesystem::path> folders;
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir))
			if (entry.path().filename().u8string().rfind("profile-removed-", 0) == 0)
				folders.push_back(entry.path());
		return folders;
	}

	// A folder that something else has open while it lives, as a virus scanner may look at the files just written.
	// On Windows, where the file system has no POSIX deletes (FAT, exFAT, network shares), a deleted folder stays
	// while it is open, and no folder can be made under its name until then (#1284). NTFS, and the other OSes, free
	// the name at once.
	class OpenFolder
	{
	public:
		explicit OpenFolder(const std::filesystem::path& folder)
		{
#ifdef _WIN32
			m_handle = CreateFileW(folder.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			REQUIRE(m_handle != INVALID_HANDLE_VALUE);
#else
			m_handle = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			REQUIRE(m_handle != -1);
#endif
		}
		~OpenFolder()
		{
#ifdef _WIN32
			CloseHandle(m_handle);
#else
			::close(m_handle);
#endif
		}
		OpenFolder(const OpenFolder&) = delete;
		OpenFolder& operator=(const OpenFolder&) = delete;
		OpenFolder(OpenFolder&&) = delete;
		OpenFolder& operator=(OpenFolder&&) = delete;

	private:
#ifdef _WIN32
		HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
		int m_handle = -1;
#endif
	};
}

TEST_CASE("profiles switch back and forth many times in a row while something has the deleted folders open", "[unit]")
{
	namespace fs = std::filesystem;
	Fixture f;
	f.setScale(2.0);
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}});
	f.setScale(1.5);
	f.client.reset();
	f.engine.reset();
	const fs::path dir = f.dir.path();
	const std::string defaults = contents(dir / "settings.json");
	const std::string studio = contents(dir / "profiles" / "Studio" / "settings.json");
	REQUIRE(defaults != studio);
	// Each switch deletes the next profile's folder, and the one after makes it again under that name, while the
	// folder is still open.
	std::unique_ptr<OpenFolder> held;
	for (int round = 0; round < 20; ++round)
	{
		INFO("round " << round);
		const bool toStudio = round % 2 == 0;
		const std::string active = toStudio ? "Default" : "Studio";
		const std::string next = toStudio ? "Studio" : "Default";
		auto opened = std::make_unique<OpenFolder>(dir / "profiles" / next);
		std::ofstream(dir / "profiles.json") << Json{{"active", active}, {"next", next}}.dump();
		engine::settings::switchProfile(dir);
		held = std::move(opened); // the one before stays open until this switch made its folder again
		REQUIRE(Json::parse(contents(dir / "profiles.json")) == Json{{"active", next}, {"next", ""}});
		REQUIRE(contents(dir / "settings.json") == (toStudio ? studio : defaults));
		REQUIRE(contents(dir / "profiles" / active / "settings.json") == (toStudio ? defaults : studio));
		REQUIRE(!fs::exists(dir / "profiles" / next));
		REQUIRE(!fs::exists(dir / "profile-switch"));
	}
	held.reset();
	f.start();
	CHECK(f.call("profile.list").at("active") == "Default");
	CHECK(f.scale() == 1.5);
	CHECK(removedFolders(dir).empty());
}

TEST_CASE("a deleted profile can be added again under its name at once while its folder is still open", "[unit]")
{
	const Fixture f;
	f.call("profile.add", {{"name", "Studio"}});
	{
		const OpenFolder held(f.dir.path() / "profiles" / "Studio");
		f.call("profile.remove", {{"name", "Studio"}});
		f.call("profile.add", {{"name", "Studio"}});
	}
	CHECK(f.names() == std::vector<std::string>{"Default", "Studio"});
	CHECK(std::filesystem::is_directory(f.dir.path() / "profiles" / "Studio"));
}

TEST_CASE("what is left of a profile folder that couldn't be deleted goes at the next start", "[unit]")
{
	namespace fs = std::filesystem;
	Fixture f;
	f.setScale(1.5);
	const fs::path left = f.dir.path() / "profile-removed-0123456789abcdef";
	fs::create_directories(left / "folder");
	std::ofstream(left / "settings.json") << "{}";
	std::ofstream(f.dir.path() / "profile-removed-0000000000000001").close();
	f.start();
	CHECK(removedFolders(f.dir.path()).empty());
	CHECK(f.scale() == 1.5);
	CHECK(f.names() == std::vector<std::string>{"Default"});
}

#ifndef _WIN32
// Not on Windows: there a folder isn't renamed while a file in it is open, which is what keeps a file from being
// deleted there.
TEST_CASE(
	"a deleted profile whose folder can't be deleted completely is gone at once and the rest goes later", "[unit]")
{
	namespace fs = std::filesystem;
	Fixture f;
	f.call("profile.add", {{"name", "Studio"}});
	// A folder in it whose file can't be deleted.
	const fs::path locked = f.dir.path() / "profiles" / "Studio" / "Locked";
	fs::create_directories(locked);
	std::ofstream(locked / "settings.json") << "{}";
	fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
	if (std::ofstream(locked / "probe").good())
	{
		fs::permissions(locked, fs::perms::owner_all);
		SKIP("the folder can be changed here anyway, e.g. by root");
	}
	f.call("profile.remove", {{"name", "Studio"}});
	CHECK(f.names() == std::vector<std::string>{"Default"});
	f.call("profile.add", {{"name", "Studio"}});
	CHECK(f.names() == std::vector<std::string>{"Default", "Studio"});
	CHECK(!fs::exists(locked));
	const std::vector<fs::path> left = removedFolders(f.dir.path());
	REQUIRE(left.size() == 1);
	fs::permissions(left.front() / "Locked", fs::perms::owner_all);
	f.start();
	CHECK(removedFolders(f.dir.path()).empty());
	CHECK(f.names() == std::vector<std::string>{"Default", "Studio"});
}
#endif

#ifndef _WIN32
namespace
{
	// A folder with a file that can't be deleted, as the folder's permissions keep it. When the test ends, also after
	// a failed check, the folder can be changed again, where it is or in a profile-removed-* folder that a switch put
	// it in, so that the settings folder can be deleted.
	class LockedFolder
	{
	public:
		LockedFolder(std::filesystem::path settings, std::filesystem::path folder)
			: m_settings(std::move(settings)), m_folder(std::move(folder))
		{
			namespace fs = std::filesystem;
			fs::create_directories(m_folder);
			std::ofstream(m_folder / "settings.json") << "{}";
			fs::permissions(m_folder, fs::perms::owner_read | fs::perms::owner_exec);
		}
		~LockedFolder() { unlock(); }
		LockedFolder(const LockedFolder&) = delete;
		LockedFolder& operator=(const LockedFolder&) = delete;
		LockedFolder(LockedFolder&&) = delete;
		LockedFolder& operator=(LockedFolder&&) = delete;

		// Whether the file can't be deleted indeed: root changes any folder.
		bool locked() const { return !std::ofstream(m_folder / "probe").good(); }

		void unlock() const
		{
			namespace fs = std::filesystem;
			std::error_code ignored;
			fs::permissions(m_folder, fs::perms::owner_all, ignored);
			std::error_code error;
			for (fs::directory_iterator entry(m_settings, error), end; !error && entry != end; entry.increment(error))
				if (entry->path().filename().u8string().rfind("profile-removed-", 0) == 0)
					fs::permissions(entry->path() / m_folder.filename(), fs::perms::owner_all, ignored);
		}

	private:
		std::filesystem::path m_settings;
		std::filesystem::path m_folder;
	};
}

// Not on Windows, as above.
TEST_CASE(
	"a profile switch deletes the next profile's folder at once though something in it can't be deleted", "[unit]")
{
	namespace fs = std::filesystem;
	Fixture f;
	f.setScale(1.5);
	f.call("profile.add", {{"name", "Studio"}});
	const fs::path studio = f.dir.path() / "profiles" / "Studio";
	const LockedFolder locked(f.dir.path(), studio / "Locked");
	if (!locked.locked())
		SKIP("the folder can be changed here anyway, e.g. by root");
	f.call("profile.activate", {{"name", "Studio"}});
	f.start();
	CHECK(f.call("profile.list").at("active") == "Studio");
	CHECK(f.scale() == 1.0);
	// Its folder in profiles is gone at once, so it isn't a profile of its own; what can't be deleted waits aside.
	CHECK(!fs::exists(studio));
	CHECK(f.names() == std::vector<std::string>{"Default", "Studio"});
	CHECK(removedFolders(f.dir.path()).size() == 1);
	f.setScale(2.0);
	// The switch back makes the folder again, with the profile's files and without the folder that waits aside.
	f.call("profile.activate", {{"name", "Default"}});
	f.start();
	CHECK(f.call("profile.list").at("active") == "Default");
	CHECK(f.scale() == 1.5);
	CHECK(fs::is_regular_file(studio / "settings.json"));
	CHECK(!fs::exists(studio / "Locked"));
	CHECK(removedFolders(f.dir.path()).size() == 1);
	// Once it can be deleted, the next start deletes it.
	locked.unlock();
	f.start();
	CHECK(removedFolders(f.dir.path()).empty());
	f.call("profile.activate", {{"name", "Studio"}});
	f.start();
	CHECK(f.scale() == 2.0);
}

// Not on Windows, as above.
TEST_CASE(
	"a profile switch goes ahead when a profile-switch folder that was left can't be deleted completely", "[unit]")
{
	namespace fs = std::filesystem;
	Fixture f;
	f.setScale(2.0);
	f.call("profile.duplicate", {{"name", "Default"}, {"newName", "Studio"}});
	f.setScale(1.5);
	// What an earlier switch couldn't delete of its copies.
	const fs::path staging = f.dir.path() / "profile-switch";
	const LockedFolder locked(f.dir.path(), staging / "Locked");
	if (!locked.locked())
		SKIP("the folder can be changed here anyway, e.g. by root");
	f.call("profile.activate", {{"name", "Studio"}});
	f.start();
	const Json list = f.call("profile.list");
	CHECK(list.at("active") == "Studio");
	CHECK(list.at("next") == "");
	CHECK(list.at("unfinished") == false);
	CHECK(f.scale() == 2.0);
	CHECK(!fs::exists(staging));
	CHECK(removedFolders(f.dir.path()).size() == 1);
	// Once it can be deleted, the next start deletes it.
	locked.unlock();
	f.start();
	CHECK(removedFolders(f.dir.path()).empty());
	f.call("profile.activate", {{"name", "Default"}});
	f.start();
	CHECK(f.scale() == 1.5);
}
#endif
