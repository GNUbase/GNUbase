// SPDX-License-Identifier: GPL-3.0-or-later
// Project handling (#77, docs/features/project-files.md): save, save as, save new version, revert, auto save, recent
// projects and templates through the engine API, which MCP and Lua use as they are, and the File menu and the start
// screen in the app's UI.
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "platform/log.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
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

		Json last()
		{
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
	engine.clock().advance(std::chrono::minutes(14));
	CHECK(events.names(client).empty());
	CHECK(autoSaveLog(client).empty()); // checks that save nothing stay out of the log
	engine.clock().advance(std::chrono::minutes(1) + std::chrono::seconds(1));
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaved"});
	CHECK(fs::is_regular_file(autoSave));
	const Json log = autoSaveLog(client);
	REQUIRE(log.size() == 1);
	CHECK(log[0].at("client") == "engine");
	CHECK(log[0].at("parameters") == Json{{"due", true}});
	CHECK(!log[0].contains("error"));
	CHECK(modified(client)); // an auto save isn't a save

	// Nothing changed since: the next interval saves nothing.
	engine.clock().advance(g_pastInterval);
	CHECK(events.names(client).size() == 1);
	CHECK(autoSaveLog(client).size() == 1);

	// Off, nothing is saved however long it waits.
	run(client, "settings.set", {{"key", "project.autoSave"}, {"value", false}});
	run(client, "track.add", {{"type", "audio"}});
	engine.clock().advance(std::chrono::minutes(60));
	CHECK(events.names(client).size() == 1);
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
	engine.clock().advance(g_pastInterval);
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaveFailed"});
	CHECK(events.last().at("file") == utf8(autoSaves / "Song-01.bak"));
	CHECK(!events.last().at("error").get<std::string>().empty());
	Json log = autoSaveLog(client);
	REQUIRE(log.size() == 1);
	CHECK(log[0].contains("error"));
	CHECK(loggedFailures(logFile) == 1);

	// Not tried again every 30 seconds but after the interval, and only the first failure is in the log.
	engine.clock().advance(std::chrono::minutes(14));
	CHECK(events.names(client).size() == 1);
	engine.clock().advance(std::chrono::minutes(1) + std::chrono::seconds(1));
	CHECK(events.names(client) == std::vector<std::string>{"project.autoSaveFailed", "project.autoSaveFailed"});
	CHECK(autoSaveLog(client).size() == 1);
	for (size_t interval = 1; interval <= 10; ++interval)
	{
		engine.clock().advance(g_pastInterval);
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
	engine.clock().advance(g_pastInterval);
	CHECK(events.names(client).back() == "project.autoSaved");
	CHECK(fs::is_regular_file(autoSaves / "Song-01.bak"));
	CHECK(autoSaveLog(client).size() == 3);
	fs::remove_all(autoSaves);
	std::ofstream(autoSaves) << "not a folder";
	run(client, "track.add", {{"type", "audio"}});
	engine.clock().advance(g_pastInterval);
	CHECK(events.names(client).back() == "project.autoSaveFailed");
	log = autoSaveLog(client);
	REQUIRE(log.size() == 4);
	CHECK(log[3].contains("error"));
	CHECK(loggedFailures(logFile) == 2);

	// So is the next failure after saving or opening the project.
	engine.clock().advance(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 4);
	run(client, "project.save");
	run(client, "track.add", {{"type", "audio"}});
	engine.clock().advance(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 5);
	engine.clock().advance(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 5);
	run(client, "project.load", {{"path", utf8(file)}});
	run(client, "track.add", {{"type", "audio"}});
	engine.clock().advance(g_pastInterval);
	CHECK(autoSaveLog(client).size() == 6);
	CHECK(loggedFailures(logFile) == 4);
	CHECK(events.names(client).size() == 19);
}

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
	REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
	click(app, "#project-error-dialog button[result=ok]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#project-error-dialog"); }));
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

	app.call("project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	fileMenu(app, "file.backup");
	CHECK(answer(app, {dir.path() / "Backup"}).kind == gnubase::ui::FileDialogKind::Folder);
	REQUIRE(app.waitUntil([&dir] { return fs::exists(dir.path() / "Backup" / "Song.dawproject"); }));
}
