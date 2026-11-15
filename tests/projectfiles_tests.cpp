// SPDX-License-Identifier: GPL-3.0-or-later
// Project handling (#77, docs/features/project-files.md): save, save as, save new version, revert, auto save, recent
// projects and templates through the engine API, which MCP and Lua use as they are, and the File menu and the start
// screen in the app's UI.
#include "engine/engine.h"
#include "platform/log.h"
#include "testutils/commandholds.h"
#include "testutils/heldjob.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace
{
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		REQUIRE(!result.ok());
		return result.error;
	}

	std::string utf8(const fs::path& path)
	{
		return path.u8string();
	}

	bool modified(Client& client)
	{
		return run(client, "project.revision").at("modified").get<bool>();
	}

	size_t trackCount(Client& client)
	{
		return run(client, "project.get").at("tracks").size();
	}

	std::vector<std::string> recent(Client& client)
	{
		std::vector<std::string> files;
		const Json projects = run(client, "project.recent").at("projects");
		for (const Json& project : projects)
			files.push_back(project.at("file").get<std::string>());
		return files;
	}

	// A second more than the auto save interval (project.autoSaveInterval): a due auto save takes the time when it
	// runs, a moment after the clock's tick, as the time of the last one.
	constexpr auto g_pastInterval = std::chrono::minutes(15) + std::chrono::seconds(1);

	// How many failed auto saves the engine's error log has.
	size_t loggedFailures(const fs::path& logFile)
	{
		gnubase::logging::flush();
		const std::string text = test::readText(logFile);
		const std::string failure = "project.autoSave failed";
		size_t count = 0;
		for (size_t at = text.find(failure); at != std::string::npos; at = text.find(failure, at + 1))
			++count;
		return count;
	}

	// project.autoSave in engine.commandLog.
	Json autoSaveLog(Client& client)
	{
		Json entries = Json::array();
		const Json log = run(client, "engine.commandLog");
		for (const Json& entry : log.at("commands"))
			if (entry.at("command") == "project.autoSave")
				entries.push_back(entry);
		return entries;
	}

	// Puts a folder, with a file in it, in the place of the temporary file that saving writes before it renames it to
	// file: saving fails, and can't remove the folder afterwards, so it fails again for the same reason.
	void blockSaving(const fs::path& file)
	{
		const fs::path folder = fs::u8path(file.u8string() + ".saving");
		fs::create_directories(folder);
		std::ofstream(folder / "blocked") << "not a temporary file";
	}

	// Collects project.autoSaved and project.autoSaveFailed, which the engine's own auto saves announce.
	class AutoSaveEvents
	{
	public:
		explicit AutoSaveEvents(Engine& engine)
			: m_listener(engine,
				  [this](const Event& event)
				  {
					  if (event.name != "project.autoSaved" && event.name != "project.autoSaveFailed")
						  return;
					  const std::scoped_lock lock(m_mutex);
					  m_names.push_back(event.name);
					  m_last = event.data;
				  })
		{
		}

		// The names of the events so far, once the commands queued before, such as the engine's auto saves, ran.
		std::vector<std::string> names(Client& client)
		{
			run(client, "engine.commandLog"); // runs after them
			const std::scoped_lock lock(m_mutex);
			return m_names;
		}

		// The last event's data, once the commands queued before ran.
		Json last(Client& client)
		{
			run(client, "engine.commandLog");
			const std::scoped_lock lock(m_mutex);
			return m_last;
		}

	private:
		std::mutex m_mutex;
		std::vector<std::string> m_names;
		Json m_last;
		Listener m_listener; // last, so that it stops before the rest goes
	};
}

TEST_CASE(
	"save without a path saves to the project's file, and the project knows whether it changed since", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	CHECK(!modified(client)); // a new project
	CHECK_THAT(reject(client, "project.save"), ContainsSubstring("no file"));

	run(client, "track.add", {{"type", "audio"}});
	CHECK(modified(client));
	const fs::path file = dir.path() / "Song" / "Song.dawproject";
	fs::create_directories(file.parent_path());
	run(client, "project.save", {{"path", utf8(file)}}); // save as
	CHECK(!modified(client));
	CHECK(run(client, "project.location").at("file") == utf8(fs::absolute(file)));

	run(client, "track.add", {{"type", "midi"}});
	CHECK(modified(client));
	run(client, "project.save"); // save
	CHECK(!modified(client));
	run(client, "project.new");
	CHECK(!modified(client));
	run(client, "project.load", {{"path", utf8(file)}});
	CHECK(trackCount(client) == 2);
	CHECK(!modified(client));
}

TEST_CASE("save new version saves under the next free number and goes on with it", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	CHECK_THAT(reject(client, "project.saveNewVersion"), ContainsSubstring("no file"));

	const fs::path file = dir.path() / "My Project.dawproject";
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});
	const Json first = run(client, "project.saveNewVersion");
	CHECK(first.at("file") == utf8(fs::absolute(dir.path() / "My Project-01.dawproject")));
	CHECK(run(client, "project.location").at("file") == first.at("file"));
	CHECK(!modified(client));

	// From a version, the next number of the same name, after the highest one there.
	fs::copy_file(file, dir.path() / "My Project-05.dawproject");
	CHECK(run(client, "project.saveNewVersion").at("file") ==
		utf8(fs::absolute(dir.path() / "My Project-06.dawproject")));
	CHECK(fs::exists(file)); // the original stays

	// A number too long for a version is part of another name, and doesn't keep the next version from being saved.
	fs::copy_file(file, dir.path() / "My Project-12345678901.dawproject");
	CHECK(run(client, "project.saveNewVersion").at("file") ==
		utf8(fs::absolute(dir.path() / "My Project-07.dawproject")));

	// Versions whose name or extension is written in another case count too: the file systems of Windows and macOS
	// don't tell them apart, so the next version would overwrite them there.
	fs::copy_file(file, dir.path() / "My Project-08.DAWPROJECT");
	CHECK(run(client, "project.saveNewVersion").at("file") ==
		utf8(fs::absolute(dir.path() / "My Project-09.dawproject")));
	fs::copy_file(file, dir.path() / "MY PROJECT-10.dawproject");
	CHECK(run(client, "project.saveNewVersion").at("file") ==
		utf8(fs::absolute(dir.path() / "My Project-11.dawproject")));

	// Only .dawproject, in any case, is the extension that a version replaces: a file saved through the API under a
	// name with a dot is numbered from its whole name.
	run(client, "project.save", {{"path", utf8(dir.path() / "Song v2.1")}});
	CHECK(
		run(client, "project.saveNewVersion").at("file") == utf8(fs::absolute(dir.path() / "Song v2.1-01.dawproject")));
	run(client, "project.save", {{"path", utf8(dir.path() / "Mix-03.DAWPROJECT")}});
	CHECK(run(client, "project.saveNewVersion").at("file") == utf8(fs::absolute(dir.path() / "Mix-04.dawproject")));

	// Dots and spaces at the end of a file's name, which project.save keeps on Linux and macOS while Windows drops
	// them, aren't part of its versions' name, nor is the extension before them (#706).
	run(client, "project.save", {{"path", utf8(dir.path() / "Take.dawproject.")}});
	CHECK(run(client, "project.saveNewVersion").at("file") == utf8(fs::absolute(dir.path() / "Take-01.dawproject")));
	run(client, "project.save", {{"path", utf8(dir.path() / "Take. .")}});
	CHECK(run(client, "project.saveNewVersion").at("file") == utf8(fs::absolute(dir.path() / "Take-02.dawproject")));
	// A name whose last characters before where the extension would be aren't all ASCII: the comparison mustn't cut
	// one of them in two, which Windows can't take for a path.
	run(client, "project.save", {{"path", utf8(dir.path() / fs::u8path(u8"Für Elise v2"))}});
	CHECK(run(client, "project.saveNewVersion").at("file") ==
		utf8(fs::absolute(dir.path() / fs::u8path(u8"Für Elise v2-01.dawproject"))));

	// The folder scan folds ASCII letters only, but the file systems of Windows and macOS also fold others: "übung-01"
	// is "Übung-01" there, so the next version steps past it instead of overwriting it. On Linux they are two files,
	// and "Übung-01" is free.
	const fs::path lower = dir.path() / fs::u8path(u8"übung-01.dawproject");
	fs::copy_file(file, lower);
	const bool folds = fs::exists(dir.path() / fs::u8path(u8"Übung-01.dawproject"));
	run(client, "project.save", {{"path", utf8(dir.path() / fs::u8path(u8"Übung.dawproject"))}});
	CHECK(run(client, "project.saveNewVersion").at("file") ==
		utf8(fs::absolute(dir.path() / fs::u8path(folds ? u8"Übung-02.dawproject" : u8"Übung-01.dawproject"))));
	CHECK(test::readText(lower) == test::readText(file)); // not overwritten

	// Each version has the project as it was.
	run(client, "project.load", {{"path", utf8(file)}});
	CHECK(trackCount(client) == 0);
	run(client, "project.load", {{"path", first.at("file")}});
	CHECK(trackCount(client) == 1);
}

TEST_CASE("revert goes back to the project as it was last saved", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	CHECK_THAT(reject(client, "project.revert"), ContainsSubstring("no file"));

	const fs::path file = dir.path() / "Song.dawproject";
	run(client, "track.add", {{"type", "audio"}});
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});
	run(client, "track.add", {{"type", "audio"}});
	CHECK(trackCount(client) == 3);

	run(client, "project.revert");
	CHECK(trackCount(client) == 1);
	CHECK(!modified(client));
	CHECK(run(client, "history.get").at("steps").empty()); // as after loading
	CHECK(run(client, "project.location").at("file") == utf8(fs::absolute(file)));
}

TEST_CASE(
	"auto save keeps copies of a changed project in its Auto Saves folder, the oldest overwritten", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "settings.set", {{"key", "project.autoSaveFiles"}, {"value", 2}});

	// An untitled project goes to the settings folder's Auto Saves, named after the project.
	CHECK(run(client, "project.autoSave").at("file").is_null()); // nothing changed
	run(client, "track.add", {{"type", "audio"}});
	CHECK(
		run(client, "project.autoSave").at("file") == utf8(dir.path() / "settings" / "Auto Saves" / "Untitled-01.bak"));
	CHECK(run(client, "project.autoSave").at("file").is_null()); // nothing changed since
	CHECK(modified(client));                                     // an auto save doesn't save the project

	const fs::path file = dir.path() / "Song" / "Song.dawproject";
	fs::create_directories(file.parent_path());
	run(client, "project.save", {{"path", utf8(file)}});
	const fs::path autoSaves = dir.path() / "Song" / "Auto Saves";
	std::vector<std::string> saved;
	for (int i = 0; i < 3; ++i)
	{
		run(client, "track.add", {{"type", "audio"}});
		saved.push_back(run(client, "project.autoSave").at("file").get<std::string>());
	}
	CHECK(saved[0] == utf8(autoSaves / "Song-01.bak"));
	CHECK(saved[1] == utf8(autoSaves / "Song-02.bak"));
	CHECK(saved[2] == saved[0]); // the oldest of the two
	CHECK(!fs::exists(autoSaves / "Song-03.bak"));

	// An auto save opens with the project as it was then, but without a file and unsaved, so that saving asks for a
	// path instead of overwriting the auto save; it isn't a recent project.
	run(client, "project.load", {{"path", saved[2]}});
	CHECK(trackCount(client) == 4);
	CHECK(run(client, "project.location").at("file").is_null());
	CHECK(modified(client));
	CHECK(recent(client) == std::vector<std::string>{utf8(fs::absolute(file))});
	CHECK_THAT(reject(client, "project.save"), ContainsSubstring("no file"));

	// Due auto saves, as the engine checks every half a minute: only when on and the interval has passed.
	run(client, "track.add", {{"type", "audio"}});
	CHECK(run(client, "project.autoSave", {{"due", true}}).at("file").is_null()); // just loaded
	run(client, "settings.set", {{"key", "project.autoSave"}, {"value", false}});
	CHECK(run(client, "project.autoSave", {{"due", true}}).at("file").is_null());
}

TEST_CASE("auto saves are named after the project file as its versions are", "[integration]")
{
	// After the whole name of a file without the .dawproject extension, so that "Mix v2.1" and "Mix v2.2" don't share
	// theirs, and without dots and spaces at its end and the extension before them (#706), as Save New Version does.
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const std::vector<std::pair<std::string, std::string>> named{{"Mix v2.1", "Mix v2.1-01.bak"},
		{"Mix v2.2", "Mix v2.2-01.bak"}, {"Take.DAWPROJECT. ", "Take-01.bak"},
		{u8"Für Elise v2", u8"Für Elise v2-01.bak"}};
	for (const auto& [name, autoSave] : named)
	{
		INFO(name);
		run(client, "project.save", {{"path", utf8(dir.path() / fs::u8path(name))}});
		run(client, "track.add", {{"type", "audio"}});
		CHECK(run(client, "project.autoSave").at("file") == utf8(dir.path() / "Auto Saves" / fs::u8path(autoSave)));
	}
}

TEST_CASE("a project opened from an auto save goes on untitled, outside Auto Saves, until it is saved as a file",
	"[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const fs::path file = dir.path() / "Song" / "Song.dawproject";
	fs::create_directories(file.parent_path());
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});
	const fs::path autoSave = dir.path() / "Song" / "Auto Saves" / "Song-01.bak";
	REQUIRE(run(client, "project.autoSave").at("file") == utf8(autoSave));

	// Also when the path names the auto save another way: with "." in it, or with other case, as the file systems of
	// Windows and macOS allow.
	const fs::path otherCase = dir.path() / "Other" / "auto saves" / "Other-01.BAK";
	fs::create_directories(otherCase.parent_path());
	fs::copy_file(autoSave, otherCase);
	for (const fs::path& path : {autoSave, dir.path() / "Song" / "Auto Saves" / "." / "Song-01.bak", otherCase})
	{
		INFO(utf8(path));
		run(client, "project.load", {{"path", utf8(path)}});
		CHECK(trackCount(client) == 1);
		const Json location = run(client, "project.location");
		CHECK(location.at("file").is_null());
		CHECK(location.at("mediaFolder").is_null()); // so media aren't copied to Auto Saves/Audio
		CHECK(modified(client));
		CHECK_THAT(reject(client, "project.save"), ContainsSubstring("no file"));
		CHECK_THAT(reject(client, "project.revert"), ContainsSubstring("no file"));
		CHECK(recent(client) == std::vector<std::string>{utf8(fs::absolute(file))});
	}

	// Its next auto saves go where those of untitled projects go, not to Auto Saves/Auto Saves.
	run(client, "track.add", {{"type", "audio"}});
	CHECK(
		run(client, "project.autoSave").at("file") == utf8(dir.path() / "settings" / "Auto Saves" / "Untitled-01.bak"));
	CHECK(!fs::exists(autoSave.parent_path() / "Auto Saves"));

	// Saving it as the project file makes that its file again, with its folders.
	run(client, "project.save", {{"path", utf8(file)}});
	CHECK(run(client, "project.location").at("folder") == utf8(fs::absolute(file.parent_path())));
	CHECK(recent(client) == std::vector<std::string>{utf8(fs::absolute(file))});
	run(client, "track.add", {{"type", "audio"}});
	CHECK(run(client, "project.autoSave").at("file") == utf8(dir.path() / "Song" / "Auto Saves" / "Song-02.bak"));

	// The auto save of a project that never had a file, and one whose project file is gone, open untitled too.
	run(client, "project.new");
	run(client, "track.add", {{"type", "midi"}});
	const fs::path untitled = dir.path() / "settings" / "Auto Saves" / "Untitled-02.bak";
	REQUIRE(run(client, "project.autoSave").at("file") == utf8(untitled));
	fs::remove(file);
	for (const fs::path& path : {untitled, autoSave})
	{
		INFO(utf8(path));
		run(client, "project.load", {{"path", utf8(path)}});
		CHECK(run(client, "project.location").at("file").is_null());
		CHECK(modified(client));
	}
	CHECK(recent(client) == std::vector<std::string>{utf8(fs::absolute(file))});

	// A .bak elsewhere isn't an auto save: it opens as the project's file.
	const fs::path copy = dir.path() / "Song-01.bak";
	fs::copy_file(autoSave, copy);
	run(client, "project.load", {{"path", utf8(copy)}});
	CHECK(run(client, "project.location").at("file") == utf8(fs::absolute(copy)));
}

TEST_CASE("the engine auto saves a changed project on its own once the interval passed", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	AutoSaveEvents events(engine);
	const fs::path file = dir.path() / "Song.dawproject";
	const fs::path autoSave = dir.path() / "Auto Saves" / "Song-01.bak";
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});

	// The engine checks every 30 seconds; the interval is 15 minutes since the project was saved.
	engine.advanceClock(std::chrono::minutes(14));
	CHECK(events.names(client).empty());
	CHECK(autoSaveLog(client).empty()); // checks that save nothing stay out of the log
	engine.advanceClock(std::chrono::minutes(1) + std::chrono::seconds(1));
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaved"});
	CHECK(fs::is_regular_file(autoSave));
	const Json log = autoSaveLog(client);
	REQUIRE(log.size() == 1);
	CHECK(log[0].at("client") == "engine");
	CHECK(log[0].at("parameters") == Json{{"due", true}});
	CHECK(!log[0].contains("error"));
	CHECK(modified(client)); // an auto save isn't a save

	// Nothing changed since: the next interval saves nothing.
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client).size() == 1);
	CHECK(autoSaveLog(client).size() == 1);

	// Off, nothing is saved however long it waits.
	run(client, "settings.set", {{"key", "project.autoSave"}, {"value", false}});
	run(client, "track.add", {{"type", "audio"}});
	engine.advanceClock(std::chrono::minutes(60));
	CHECK(events.names(client).size() == 1);
}

TEST_CASE("project.autoSaveInterval sets how long the engine waits before it auto saves", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	AutoSaveEvents events(engine);
	run(client, "settings.set", {{"key", "project.autoSaveInterval"}, {"value", 5}});
	run(client, "project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	run(client, "track.add", {{"type", "audio"}});

	engine.advanceClock(std::chrono::minutes(4));
	CHECK(events.names(client).empty());
	engine.advanceClock(std::chrono::minutes(1) + std::chrono::seconds(1));
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaved"});
	CHECK(fs::is_regular_file(dir.path() / "Auto Saves" / "Song-01.bak"));
}

TEST_CASE("a failing auto save waits for the next interval, is logged once and announces each failure", "[integration]")
{
	const test::TempDir dir;
	const fs::path logFile = dir.path() / "gnubase.log";
	const gnubase::logging::Session session(logFile);
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	AutoSaveEvents events(engine);
	const fs::path file = dir.path() / "Song.dawproject";
	const fs::path autoSaves = dir.path() / "Auto Saves";
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});

	// A file in the place of the Auto Saves folder: the folder can't be made.
	std::ofstream(autoSaves) << "not a folder";
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaveFailed"});
	CHECK(events.last(client).at("file") == utf8(autoSaves / "Song-01.bak"));
	CHECK(!events.last(client).at("error").get<std::string>().empty());
	CHECK(events.last(client).at("repeated") == false);
	Json log = autoSaveLog(client);
	REQUIRE(log.size() == 1);
	CHECK(log[0].contains("error"));
	CHECK(loggedFailures(logFile) == 1);

	// Not tried again every 30 seconds but after the interval, and only the first failure is in the log.
	engine.advanceClock(std::chrono::minutes(14));
	CHECK(events.names(client).size() == 1);
	engine.advanceClock(std::chrono::minutes(1) + std::chrono::seconds(1));
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaveFailed", "project.autoSaveFailed"});
	CHECK(events.last(client).at("repeated") == true);
	CHECK(autoSaveLog(client).size() == 1);
	for (size_t interval = 1; interval <= 10; ++interval)
	{
		engine.advanceClock(g_pastInterval);
		CHECK(events.names(client).size() == 2 + interval); // after the auto save ran, before the clock goes on
	}
	CHECK(autoSaveLog(client).size() == 1);
	CHECK(loggedFailures(logFile) == 1);

	// A client's own auto save is its command: it is in the log, and fails with the reason.
	CHECK(!reject(client, "project.autoSave").empty());
	CHECK(autoSaveLog(client).size() == 2);
	CHECK(events.names(client).size() == 13);

	// Once one succeeds, the next failure is logged again.
	fs::remove(autoSaves);
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client).back() == "project.autoSaved");
	CHECK(fs::is_regular_file(autoSaves / "Song-01.bak"));
	CHECK(autoSaveLog(client).size() == 3);
	fs::remove_all(autoSaves);
	std::ofstream(autoSaves) << "not a folder";
	run(client, "track.add", {{"type", "audio"}});
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client).back() == "project.autoSaveFailed");
	CHECK(events.last(client).at("repeated") == false);
	log = autoSaveLog(client);
	REQUIRE(log.size() == 4);
	CHECK(log[3].contains("error"));
	CHECK(loggedFailures(logFile) == 2);

	// So is the next failure after saving or opening the project.
	engine.advanceClock(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 4);
	run(client, "project.save");
	run(client, "track.add", {{"type", "audio"}});
	engine.advanceClock(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 5);
	CHECK(events.last(client).at("repeated") == false);
	engine.advanceClock(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 5);
	run(client, "project.load", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});
	engine.advanceClock(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 6);
	CHECK(events.last(client).at("repeated") == false);
	CHECK(loggedFailures(logFile) == 4);
	CHECK(events.names(client).size() == 19);
}

TEST_CASE("a failing auto save is logged again when its reason changes, not when only its file does", "[integration]")
{
	const test::TempDir dir;
	const fs::path logFile = dir.path() / "gnubase.log";
	const gnubase::logging::Session session(logFile);
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	AutoSaveEvents events(engine);
	const fs::path file = dir.path() / "Song.dawproject";
	const fs::path autoSaves = dir.path() / "Auto Saves";
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});

	// Auto Saves can't be made, twice.
	std::ofstream(autoSaves) << "not a folder";
	engine.advanceClock(g_pastInterval);
	CHECK(events.last(client).at("repeated") == false); // once the auto save ran, before the clock goes on
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client).size() == 2);
	CHECK(events.last(client).at("repeated") == true);
	CHECK(loggedFailures(logFile) == 1);

	// Then it can be made, but the auto save can't be written, as a folder is in the place of its temporary file:
	// another reason, which is logged and announced as new.
	fs::remove(autoSaves);
	blockSaving(autoSaves / "Song-01.bak");
	blockSaving(autoSaves / "Song-02.bak");
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client).size() == 3);
	CHECK(events.last(client).at("file") == utf8(autoSaves / "Song-01.bak"));
	CHECK(events.last(client).at("repeated") == false);
	CHECK(loggedFailures(logFile) == 2);
	CHECK(autoSaveLog(client).size() == 2);

	// The same reason for the next file in turn is no news: the error names the file, the reason doesn't.
	std::ofstream(autoSaves / "Song-01.bak") << "an earlier auto save";
	engine.advanceClock(g_pastInterval);
	CHECK(events.names(client).size() == 4);
	CHECK(events.last(client).at("file") == utf8(autoSaves / "Song-02.bak"));
	CHECK_THAT(events.last(client).at("error").get<std::string>(), ContainsSubstring("Song-02.bak"));
	CHECK(events.last(client).at("repeated") == true);
	CHECK(loggedFailures(logFile) == 2);
	CHECK(autoSaveLog(client).size() == 2);

	// Back to the first reason: logged again, as it isn't the last one logged.
	fs::remove_all(autoSaves);
	std::ofstream(autoSaves) << "not a folder";
	engine.advanceClock(g_pastInterval);
	CHECK(events.last(client).at("repeated") == false);
	CHECK(loggedFailures(logFile) == 3);
	CHECK(autoSaveLog(client).size() == 3);
}

// Only the engine starts and stops its clock; others read the time from it, and tests move it with advanceClock.
static_assert(std::is_same_v<decltype(std::declval<Engine&>().clock()), const gnubase::engine::Clock&>);

TEST_CASE("saved and opened projects are recent projects, newest first, until forgotten", "[integration]")
{
	const test::TempDir dir;
	const fs::path a = dir.path() / "A.dawproject";
	const fs::path b = dir.path() / "B.dawproject";
	{
		Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
		Client client(engine);
		CHECK(recent(client).empty());
		run(client, "project.save", {{"path", utf8(a)}});
		run(client, "project.save", {{"path", utf8(b)}});
		run(client, "project.load", {{"path", utf8(a)}});
		CHECK(recent(client) == std::vector<std::string>{utf8(fs::absolute(a)), utf8(fs::absolute(b))});
		run(client, "project.autoSave"); // auto saves and templates aren't recent projects
		run(client, "project.saveTemplate", {{"name", "Empty"}});
		CHECK(recent(client).size() == 2);
	}
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings"); // kept in the settings folder
	Client client(engine);
	fs::remove(b);
	const Json projects = run(client, "project.recent").at("projects");
	REQUIRE(projects.size() == 2);
	CHECK(projects[0].at("exists") == true);
	CHECK(projects[1].at("exists") == false); // still listed, so that the user sees it went

	CHECK(run(client, "project.forgetRecent", {{"file", utf8(fs::absolute(b))}}).at("projects").size() == 1);
	CHECK(recent(client) == std::vector<std::string>{utf8(fs::absolute(a))});
	run(client, "project.forgetRecent");
	CHECK(recent(client).empty());
}

TEST_CASE("save and load succeed when the recent projects can't be written", "[integration]")
{
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	// A folder in the place of the recent projects' file: it can't be read, kept as a backup or replaced.
	fs::create_directories(dir.path() / "settings" / "recent-projects.json");
	std::mutex mutex;
	std::vector<std::string> events;
	const Listener listener(engine,
		[&](const Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "project.saved" || event.name == "project.reset")
				events.push_back(event.name);
		});

	run(client, "project.save", {{"path", utf8(file)}});
	CHECK(fs::is_regular_file(file));
	CHECK(!modified(client));
	run(client, "track.add", {{"type", "audio"}});
	run(client, "project.load", {{"path", utf8(file)}});
	CHECK(trackCount(client) == 0);
	CHECK(recent(client).empty()); // the list couldn't be written
	const std::scoped_lock lock(mutex);
	CHECK(events == std::vector<std::string>{"project.saved", "project.reset"});
}

TEST_CASE("templates are projects in the settings folder that new projects start from", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	CHECK(run(client, "project.templates").at("templates").empty());

	run(client, "project.set", {{"sampleRate", 44100.0}});
	run(client, "track.add", {{"type", "instrument"}});
	run(client, "track.add", {{"type", "audio"}});
	const Json saved = run(client, "project.saveTemplate", {{"name", "Band"}});
	CHECK(saved.at("file") == utf8(dir.path() / "settings" / "Project Templates" / "Band.dawproject"));
	CHECK(modified(client)); // saving a template doesn't save the project
	CHECK(run(client, "project.location").at("file").is_null());
	CHECK_THAT(reject(client, "project.saveTemplate", {{"name", "a/b"}}), ContainsSubstring("name"));
	// Nor names that Windows keeps for devices, which before Windows 11 are devices with an extension too, or names
	// that end in a dot or a space, which Windows drops.
	for (const char* name : {"NUL", "con", "Com1", "LPT9.mix", "aux .x", "Band.", "Band "})
	{
		INFO(name);
		CHECK_THAT(reject(client, "project.saveTemplate", {{"name", name}}), ContainsSubstring("name"));
	}

	const Json templates = run(client, "project.templates").at("templates");
	REQUIRE(templates.size() == 1);
	CHECK(templates[0].at("name") == "Band");
	CHECK(templates[0].at("file") == saved.at("file"));

	run(client, "project.new");
	CHECK(trackCount(client) == 0);
	run(client, "project.new", {{"template", "Band"}});
	const Json project = run(client, "project.get");
	CHECK(project.at("tracks").size() == 2);
	CHECK(project.at("sampleRate") == 44100.0);
	CHECK(run(client, "project.location").at("file").is_null()); // untitled: saving asks where
	CHECK(!modified(client));
	CHECK(run(client, "history.get").at("steps").empty());
	CHECK_THAT(reject(client, "project.new", {{"template", "Nothing"}}), ContainsSubstring("Nothing"));
	// Only templates in Project Templates: a name that isn't a file name is refused.
	fs::copy_file(fs::u8path(saved.at("file").get<std::string>()), dir.path() / "settings" / "x.dawproject");
	CHECK_THAT(reject(client, "project.new", {{"template", "../x"}}), ContainsSubstring("name"));
	// A template named as a device on Windows, which earlier builds saved and Linux, macOS and Windows 11 keep as a
	// file, still opens, although no template can be saved under such a name any more.
	fs::copy_file(fs::u8path(saved.at("file").get<std::string>()),
		dir.path() / "settings" / "Project Templates" / "Aux.dawproject");
	run(client, "project.new", {{"template", "Aux"}});
	CHECK(trackCount(client) == 2);
}

TEST_CASE("without a settings folder there are no templates, recent projects or untitled auto saves", "[integration]")
{
	Engine engine;
	Client client(engine);
	CHECK(run(client, "project.templates").at("templates").empty());
	CHECK_THAT(reject(client, "project.saveTemplate", {{"name", "Band"}}), ContainsSubstring("settings folder"));
	CHECK(recent(client).empty());
	run(client, "track.add", {{"type", "audio"}});
	CHECK(run(client, "project.autoSave").at("file").is_null());
}

// The File menu and the start screen in the app's UI (ui/projectfiles.h), driven with ui::Automation as MCP's UI tools
// do; file dialogs are answered as the user would (Shell::answerFileDialog, MCP's ui_file_dialog).
namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	bool disabled(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].value("attributes", Json::object()).contains("disabled");
	}

	void click(test::UiApp& app, const std::string& selector, int count = 1)
	{
		REQUIRE(app.waitUntil([&app, &selector] { return shown(app, selector); }));
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, gnubase::ui::MouseButton::Left, count, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// File > item.
	void fileMenu(test::UiApp& app, const std::string& command)
	{
		click(app, "#menu-bar button[menu=file-menu]");
		click(app, "menuitem[command=\"" + command + "\"]");
	}

	// Waits for the file dialog that the UI shows and answers it with the files.
	gnubase::ui::FileDialog answer(test::UiApp& app, std::vector<fs::path> files)
	{
		REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
		const auto [id, dialog] = *app.shell().fileDialogs().begin();
		const bool answered = app.shell().answerFileDialog(id, std::move(files));
		CHECK(answered);
		return dialog;
	}

	Json location(test::UiApp& app)
	{
		return app.call("project.location").at("file");
	}

	// The unsaved changes question in the main window, or null.
	Rml::Element* question(test::UiApp& app)
	{
		Rml::Context& context = app.main().context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* dialog = context.GetDocument(i)->GetElementById("discard-changes-dialog"))
				return dialog;
		return nullptr;
	}

	// What SDL sends when the user closes the last visible window (SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE): the window's
	// close request, and then its own quit request, which GNUbase handles in the same step.
	void closeLastWindow(test::UiApp& app)
	{
		SDL_Event close{};
		close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
		close.window.windowID = app.main().id();
		SDL_Event quit{};
		quit.type = SDL_EVENT_QUIT;
		REQUIRE(SDL_PushEvent(&close));
		REQUIRE(SDL_PushEvent(&quit));
	}
}

TEST_CASE("File > Save As, Save and Save New Version save the project through file dialogs", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	app.call("track.add", {{"type", "audio"}});

	// Save on a project without a file asks where, as Save As does; the extension is added.
	fileMenu(app, "file.save");
	const gnubase::ui::FileDialog dialog = answer(app, {dir.path() / "Song"});
	CHECK(dialog.kind == gnubase::ui::FileDialogKind::Save);
	CHECK(dialog.filters.at(0).extensions == "dawproject");
	const std::string song = utf8(fs::absolute(dir.path() / "Song.dawproject"));
	REQUIRE(app.waitUntil([&app, &song] { return location(app) == song; }));

	app.call("track.add", {{"type", "audio"}});
	fileMenu(app, "file.save"); // to its file, without asking
	REQUIRE(app.waitUntil([&app] { return !app.call("project.revision").at("modified").get<bool>(); }));
	CHECK(app.shell().fileDialogs().empty());

	fileMenu(app, "file.saveNewVersion");
	const std::string version = utf8(fs::absolute(dir.path() / "Song-01.dawproject"));
	REQUIRE(app.waitUntil([&app, &version] { return location(app) == version; }));

	fileMenu(app, "file.saveAs");
	answer(app, {dir.path() / "Other.dawproject"});
	REQUIRE(app.waitUntil([&app] { return location(app).get<std::string>().find("Other") != std::string::npos; }));
	fileMenu(app, "file.saveAs");
	answer(app, {}); // cancelled: nothing happens
	CHECK(location(app).get<std::string>().find("Other") != std::string::npos);

	// A name with a dot isn't taken for one with an extension.
	fileMenu(app, "file.saveAs");
	answer(app, {dir.path() / "Song v1.2"});
	const std::string dotted = utf8(fs::absolute(dir.path() / "Song v1.2.dawproject"));
	REQUIRE(app.waitUntil([&app, &dotted] { return location(app) == dotted; }));
	fileMenu(app, "file.saveNewVersion"); // numbered from the whole name
	const std::string dottedVersion = utf8(fs::absolute(dir.path() / "Song v1.2-01.dawproject"));
	REQUIRE(app.waitUntil([&app, &dottedVersion] { return location(app) == dottedVersion; }));

	// The extension in another case is the extension.
	fileMenu(app, "file.saveAs");
	answer(app, {dir.path() / "Mix.DAWPROJECT"});
	const std::string upper = utf8(fs::absolute(dir.path() / "Mix.DAWPROJECT"));
	REQUIRE(app.waitUntil([&app, &upper] { return location(app) == upper; }));
	fileMenu(app, "file.saveNewVersion");
	const std::string upperVersion = utf8(fs::absolute(dir.path() / "Mix-01.dawproject"));
	REQUIRE(app.waitUntil([&app, &upperVersion] { return location(app) == upperVersion; }));

	// A name that ends in a dot, as the dialogs of Linux return it as typed, is saved without a second dot.
	fileMenu(app, "file.saveAs");
	answer(app, {dir.path() / "Take."});
	const std::string take = utf8(fs::absolute(dir.path() / "Take.dawproject"));
	REQUIRE(app.waitUntil([&app, &take] { return location(app) == take; }));

	// Dots and spaces at the end go before the extension is checked, which Windows drops anyway, so that a name ending
	// in the extension and then a dot or a space gets it once (#706).
	const std::vector<std::pair<std::string, std::string>> typed{{"Final.dawproject.", "Final.dawproject"},
		{"Live.DAWPROJECT ", "Live.DAWPROJECT"}, {"Demo . .", "Demo.dawproject"}};
	for (const auto& [name, saved] : typed)
	{
		INFO(name);
		fileMenu(app, "file.saveAs");
		answer(app, {dir.path() / fs::u8path(name)});
		const std::string expected = utf8(fs::absolute(dir.path() / fs::u8path(saved)));
		REQUIRE(app.waitUntil([&app, &expected] { return location(app) == expected; }));
		CHECK(fs::is_regular_file(dir.path() / fs::u8path(saved)));
	}
	CHECK(!fs::exists(dir.path() / "Final.dawproject.dawproject"));

	// A name that Windows keeps for a device is the user's to choose: with the extension it is a file like any other on
	// Linux, macOS and Windows 11 (on Windows 10 it is still the device, and this fails).
	fileMenu(app, "file.saveAs");
	answer(app, {dir.path() / "NUL"});
	const std::string device = utf8(fs::absolute(dir.path() / "NUL.dawproject"));
	REQUIRE(app.waitUntil([&app, &device] { return location(app) == device; }));
	CHECK(fs::is_regular_file(dir.path() / "NUL.dawproject"));
}

TEST_CASE("a save or backup that fails shows its error", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";

	fileMenu(app, "file.saveAs");
	answer(app, {notFolder / "Song.dawproject"});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK(!elements(app, "#project-error-message").at(0).at("text").get<std::string>().empty());
	click(app, "#project-error-dialog button[result=ok]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#project-error-dialog"); }));
	CHECK(location(app).is_null());

	// A backup fails in its job, after project.backup returned.
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	fileMenu(app, "file.backup");
	answer(app, {notFolder / "Backup"});
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	click(app, "#project-error-dialog button[result=ok]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#project-error-dialog"); }));

	// A backup that is to be opened isn't if the project changed while the job ran, and says so. The job is held at
	// its first progress until the edit ran, so that it can't ask to open the backup first (#1154).
	test::writeWav(dir.path() / "take.wav", {std::vector<float>(96000, 0.25f), 2, 48000}); // 1 s
	app.call("media.add", {{"path", utf8(dir.path() / "take.wav")}});
	app.call("project.save", Json::object());
	const Json song = location(app);
	test::HeldJob held(app.engine(), "project.backup");
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Opened"});
	click(app, "#backup-keep-active");
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&held] { return held.held(); }, std::chrono::seconds(30)));
	app.call("project.set", {{"name", "Edited"}});
	CHECK(held.release());
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }, std::chrono::seconds(30)));
	CHECK_THAT(elements(app, "#project-error-message").at(0).at("text").get<std::string>(),
		ContainsSubstring("wasn't opened"));
	CHECK(fs::exists(dir.path() / "Opened" / "Song.dawproject"));
	CHECK(location(app) == song);
	CHECK(app.call("project.get").at("name") == "Edited");
	click(app, "#project-error-dialog button[result=ok]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#project-error-dialog"); }));
}

namespace
{
	// Adds a track and waits until the track list shows it, and a step more: by then the UI handled the engine's
	// events from before, and ran what it posted for them.
	void handleEarlierEvents(test::UiApp& app)
	{
		const size_t tracks = elements(app, "#track-list .track-name").size();
		app.call("track.add", {{"type", "audio"}});
		REQUIRE(
			app.waitUntil([&app, tracks] { return elements(app, "#track-list .track-name").size() == tracks + 1; }));
		app.shell().step(std::chrono::milliseconds(0));
		app.shell().step(std::chrono::milliseconds(0));
	}

	void closeError(test::UiApp& app)
	{
		click(app, "#project-error-dialog button[result=ok]");
		REQUIRE(app.waitUntil([&app] { return !shown(app, "#project-error-dialog"); }));
	}

	// The message of the error dialog that is open.
	std::string errorMessage(test::UiApp& app)
	{
		const Json message = elements(app, "#project-error-message");
		return message.size() == 1 ? message[0].at("text").get<std::string>() : std::string();
	}
}

TEST_CASE("an auto save that fails shows its error once, and again when it fails for another reason", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path autoSaves = dir.path() / "Auto Saves";
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	app.call("track.add", {{"type", "audio"}});
	std::ofstream(autoSaves) << "not a folder";

	// The engine's own auto save, once the interval passed.
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK_THAT(
		elements(app, "#project-error-message").at(0).at("text").get<std::string>(), ContainsSubstring("auto saved"));
	closeError(app);

	// Not at every interval while it fails for the same reason.
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));

	// Another reason: Auto Saves can be made, but a folder is in the place of the auto save's temporary file.
	fs::remove(autoSaves);
	blockSaving(autoSaves / "Song-01.bak");
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	closeError(app);
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));

	// Again after the project was saved, although the next auto save fails for the same reason.
	fileMenu(app, "file.save");
	REQUIRE(app.waitUntil([&app] { return !app.call("project.revision").at("modified").get<bool>(); }));
	app.call("track.add", {{"type", "audio"}}); // so that the next auto save is due
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK_THAT(errorMessage(app), ContainsSubstring("auto saved"));
	closeError(app);
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));

	// While another dialog is open the error can't show; it shows at the next failure instead.
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
	fs::remove_all(autoSaves);
	std::ofstream(autoSaves) << "not a folder";
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));
	click(app, "#start-screen button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#start-screen"); }));
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));

	// A dialog that the user asks for meanwhile takes the error's place, and the error shows again at the next failure.
	app.keys().run("file.revert", Json::object(), app.main(), nullptr); // with unsaved changes
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	CHECK(!shown(app, "#project-error-dialog"));
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	closeError(app);
}

TEST_CASE("a failed auto save's error makes way for the error of a newer one and of a failed save", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path autoSaves = dir.path() / "Auto Saves";
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	app.call("track.add", {{"type", "audio"}});
	std::ofstream(autoSaves) << "not a folder";
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	const std::string first = errorMessage(app);

	// An auto save that fails for another reason while the error is open: the old error closes for the new one, which
	// says its own reason.
	fs::remove(autoSaves);
	blockSaving(autoSaves / "Song-01.bak");
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app, &first] { return errorMessage(app) != first && !errorMessage(app).empty(); }));
	CHECK_THAT(errorMessage(app), ContainsSubstring("auto saved"));
	CHECK(elements(app, "#project-error-dialog").size() == 1);
	CHECK(shown(app, "#project-error-dialog"));

	// A save that fails meanwhile: its error takes the auto save's error's place.
	app.keys().run("file.saveAs", Json::object(), app.main(), nullptr);
	answer(app, {notFolder / "Song.dawproject"});
	REQUIRE(app.waitUntil([&app]
		{ return shown(app, "#project-error-dialog") && errorMessage(app).find("auto saved") == std::string::npos; }));
	CHECK(elements(app, "#project-error-dialog").size() == 1);
	closeError(app);

	// The auto save's error shows again at the next failure, although that is for the same reason.
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK_THAT(errorMessage(app), ContainsSubstring("auto saved"));
	closeError(app);
}

TEST_CASE("a failed auto save's error waits while other modal dialogs are open, and shows once they closed", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path autoSaves = dir.path() / "Auto Saves";
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	app.call("track.add", {{"type", "audio"}});
	std::ofstream(autoSaves) << "not a folder";

	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list .track-name").size() == 1; }));

	// The auto fades dialog and the studio setup over it are modal dialogs of the main window that aren't project
	// dialogs.
	app.keys().run("project.autoFades", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#auto-fades"); }));
	app.keys().run("window.studioSetup", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#studio-setup"); }));
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	CHECK(elements(app, "#project-error-dialog").empty());
	CHECK(shown(app, "#auto-fades"));

	// The studio setup closes, the auto fades dialog is still open: the error waits for it.
	click(app, "#studio-setup #cancel");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#studio-setup"); }));
	handleEarlierEvents(app);
	CHECK(elements(app, "#project-error-dialog").empty());

	// Cancel closes the last one, and the error shows without another failure.
	click(app, "#auto-fades-cancel");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#auto-fades").empty(); }));
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK_THAT(errorMessage(app), ContainsSubstring("auto saved"));
	closeError(app);

	// Once per reason: an auto save that fails for the same reason while a dialog is open doesn't show it again.
	app.keys().run("project.autoFades", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#auto-fades"); }));
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	click(app, "#auto-fades-cancel");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#auto-fades").empty(); }));
	handleEarlierEvents(app);
	CHECK(elements(app, "#project-error-dialog").empty());
}

TEST_CASE("a failed auto save's error that waits for a dialog is dropped once auto save works or the project was saved "
		  "or opened",
	"[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	Client client(app.engine());
	AutoSaveEvents events(app.engine());
	const test::TempDir dir;
	const fs::path autoSaves = dir.path() / "Auto Saves";
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	app.call("track.add", {{"type", "audio"}});
	std::ofstream(autoSaves) << "not a folder";
	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list .track-name").size() == 1; }));

	const auto lastEvent = [&events, &client]
	{
		const std::vector<std::string> names = events.names(client);
		return names.empty() ? std::string() : names.back();
	};
	const auto openAutoFades = [&app]
	{
		app.keys().run("project.autoFades", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return shown(app, "#auto-fades"); }));
	};
	const auto failWhileOpen = [&app, &openAutoFades, &lastEvent]
	{
		openAutoFades();
		app.engine().advanceClock(g_pastInterval);
		CHECK(lastEvent() == "project.autoSaveFailed");
		handleEarlierEvents(app);
		CHECK(elements(app, "#project-error-dialog").empty());
	};
	const auto closeAndCheck = [&app]
	{
		click(app, "#auto-fades-cancel");
		REQUIRE(app.waitUntil([&app] { return elements(app, "#auto-fades").empty(); }));
		handleEarlierEvents(app);
		CHECK(elements(app, "#project-error-dialog").empty());
	};

	// The next auto save succeeds while the dialog is open: the error is out of date when it closes.
	failWhileOpen();
	fs::remove(autoSaves);
	app.engine().advanceClock(g_pastInterval);
	CHECK(lastEvent() == "project.autoSaved");
	handleEarlierEvents(app);
	closeAndCheck();

	// The project is saved while the dialog is open.
	blockSaving(autoSaves / "Song-02.bak");
	failWhileOpen();
	const size_t saved = elements(app, "#track-list .track-name").size();
	app.call("project.save");
	handleEarlierEvents(app);
	closeAndCheck();

	// An auto save fails and the next succeeds before the UI hears of either.
	openAutoFades();
	CHECK(!client.call("project.autoSave").ok());
	fs::remove_all(fs::u8path((autoSaves / "Song-02.bak").u8string() + ".saving"));
	CHECK(client.call("project.autoSave").ok());
	handleEarlierEvents(app);
	CHECK(elements(app, "#project-error-dialog").empty());
	closeAndCheck();

	// The project is opened again while the dialog is open: File > Revert.
	blockSaving(autoSaves / "Song-03.bak");
	failWhileOpen();
	app.call("project.revert");
	REQUIRE(app.waitUntil([&app, saved] { return elements(app, "#track-list .track-name").size() == saved; }));
	closeAndCheck();
}

TEST_CASE("a modal dialog in a panel's own window doesn't keep a failed auto save's error away", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path autoSaves = dir.path() / "Auto Saves";
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	app.call("track.add", {{"type", "audio"}});
	std::ofstream(autoSaves) << "not a folder";
	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list .track-name").size() == 1; }));

	app.appUi().docking().setFloating("history", true);
	const gnubase::ui::Window* floating = app.appUi().docking().windowOf("history");
	REQUIRE(floating != nullptr);
	const gnubase::ui::WindowId panel = floating->id();
	gnubase::ui::Window* window = app.shell().find(panel);
	REQUIRE(window != nullptr);
	app.keys().run("project.autoFades", Json::object(), *window, nullptr);
	const auto autoFadesInPanel = [&app, panel]
	{
		const Json found = app.ui().dom(panel, "#auto-fades", 0).at("elements");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	};
	REQUIRE(app.waitUntil(autoFadesInPanel));

	// The error opens in the main window, whose documents have no other modal dialog.
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK_THAT(errorMessage(app), ContainsSubstring("auto saved"));
	CHECK(autoFadesInPanel());
	closeError(app);
}

TEST_CASE("File > Open and Revert ask before unsaved changes are discarded", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	app.call("track.add", {{"type", "audio"}});
	app.call("project.save", {{"path", utf8(file)}});
	app.call("project.new");
	app.call("track.add", {{"type", "midi"}});
	app.call("track.add", {{"type", "midi"}});

	fileMenu(app, "file.open");
	CHECK(answer(app, {file}).kind == gnubase::ui::FileDialogKind::Open);
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	CHECK(app.call("project.get").at("tracks").size() == 2); // kept

	fileMenu(app, "file.open");
	answer(app, {file});
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 1; }));
	CHECK(location(app) == utf8(fs::absolute(file)));

	// Revert: without changes at once, with changes once the user agrees.
	app.call("track.add", {{"type", "audio"}});
	REQUIRE(app.waitUntil([&app] { return !disabled(app, "menuitem[command=\"file.revert\"]"); }));
	fileMenu(app, "file.revert");
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 1; }));

	app.call("project.new");
	REQUIRE(app.waitUntil([&app] { return disabled(app, "menuitem[command=\"file.revert\"]"); })); // no file

	// An auto save opens untitled: there is nothing to revert to, and Save asks where instead of overwriting it.
	app.call("project.load", {{"path", utf8(file)}});
	REQUIRE(app.waitUntil([&app] { return !disabled(app, "menuitem[command=\"file.revert\"]"); }));
	app.call("track.add", {{"type", "audio"}});
	const fs::path autoSave = fs::u8path(app.call("project.autoSave").at("file").get<std::string>());
	const fs::file_time_type autoSaved = fs::last_write_time(autoSave);
	fileMenu(app, "file.open");
	answer(app, {autoSave});
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return disabled(app, "menuitem[command=\"file.revert\"]"); }));
	CHECK(location(app).is_null());
	fileMenu(app, "file.save");
	CHECK(answer(app, {}).kind == gnubase::ui::FileDialogKind::Save);
	CHECK(location(app).is_null());
	// Compared into a bool: Catch2 can't print a file time where libc++ counts it in __int128 (macOS).
	const bool autoSaveUntouched = fs::last_write_time(autoSave) == autoSaved;
	CHECK(autoSaveUntouched);
}

TEST_CASE("closing the main window or quitting through the OS asks before unsaved changes are discarded", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	// The main window's close button or Alt+F4, or SDL's quit request, as from the macOS app menu's Quit.
	SDL_Event request{};
	request.type = GENERATE(as<Uint32>(), SDL_EVENT_WINDOW_CLOSE_REQUESTED, SDL_EVENT_QUIT);
	request.window.windowID = app.main().id();

	SECTION("without unsaved changes it quits at once")
	{
		REQUIRE(SDL_PushEvent(&request));
		REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
	}
	SECTION("with unsaved changes Cancel keeps GNUbase running, and Discard quits")
	{
		app.call("track.add", {{"type", "audio"}});
		REQUIRE(SDL_PushEvent(&request));
		REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
		click(app, "#discard-changes-dialog button[result=cancel]");
		REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(app.shell().running());
		CHECK(app.call("project.get").at("tracks").size() == 1); // kept
		CHECK(app.call("project.revision").at("modified").get<bool>());

		REQUIRE(SDL_PushEvent(&request));
		click(app, "#discard-changes");
		REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
	}
	SECTION("a dialog that is open closes for the question")
	{
		app.call("track.add", {{"type", "audio"}});
		fileMenu(app, "file.new");
		REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
		REQUIRE(SDL_PushEvent(&request));
		click(app, "#discard-changes");
		REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
	}
}

TEST_CASE(
	"closing the last window asks one question for SDL's close and quit requests, and for requests after them", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});

	closeLastWindow(app);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	Rml::Element* first = question(app);
	const Rml::ObserverPtr<Rml::Element> asked =
		first != nullptr ? first->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
	REQUIRE(asked.get() != nullptr);
	// Asking again while the question is open, by closing the window or through MCP's ui_close, keeps it as it is.
	closeLastWindow(app);
	app.ui().close(std::nullopt, true);
	app.ui().close(std::nullopt, false);
	app.call("project.revision"); // after the engine answered whatever these asked
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#discard-changes-dialog").size() == 1);
	CHECK(asked.get() != nullptr);
	CHECK(asked.get() == question(app));

	// Cancel keeps GNUbase running with the changes.
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(app.shell().running());
	CHECK(question(app) == nullptr);
	CHECK(app.call("project.get").at("tracks").size() == 1);
	CHECK(app.call("project.revision").at("modified").get<bool>());

	// The next request asks again, and Discard quits.
	closeLastWindow(app);
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE("a quit question cancelled before the engine answered its second look lets the next quit request ask", "[ui]")
{
	test::CommandHolds holds; // declared before the app: its engine's command thread waits in hold()
	test::UiApp app({900, 640});
	const test::ReleaseHolds releaseHolds{holds};
	app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", CommandInfo{}.parameters, Json::object(), false},
		[&holds](CommandContext&) { return holds.hold(); });
	Client holder(app.engine(), "holder");
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	gnubase::ui::EngineBinding& binding = app.appUi().binding();

	// The engine answers whether to ask, and then holds: the question opens, and the engine answers its second look
	// at project.revision only once the test lets it go, as on a busy machine.
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	app.ui().close(std::nullopt, false);
	REQUIRE(app.waitUntil([&binding] { return binding.waiting("project.revision") != 0; }));
	holder.send("test.hold");
	holds.release(1);
	REQUIRE(holds.waitEntered(2));
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));

	// Cancel before the engine answered, and quit again, as dialog.lua's test does at once (#1454): the request asks
	// again instead of finding the question still coming.
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	app.ui().close(std::nullopt, true);
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	holds.release(2);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE("a quit question that goes with its window doesn't keep the next quit request from asking", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.call("track.add", {{"type", "audio"}});
	// File > Quit's key in a panel's own window asks there.
	app.appUi().docking().setFloating("history", true);
	const gnubase::ui::Window* floating = app.appUi().docking().windowOf("history");
	REQUIRE(floating != nullptr);
	const gnubase::ui::WindowId panel = floating->id();
	gnubase::ui::Window* window = app.shell().find(panel);
	REQUIRE(window != nullptr);
	app.keys().run("file.quit", Json::object(), *window, nullptr);
	REQUIRE(app.waitUntil(
		[&app, panel] { return app.ui().dom(panel, "#discard-changes-dialog", 0).at("elements").size() == 1; }));

	// Closing the window, which docks the panel again, takes the question along unanswered.
	app.ui().close(panel, false);
	REQUIRE(app.waitUntil([&app, panel] { return app.shell().find(panel) == nullptr; }));
	REQUIRE(app.shell().running());

	closeLastWindow(app);
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE("a failed auto save's error makes way for the quit question, and waits while the question is open", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path autoSaves = dir.path() / "Auto Saves";
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	app.call("track.add", {{"type", "audio"}});
	std::ofstream(autoSaves) << "not a folder";
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));

	// A quit request closes the error for its question.
	closeLastWindow(app);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	CHECK(elements(app, "#project-error-dialog").empty());
	Rml::Element* first = question(app);
	const Rml::ObserverPtr<Rml::Element> asked =
		first != nullptr ? first->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
	REQUIRE(asked.get() != nullptr);

	// An auto save that fails for another reason while the question is open leaves it open, and so does another quit
	// request.
	fs::remove(autoSaves);
	blockSaving(autoSaves / "Song-01.bak");
	app.engine().advanceClock(g_pastInterval);
	handleEarlierEvents(app);
	closeLastWindow(app);
	app.call("project.revision");
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#project-error-dialog").empty());
	CHECK(elements(app, "#discard-changes-dialog").size() == 1);
	CHECK(asked.get() == question(app));

	// Cancel keeps GNUbase running; the error shows at the next failure, and the next quit request asks again.
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	REQUIRE(app.shell().running());
	app.engine().advanceClock(g_pastInterval);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	closeLastWindow(app);
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE("the unsaved changes question and a failed command's error wait while another project dialog is open", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	app.call("project.save", {{"path", utf8(file)}});
	app.call("track.add", {{"type", "audio"}});

	// File > Open through its key in another window, or MCP, while the start screen is open: the question waits for
	// it instead of being dropped with the project that was to open.
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
	app.keys().run("file.open", Json::object(), app.main(), nullptr);
	answer(app, {file});
	app.call("project.revision"); // after the engine answered what Open asked
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#discard-changes-dialog").empty());
	click(app, "#start-screen button[result=cancel]");
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").empty(); }));

	// A save that fails while Save as Template is open shows its error once the dialog closed.
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";
	fileMenu(app, "file.saveTemplate");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#save-template-dialog"); }));
	app.keys().run("file.saveAs", Json::object(), app.main(), nullptr);
	answer(app, {notFolder / "Other.dawproject"});
	app.call("project.revision");
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#project-error-dialog").empty());
	click(app, "#save-template-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	closeError(app);
	CHECK(location(app) == utf8(fs::absolute(file)));
}

namespace
{
	// The history panel in a window of its own, as the tests around here take a panel's own window.
	gnubase::ui::Window& panelWindow(test::UiApp& app)
	{
		app.appUi().docking().setFloating("history", true);
		const gnubase::ui::Window* floating = app.appUi().docking().windowOf("history");
		REQUIRE(floating != nullptr);
		gnubase::ui::Window* window = app.shell().find(floating->id());
		REQUIRE(window != nullptr);
		return *window;
	}

	// Lets the shell run what it posted and the engine answered before, as the waiting tests above do.
	void settle(test::UiApp& app)
	{
		app.call("project.revision"); // after the engine answered whatever the UI asked
		for (int i = 0; i < 5; ++i)
			app.shell().step(std::chrono::milliseconds(0));
	}
}

TEST_CASE("a quit request drops the questions and start screens that wait, so that its question is next", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	app.call("project.save", {{"path", utf8(file)}});
	app.call("track.add", {{"type", "audio"}});

	// File > Open's question waits while the start screen is open.
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
	app.keys().run("file.open", Json::object(), app.main(), nullptr);
	answer(app, {file});
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());

	// The quit request closes the start screen and drops the question that waits: its own question opens, and one
	// Discard quits instead of opening the project the dropped question was about and asking again.
	closeLastWindow(app);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE("an error whose window closed while it waited shows in the main window", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";

	// A save asked for in a panel's own window fails while the start screen is open: its error waits for it.
	gnubase::ui::Window& panel = panelWindow(app);
	const gnubase::ui::WindowId panelId = panel.id();
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
	app.keys().run("file.saveAs", Json::object(), panel, nullptr);
	answer(app, {notFolder / "Song.dawproject"});
	settle(app);
	CHECK(elements(app, "#project-error-dialog").empty());

	// Closing the panel window takes the error's window along before the start screen closes: the error shows in
	// the main window instead of only being logged, so that the failed save isn't taken for done.
	app.ui().close(panelId, false);
	REQUIRE(app.waitUntil([&app, panelId] { return app.shell().find(panelId) == nullptr; }));
	click(app, "#start-screen button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK(!errorMessage(app).empty());
	closeError(app);
}

TEST_CASE("a question that waited doesn't ask once the project was saved meanwhile", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path other = dir.path() / "Other.dawproject";
	app.call("track.add", {{"type", "audio"}});
	app.call("project.save", {{"path", utf8(other)}}); // one track
	app.call("project.new");
	app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "audio"}});

	// File > Open's question waits while the start screen is open.
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
	app.keys().run("file.open", Json::object(), app.main(), nullptr);
	answer(app, {other});
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());

	// The project is saved while the question waits: once the start screen closes, there is nothing left to
	// discard, so the project opens without the question.
	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	click(app, "#start-screen button[result=cancel]");
	REQUIRE(app.waitUntil([&app, &other] { return location(app) == utf8(fs::absolute(other)); }));
	CHECK(app.call("project.get").at("tracks").size() == 1);
	CHECK(!shown(app, "#discard-changes-dialog"));
}

TEST_CASE("a quit question that waits and goes with its window lets the next quit request ask again", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";
	app.call("track.add", {{"type", "audio"}});

	// A save that fails leaves its error waiting while Save as Template is open.
	fileMenu(app, "file.saveTemplate");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#save-template-dialog"); }));
	app.keys().run("file.saveAs", Json::object(), app.main(), nullptr);
	answer(app, {notFolder / "Song.dawproject"});
	settle(app);
	CHECK(elements(app, "#project-error-dialog").empty());

	// Quit in a panel's own window: the error stays and opens as Save as Template closes, and the question waits
	// behind it for the panel's window.
	gnubase::ui::Window& panel = panelWindow(app);
	const gnubase::ui::WindowId panelId = panel.id();
	app.keys().run("file.quit", Json::object(), panel, nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());

	// Closing the panel window takes the question that waits along: it is skipped, which leaves GNUbase running
	// and lets the next quit request ask again.
	app.ui().close(panelId, false);
	REQUIRE(app.waitUntil([&app, panelId] { return app.shell().find(panelId) == nullptr; }));
	closeError(app);
	settle(app);
	REQUIRE(app.shell().running());
	CHECK(elements(app, "#discard-changes-dialog").empty());

	closeLastWindow(app);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE(
	"a quit question that opens after a newer question came still answers, so Cancel leaves quitting working", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	app.call("project.save", {{"path", utf8(file)}});
	app.call("track.add", {{"type", "audio"}});
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";

	// A failed save's error waits while Save as Template is open.
	fileMenu(app, "file.saveTemplate");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#save-template-dialog"); }));
	app.keys().run("file.saveAs", Json::object(), app.main(), nullptr);
	answer(app, {notFolder / "Other.dawproject"});
	settle(app);
	CHECK(elements(app, "#project-error-dialog").empty());

	// Quit: Save as Template closes for the error, and the quit question waits behind the error.
	app.keys().run("file.quit", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());

	// File > Open comes before the error is dismissed: its question is the newer one, waiting behind the quit's.
	app.keys().run("file.open", Json::object(), app.main(), nullptr);
	answer(app, {file});
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());

	// The error is dismissed and the quit question opens, although a newer question came: its asked runs, so that
	// Cancel doesn't leave GNUbase unable to quit.
	closeError(app);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	settle(app); // the question's second look at project.revision answered by now
	Rml::Element* first = question(app);
	const Rml::ObserverPtr<Rml::Element> quitQuestion =
		first != nullptr ? first->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
	REQUIRE(quitQuestion.get() != nullptr);
	click(app, "#discard-changes-dialog button[result=cancel]");

	// The newer question opens in its turn, another document; Cancel answers it too.
	REQUIRE(app.waitUntil(
		[&app, &quitQuestion] { return question(app) != nullptr && question(app) != quitQuestion.get(); }));
	settle(app);
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	REQUIRE(app.shell().running());

	// The next quit request asks again, and Discard quits.
	closeLastWindow(app);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

TEST_CASE("a dialog that goes with its window lets the next one open", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	app.call("project.save", {{"path", utf8(file)}});
	app.call("track.add", {{"type", "audio"}});

	// The start screen in a panel's own window keeps File > Open's question in the main window waiting.
	gnubase::ui::Window& panel = panelWindow(app);
	const gnubase::ui::WindowId panelId = panel.id();
	app.keys().run("file.new", Json::object(), panel, nullptr);
	REQUIRE(app.waitUntil(
		[&app, panelId] { return app.ui().dom(panelId, "#start-screen", 0).at("elements").size() == 1; }));
	app.keys().run("file.open", Json::object(), app.main(), nullptr);
	answer(app, {file});
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());

	// Closing the panel window takes the start screen along: the question opens in the main window.
	app.ui().close(panelId, false);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").empty(); }));
}

TEST_CASE("the dialogs that wait open in the order they came", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Song.dawproject";
	app.call("project.save", {{"path", utf8(file)}});
	app.call("track.add", {{"type", "audio"}});
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";

	// While the start screen is open, File > Open's question comes first and a failed save's error second.
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return shown(app, "#start-screen"); }));
	app.keys().run("file.open", Json::object(), app.main(), nullptr);
	answer(app, {file});
	app.keys().run("file.saveAs", Json::object(), app.main(), nullptr);
	answer(app, {notFolder / "Other.dawproject"});
	settle(app);
	CHECK(elements(app, "#discard-changes-dialog").empty());
	CHECK(elements(app, "#project-error-dialog").empty());

	// Once the start screen closes, the question opens first, and the error follows it.
	click(app, "#start-screen button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	CHECK(elements(app, "#project-error-dialog").empty());
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	closeError(app);
}

TEST_CASE("the start screen creates projects, from templates too, and opens recent ones", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path file = dir.path() / "Recent.dawproject";
	app.call("track.add", {{"type", "audio"}});
	app.call("project.save", {{"path", utf8(file)}});
	app.call("track.add", {{"type", "audio"}});
	app.call("project.saveTemplate", {{"name", "Two Tracks"}});
	app.call("project.new");

	// File > New Project opens it; a template selected and Create.
	fileMenu(app, "file.new");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#start-templates listitem").size() == 1; }));
	CHECK(elements(app, "#start-templates listitem")[0].at("text") == "Two Tracks");
	CHECK(elements(app, "#start-recent listitem")[0].at("text") == "Recent");
	click(app, "#start-templates listitem");
	click(app, "#create-template");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 2; }));
	CHECK(location(app).is_null());
	CHECK(!shown(app, "#start-screen"));

	// A double click on a recent project opens it, after asking about the unsaved changes.
	app.call("track.add", {{"type", "audio"}});
	app.appUi().openStartScreen();
	REQUIRE(app.waitUntil([&app] { return elements(app, "#start-recent listitem").size() == 1; }));
	click(app, "#start-recent listitem", 2);
	click(app, "#discard-changes");
	REQUIRE(app.waitUntil([&app, &file] { return location(app) == utf8(fs::absolute(file)); }));
	CHECK(app.call("project.get").at("tracks").size() == 1);

	app.appUi().openStartScreen();
	click(app, "#create-empty");
	REQUIRE(app.waitUntil([&app] { return location(app).is_null(); }));
	CHECK(app.call("project.get").at("tracks").empty());

	app.appUi().openStartScreen();
	click(app, "#open-other");
	CHECK(answer(app, {}).kind == gnubase::ui::FileDialogKind::Open);
}

TEST_CASE("File > Save as Template and Back up Project", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	app.call("track.add", {{"type", "audio"}});

	fileMenu(app, "file.saveTemplate");
	click(app, "#template-name");
	app.ui().text(std::nullopt, "Drums");
	click(app, "#save-template-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.templates").at("templates").size() == 1; }));
	CHECK(app.call("project.templates").at("templates")[0].at("name") == "Drums");

	const std::string song = utf8(dir.path() / "Song.dawproject");
	app.call("project.save", {{"path", song}});
	test::writeWav(dir.path() / "Mixdown" / "Song.wav", {std::vector<float>(480, 0.25f), 1, 48000});

	// Back up Project asks for the folder, then shows its options: the project file's name, and the project stays.
	fileMenu(app, "file.backup");
	CHECK(answer(app, {dir.path() / "Backup"}).kind == gnubase::ui::FileDialogKind::Folder);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-project-dialog"); }));
	CHECK(elements(app, "#backup-name").at(0).at("attributes").at("value") == "Song");
	CHECK(elements(app, "#backup-keep-active").at(0).at("attributes").contains("checked"));
	for (const char* option :
		{"#backup-minimize", "#backup-offline", "#backup-remove-unused", "#backup-skip-video", "#backup-skip-mixdown"})
		CHECK(!elements(app, option).at(0).at("attributes").contains("checked"));
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&dir] { return fs::exists(dir.path() / "Backup" / "Song.dawproject"); }));
	CHECK(fs::exists(dir.path() / "Backup" / "Mixdown" / "Song.wav"));
	CHECK(location(app) == song);
	// Its progress dialog closes once it's done.
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-progress-dialog"); }));
	CHECK(!shown(app, "#project-error-dialog"));

	// Cancelled, it backs nothing up.
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Cancelled"});
	click(app, "#backup-project-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-project-dialog"); }));
	CHECK(!fs::exists(dir.path() / "Cancelled"));

	// Another name, without the Mixdown folder, and the backup goes on as the project.
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Final"});
	click(app, "#backup-name");
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Final Mix");
	click(app, "#backup-keep-active");
	click(app, "#backup-skip-mixdown");
	click(app, "#backup-project-dialog button.primary");
	const std::string renamed = utf8(dir.path() / "Final" / "Final Mix.dawproject");
	REQUIRE(app.waitUntil([&app, &renamed] { return location(app) == renamed; }, std::chrono::seconds(30)));
	CHECK(!fs::exists(dir.path() / "Final" / "Mixdown"));
	CHECK(app.call("project.get").at("name") == "Final Mix");
}

namespace
{
	// The parameters of the last project.backup that ran.
	Json lastBackup(test::UiApp& app)
	{
		Json parameters;
		const Json log = app.call("engine.commandLog");
		for (const Json& entry : log.at("commands"))
			if (entry.at("command") == "project.backup")
				parameters = entry.at("parameters");
		return parameters;
	}

	// Waits until no job runs, and returns the last one.
	Json lastJob(test::UiApp& app)
	{
		Json jobs;
		REQUIRE(app.waitUntil(
			[&app, &jobs]
			{
				jobs = app.call("job.list").at("jobs");
				return std::none_of(
					jobs.begin(), jobs.end(), [](const Json& job) { return job.at("state") == "running"; });
			},
			std::chrono::seconds(60)));
		REQUIRE(!jobs.empty());
		return jobs.back();
	}

	// Whether command ran since the engine started; only commands that aren't queries count.
	bool ran(test::UiApp& app, const std::string& command)
	{
		const Json log = app.call("engine.commandLog").at("commands");
		return std::any_of(
			log.begin(), log.end(), [&command](const Json& entry) { return entry.at("command") == command; });
	}
}

TEST_CASE("Back up Project shows its own backups' progress and outcome, and Cancel stops them", "[ui][slow]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;

	// A project without a file: the name starts as the project's, after which the engine names the backup. Options
	// that have no effect yet can't be chosen; Minimize Audio Files and Remove Unused Files reach the engine.
	app.call("project.set", {{"name", "My Song"}});
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Unsaved"});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-project-dialog"); }));
	CHECK(elements(app, "#backup-name").at(0).at("attributes").at("value") == "My Song");
	CHECK(disabled(app, "#backup-offline"));
	CHECK(disabled(app, "#backup-skip-video"));
	CHECK(!disabled(app, "#backup-minimize"));
	click(app, "#backup-minimize");
	click(app, "#backup-remove-unused");
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&dir] { return fs::exists(dir.path() / "Unsaved" / "My Song.dawproject"); }));
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-progress-dialog"); }));
	const Json parameters = lastBackup(app);
	CHECK(parameters.at("minimize") == true);
	CHECK(parameters.at("removeUnused") == true);
	CHECK(!parameters.contains("name"));
	CHECK(!shown(app, "#project-error-dialog"));

	// Done, a backup with files that couldn't be read says which, also two of the same name (#1001).
	for (const fs::path& gone : {dir.path() / "gone.wav", dir.path() / "elsewhere" / "gone.wav"})
		app.call("media.add", {{"path", utf8(gone)}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 48000}});
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Missing"});
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK(errorMessage(app) == "The project was backed up without 2 files that couldn't be read: gone.wav, gone.wav.");
	CHECK(!shown(app, "#backup-progress-dialog"));
	closeError(app);
	CHECK(fs::exists(dir.path() / "Missing" / "My Song.dawproject"));

	// A running one shows its progress, and Cancel in there stops it: it leaves nothing behind and says nothing more.
	// It is held at its first progress until job.cancel ran, so that it can't end first (#1015). Had it ended, it would
	// say that it couldn't read gone.wav, rightly: the backup is there.
	const fs::path take = dir.path() / "take.wav";
	test::writeWav(take, {std::vector<float>(size_t{48000} * 10 * 2, 0.25f), 2, 48000}); // 10 s, 4 MB
	app.call("media.add", {{"path", utf8(take)}});
	test::HeldJob held(app.engine(), "project.backup");
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Cancelled"});
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&held] { return held.held(); }, std::chrono::seconds(30)));
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-progress-dialog"); }));
	CHECK(elements(app, "#backup-progress").size() == 1);
	click(app, "#backup-progress-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return ran(app, "job.cancel"); }));
	CHECK(held.release());
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-progress-dialog"); }));
	const Json cancelled = lastJob(app);
	CHECK(cancelled.at("kind") == "project.backup");
	CHECK(cancelled.at("state") == "cancelled");
	CHECK(!fs::exists(dir.path() / "Cancelled"));
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));

	// A backup that another client starts, such as an MCP client or a Lua script, shows nothing here, also when it
	// fails (#944).
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";
	app.call("project.backup", {{"folder", utf8(notFolder / "Backup")}});
	CHECK(lastJob(app).at("state") == "failed");
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));
	CHECK(!shown(app, "#backup-progress-dialog"));
}

TEST_CASE(
	"a running backup's progress keeps no other project dialog waiting, and a quit that is cancelled leaves it running",
	"[ui][slow]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	app.call("track.add", {{"type", "audio"}}); // an unsaved change, so that quitting asks
	const fs::path take = dir.path() / "take.wav";
	test::writeWav(take, {std::vector<float>(size_t{48000} * 10 * 2, 0.25f), 2, 48000}); // 10 s, 4 MB
	app.call("media.add", {{"path", utf8(take)}});
	test::HeldJob held(app.engine(), "project.backup");
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Backup"});
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&held] { return held.held(); }, std::chrono::seconds(30)));
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-progress-dialog"); }));

	// Another project dialog opens while the progress shows, such as another backup's options (#1001).
	app.keys().run("file.backup", Json::object(), app.main(), nullptr);
	answer(app, {dir.path() / "Other"});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-project-dialog"); }));
	click(app, "#backup-project-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-project-dialog"); }));
	CHECK(shown(app, "#backup-progress-dialog"));

	// Quitting asks about the unsaved changes without cancelling the backup first: cancelled, it leaves it running.
	app.keys().run("file.quit", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#discard-changes-dialog"); }));
	click(app, "#discard-changes-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#discard-changes-dialog"); }));
	REQUIRE(app.shell().running());
	CHECK(shown(app, "#backup-progress-dialog"));
	CHECK(!ran(app, "job.cancel"));
	CHECK(held.release());
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-progress-dialog"); }));
	CHECK(lastJob(app).at("state") == "done");
	CHECK(!shown(app, "#project-error-dialog"));
}

TEST_CASE("a backup that couldn't read a folder in the Mixdown folder counts it as a folder", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path project = dir.path() / "Song";
	fs::create_directories(project / "Mixdown" / "Hidden");
	app.call("project.save", {{"path", utf8(project / "Song.dawproject")}});
	const test::Unreadable hidden(project / "Mixdown" / "Hidden");
	if (!hidden.active())
		SKIP("the folder can be listed here anyway, e.g. by root");
	app.call("media.add",
		{{"path", utf8(dir.path() / "gone.wav")}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 48000}});

	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "Backup"});
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK(errorMessage(app) ==
		"The project was backed up without 1 file and 1 folder that couldn't be read: gone.wav, Hidden.");
	closeError(app);
}

TEST_CASE("a backup started in another window while one's progress shows shows its own progress there", "[ui][slow]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path take = dir.path() / "take.wav";
	test::writeWav(take, {std::vector<float>(size_t{48000} * 10 * 2, 0.25f), 2, 48000}); // 10 s, 4 MB
	app.call("media.add", {{"path", utf8(take)}});
	// A panel in a window of its own.
	app.appUi().docking().setFloating("history", true);
	const gnubase::ui::Window* floating = app.appUi().docking().windowOf("history");
	REQUIRE(floating != nullptr);
	const gnubase::ui::WindowId panel = floating->id();
	gnubase::ui::Window* window = app.shell().find(panel);
	REQUIRE(window != nullptr);
	// Both backups are held at their first progress until the second one was cancelled, so that the first can't end
	// before and the second still runs then. Only the jobs wait: the commands that the UI sends meanwhile, such as
	// the window layout that docking saves, go on (#1272).
	test::HeldJob held(app.engine(), "project.backup", std::chrono::minutes(1), 2);
	fileMenu(app, "file.backup");
	answer(app, {dir.path() / "First"});
	click(app, "#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&held] { return held.job() != 0; }, std::chrono::seconds(30)));
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-progress-dialog"); }));

	// File > Back up Project in the panel's own window, while the first backup's progress shows in the main window
	// (#1109).
	const auto shownInPanel = [&app, panel](const std::string& selector)
	{
		const Json found = app.ui().dom(panel, selector, 0).at("elements");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	};
	const auto clickInPanel = [&app, panel, &shownInPanel](const std::string& selector)
	{
		REQUIRE(app.waitUntil([&shownInPanel, &selector] { return shownInPanel(selector); }));
		app.ui().click(panel, {selector, 0, std::nullopt}, gnubase::ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};
	app.keys().run("file.backup", Json::object(), *window, nullptr);
	answer(app, {dir.path() / "Second"});
	clickInPanel("#backup-project-dialog button.primary");
	REQUIRE(app.waitUntil([&held] { return held.held(); }, std::chrono::seconds(30)));
	// A command that sends an event, as docking's layout saves do, isn't held behind the backups.
	app.call("settings.set", {{"key", "ui.removeTracksWarning"}, {"value", false}});

	// Each window shows its own backup's progress, and the second's Cancel stops only that one.
	REQUIRE(app.waitUntil([&shownInPanel] { return shownInPanel("#backup-progress-dialog"); }));
	CHECK(shown(app, "#backup-progress-dialog"));
	clickInPanel("#backup-progress-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return ran(app, "job.cancel"); }));
	CHECK(!shownInPanel("#backup-progress-dialog"));
	CHECK(shown(app, "#backup-progress-dialog"));
	{
		INFO("a backup's hold lapsed before the second backup was cancelled"); // #1339
		REQUIRE(held.release());
	}
	// The first backup's whole run, as long as lastJob waits on a busy machine.
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#backup-progress-dialog"); }, std::chrono::seconds(60)));
	lastJob(app); // once none runs
	const Json jobs = app.call("job.list").at("jobs");
	std::vector<std::string> states;
	for (const Json& job : jobs)
		if (job.at("kind") == "project.backup")
			states.push_back(job.at("state").get<std::string>());
	CHECK(states == std::vector<std::string>{"done", "cancelled"});
	CHECK(fs::exists(dir.path() / "First" / "Audio" / "take.wav"));
	CHECK(!fs::exists(dir.path() / "Second"));
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));
}

TEST_CASE("a backup whose progress's window closes is cancelled, and another one from there reports in the main window",
	"[ui][slow]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path take = dir.path() / "take.wav";
	test::writeWav(take, {std::vector<float>(size_t{48000} * 10 * 2, 0.25f), 2, 48000}); // 10 s, 4 MB
	app.call("media.add", {{"path", utf8(take)}});
	app.call("media.add",
		{{"path", utf8(dir.path() / "gone.wav")}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 48000}});
	app.appUi().docking().setFloating("history", true);
	const gnubase::ui::Window* floating = app.appUi().docking().windowOf("history");
	REQUIRE(floating != nullptr);
	const gnubase::ui::WindowId panel = floating->id();
	gnubase::ui::Window* window = app.shell().find(panel);
	REQUIRE(window != nullptr);
	const auto shownInPanel = [&app, panel](const std::string& selector)
	{
		const Json found = app.ui().dom(panel, selector, 0).at("elements");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	};
	const auto backUpInPanel = [&app, &shownInPanel, window, panel](const fs::path& folder)
	{
		app.keys().run("file.backup", Json::object(), *window, nullptr);
		answer(app, {folder});
		REQUIRE(app.waitUntil([&shownInPanel] { return shownInPanel("#backup-project-dialog button.primary"); }));
		app.ui().click(
			panel, {"#backup-project-dialog button.primary", 0, std::nullopt}, gnubase::ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};

	// Two backups asked for in the panel's own window: the first shows its progress there, the second runs without
	// (#1109). Both are held at their first progress until the window closed, so that neither can end first.
	test::HeldJob held(app.engine(), "project.backup", std::chrono::minutes(1), 2);
	backUpInPanel(dir.path() / "First");
	REQUIRE(app.waitUntil([&held] { return held.job() != 0; }, std::chrono::seconds(30)));
	REQUIRE(app.waitUntil([&shownInPanel] { return shownInPanel("#backup-progress-dialog"); }));
	backUpInPanel(dir.path() / "Second");
	REQUIRE(app.waitUntil([&held] { return held.held(); }, std::chrono::seconds(30)));

	// Closing that window, which docks the panel back, takes the progress with it: its backup stops, as its Cancel
	// would, rather than run on unseen. The other one runs on and, its window gone, says in the main window that it
	// couldn't read gone.wav (#1206).
	app.shell().requestClose(panel);
	REQUIRE(app.waitUntil([&app, panel] { return app.shell().find(panel) == nullptr; }));
	REQUIRE(app.waitUntil([&app] { return ran(app, "job.cancel"); }));
	CHECK(held.release());
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }, std::chrono::seconds(60)));
	CHECK(errorMessage(app) == "The project was backed up without 1 file that couldn't be read: gone.wav.");
	closeError(app);
	lastJob(app); // once none runs
	const Json jobs = app.call("job.list").at("jobs");
	std::vector<std::string> states;
	for (const Json& job : jobs)
		if (job.at("kind") == "project.backup")
			states.push_back(job.at("state").get<std::string>());
	CHECK(states == std::vector<std::string>{"cancelled", "done"});
	CHECK(!fs::exists(dir.path() / "First"));
	CHECK(fs::exists(dir.path() / "Second" / "Audio" / "take.wav"));
	CHECK(!shown(app, "#backup-progress-dialog"));
}

TEST_CASE(
	"a backup whose window closed before it started shows its refusal or progress in the main window", "[ui][slow]")
{
	test::CommandHolds holds; // declared before the app: its engine's command thread waits in hold()
	test::UiApp app({1200, 700});
	const test::ReleaseHolds releaseHolds{holds};
	app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", CommandInfo{}.parameters, Json::object(), false},
		[&holds](CommandContext&) { return holds.hold(); });
	Client holder(app.engine(), "holder");
	app.appUi().showProject();
	const test::TempDir dir;
	const fs::path take = dir.path() / "take.wav";
	test::writeWav(take, {std::vector<float>(size_t{48000} * 10 * 2, 0.25f), 2, 48000}); // 10 s, 4 MB
	app.call("media.add", {{"path", utf8(take)}});
	const fs::path notFolder = dir.path() / "file";
	std::ofstream(notFolder) << "not a folder";
	gnubase::ui::EngineBinding& binding = app.appUi().binding();

	// Back up Project in a panel's own window, whose OK the engine answers only once the window closed: the n-th
	// hold keeps project.backup waiting.
	size_t asked = 0;
	const auto backUpInClosingPanel = [&](const fs::path& folder)
	{
		gnubase::ui::Window& window = panelWindow(app);
		const gnubase::ui::WindowId panel = window.id();
		app.keys().run("file.backup", Json::object(), window, nullptr);
		answer(app, {folder});
		REQUIRE(app.waitUntil(
			[&app, panel]
			{
				const Json found = app.ui().dom(panel, "#backup-project-dialog button.primary", 0).at("elements");
				return found.size() == 1 && found[0].at("visible").get<bool>();
			}));
		holder.send("test.hold");
		REQUIRE(holds.waitEntered(++asked));
		app.ui().click(
			panel, {"#backup-project-dialog button.primary", 0, std::nullopt}, gnubase::ui::MouseButton::Left, 1, {});
		REQUIRE(app.waitUntil([&binding] { return binding.waiting("project.backup") != 0; }));
		app.shell().requestClose(panel);
		REQUIRE(app.waitUntil([&app, panel] { return app.shell().find(panel) == nullptr; }));
		holds.release(asked);
	};

	// Refused, it says why in the main window.
	backUpInClosingPanel(notFolder);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	CHECK_THAT(errorMessage(app), ContainsSubstring("is a file"));
	closeError(app);

	// Started, it shows its progress in the main window, whose Cancel stops it, rather than run unseen (#1372). The
	// job is held at its first progress until then, so that it can't end first.
	test::HeldJob held(app.engine(), "project.backup");
	backUpInClosingPanel(dir.path() / "Backup");
	REQUIRE(app.waitUntil([&held] { return held.held(); }, std::chrono::seconds(30)));
	REQUIRE(app.waitUntil([&app] { return shown(app, "#backup-progress-dialog"); }));
	click(app, "#backup-progress-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return ran(app, "job.cancel"); }));
	CHECK(held.release());
	CHECK(lastJob(app).at("state") == "cancelled");
	CHECK(!fs::exists(dir.path() / "Backup"));
	CHECK(!shown(app, "#backup-progress-dialog"));
	handleEarlierEvents(app);
	CHECK(!shown(app, "#project-error-dialog"));
}
