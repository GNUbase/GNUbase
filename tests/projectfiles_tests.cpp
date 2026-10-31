// SPDX-License-Identifier: GPL-3.0-or-later
// Project handling (#77, docs/features/project-files.md): save, save as, save new version, revert, auto save, recent
// projects and templates through the engine API, which MCP and Lua use as they are, and the File menu and the start
// screen in the app's UI.
#include "engine/engine.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
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

	// An auto save opens like a project file, with the project as it was then.
	run(client, "project.load", {{"path", saved[2]}});
	CHECK(trackCount(client) == 4);

	// Due auto saves, as the engine checks every half a minute: only when on and the interval has passed.
	run(client, "track.add", {{"type", "audio"}});
	CHECK(run(client, "project.autoSave", {{"due", true}}).at("file").is_null()); // just loaded
	run(client, "settings.set", {{"key", "project.autoSave"}, {"value", false}});
	CHECK(run(client, "project.autoSave", {{"due", true}}).at("file").is_null());
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
		CHECK(app.shell().answerFileDialog(id, std::move(files)));
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
