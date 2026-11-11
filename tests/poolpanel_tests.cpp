// SPDX-License-Identifier: GPL-3.0-or-later
// The pool panel (#93, docs/features/pool.md) in the app's project window, driven with ui::Automation as MCP's UI
// tools drive it. The OS's file and folder dialogs are left out, since tests can't answer them; tests/pool_tests.cpp
// checks the engine's pool commands.
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/poolpanel.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	void click(test::UiApp& app, const std::string& selector, int index = 0)
	{
		app.ui().click(std::nullopt, {selector, index, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	std::vector<std::string> rows(test::UiApp& app)
	{
		std::vector<std::string> values;
		for (const Json& row : elements(app, "#pool listitem"))
			values.push_back(row.at("attributes").value("value", ""));
		return values;
	}

	int rowIndex(test::UiApp& app, const std::string& value)
	{
		const std::vector<std::string> all = rows(app);
		const auto found = std::find(all.begin(), all.end(), value);
		REQUIRE(found != all.end());
		return static_cast<int>(found - all.begin());
	}

	std::string cell(test::UiApp& app, const std::string& value, const std::string& column)
	{
		const Json found = elements(app, "#pool listitem span." + column);
		return found.at(static_cast<size_t>(rowIndex(app, value))).value("text", "");
	}

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	std::string writeAudio(const std::filesystem::path& path, size_t frames)
	{
		test::writeWav(path, {std::vector<float>(frames, 0.25f), 1, 48000});
		return path.u8string();
	}

	uint64_t addClip(test::UiApp& app, const std::string& path)
	{
		const Json media = app.call("media.add", {{"path", path}}).at("media");
		return app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
	}

	// The panel's message line: what the last pool command did, or why it failed.
	std::string message(test::UiApp& app)
	{
		const Json found = elements(app, "#pool-message");
		return found.empty() ? std::string() : found.at(0).value("text", "");
	}

	uint64_t folderOf(test::UiApp& app, uint64_t clip)
	{
		const Json pool = app.call("pool.get");
		for (const Json& item : pool.at("clips"))
			if (item.at("id") == clip)
				return item.at("folder").get<uint64_t>();
		FAIL_CHECK("no clip " << clip);
		return 0;
	}

	// Whether the message line offers Stop: a job of a pool command runs.
	bool offersStop(test::UiApp& app)
	{
		const Json row = elements(app, ".pool-status");
		return !row.empty() && has(row.at(0), "running") && elements(app, "#pool-stop").at(0).at("visible") == true;
	}

	// A media file that isn't at path, as media.add takes it for a file that is still being recorded: one second of
	// mono at 48 kHz.
	void addMissing(test::UiApp& app, const std::filesystem::path& path)
	{
		app.call("media.add", {{"path", path.u8string()}, {"sampleRate", 48000.0}, {"channels", 1}, {"length", 48000}});
	}

	void openPool(test::UiApp& app, size_t rowCount)
	{
		app.keys().run("window.pool", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, rowCount] { return rows(app).size() == rowCount; }));
	}
}

TEST_CASE("the pool panel lists folders and clips, renames them and moves clips into folders", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const uint64_t kick = addClip(app, writeAudio(dir.path() / "kick.wav", 4800));
	const uint64_t snare = addClip(app, writeAudio(dir.path() / "snare.wav", 2400));
	// A file that isn't there, as media.add takes it for a file that is still being recorded.
	const Json absent = app.call("media.add",
		{{"path", (dir.path() / "gone.wav").u8string()}, {"sampleRate", 48000.0}, {"channels", 1}, {"length", 480}});
	const uint64_t gone =
		app.call("clip.add", {{"media", absent.at("media").at("id")}}).at("clip").at("id").get<uint64_t>();
	const uint64_t drums = app.call("poolFolder.add", {{"name", "Drums"}}).at("folder").at("id").get<uint64_t>();
	const Json track = app.call("track.add", {{"type", "audio"}}).at("track");
	app.call("audioEvent.add", {{"track", track.at("id")}, {"clip", kick}, {"position", 0}});
	const std::string k = "c" + std::to_string(kick);
	const std::string s = "c" + std::to_string(snare);
	const std::string g = "c" + std::to_string(gone);
	const std::string d = "f" + std::to_string(drums);

	// Media > Open Pool Window (Ctrl+P in the reference, manual p. 739) shows the panel.
	CHECK(elements(app, "#media-menu menuitem[command=\"window.pool\"]").size() == 1);
	const std::vector<std::string> keys = app.keys().keysFor("window.pool");
	CHECK(std::find_if(keys.begin(), keys.end(),
			  [](const std::string& key) { return key == "Ctrl+P" || key == "Cmd+P"; }) != keys.end());
	app.keys().run("window.pool", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return rows(app).size() == 5; }));
	CHECK(elements(app, "#pool").at(0).at("visible") == true);

	// The Audio folder, its folders, then its clips by name; the columns of p. 741.
	CHECK(rows(app) == std::vector<std::string>{"f0", d, g, k, s});
	CHECK(cell(app, k, "used") == "1");
	CHECK(cell(app, s, "used").empty());
	CHECK(cell(app, g, "status") == "Missing");
	CHECK(cell(app, k, "status") == "External"); // the project has no folder yet
	CHECK(cell(app, k, "info") == "48 kHz, Mono, 0.100 s");
	CHECK(cell(app, k, "path") == (dir.path() / "kick.wav").u8string());
	CHECK(has(elements(app, "#pool listitem").at(2), "missing"));
	CHECK(has(elements(app, "#pool listitem").at(4), "unused"));
	CHECK(elements(app, "#pool-info .audio-files").at(0).at("text") == "Audio Files: 3");
	CHECK(elements(app, "#pool-info .missing-files").at(0).at("text") == "Missing: 1");

	// Typing a name renames the clip.
	click(app, "#pool listitem input", rowIndex(app, s) - 1); // the Audio folder has no field
	app.ui().key(std::nullopt, "End");
	app.ui().text(std::nullopt, " 2");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil(
		[&app, snare]
		{
			const Json pool = app.call("pool.get");
			for (const Json& clip : pool.at("clips"))
				if (clip.at("id") == snare)
					return clip.at("name") == "snare 2";
			return false;
		}));

	// A clip dragged onto a folder goes into it.
	app.ui().drag(std::nullopt, {"#pool listitem span.used", rowIndex(app, k), std::nullopt},
		{"#pool listitem span.used", rowIndex(app, d), std::nullopt});
	REQUIRE(app.waitUntil([&app, &k] { return rows(app).size() == 5 && rows(app)[2] == k; }));
	CHECK(rows(app)[1] == d);

	// New Folder adds a folder inside the selected one; Remove Unused Media removes the clips no event plays.
	click(app, "#pool listitem span.used", rowIndex(app, d));
	click(app, "#pool-new-folder");
	REQUIRE(app.waitUntil([&app] { return app.call("pool.get").at("folders").size() == 2; }));
	CHECK(app.call("pool.get").at("folders").at(1).at("parent") == drums);
	click(app, "#pool-remove-unused");
	REQUIRE(app.waitUntil([&app] { return app.call("pool.get").at("clips").size() == 1; }));
	REQUIRE(app.waitUntil([&app] { return rows(app).size() == 4; }));
	REQUIRE(app.waitUntil([&app] { return !message(app).empty(); }));
	CHECK(message(app) == "Removed 2 clips and 2 files from the pool.");
	CHECK(!has(elements(app, "#pool-message").at(0), "error"));

	// Undo from another client shows at once.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return rows(app).size() == 6; }));
}

TEST_CASE("the pool panel's Import puts files into the pool through the Import Options dialog", "[ui]")
{
	const test::TempDir dir;
	const std::string a = writeAudio(dir.path() / "a.wav", 480);
	const std::string b = writeAudio(dir.path() / "b.wav", 480);
	test::UiApp app({1200, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	CHECK(app.keys().canRun("pool.import", app.main(), nullptr));
	CHECK(elements(app, "#media-menu menuitem[command=\"pool.import\"]").size() == 1);

	display.importFiles({a, b}, std::nullopt, std::nullopt, true); // what Import Medium's file dialog returns
	REQUIRE(app.waitUntil([&app] { return elements(app, "#import-options-dialog").size() == 1; }));
	CHECK(elements(app, "#import-one-track").at(0).value("attributes", Json::object()).contains("disabled"));
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("clips").size() == 2; }));
	CHECK(app.call("project.get").at("tracks").empty());
	CHECK(app.call("project.get").at("audioEvents").empty());
}

TEST_CASE(
	"the pool panel moves only clips dragged onto a folder, keeps names typed away and sorts names in any case", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const uint64_t kick = addClip(app, writeAudio(dir.path() / "kick.wav", 480));
	const uint64_t bass = addClip(app, writeAudio(dir.path() / "b.wav", 480));
	const uint64_t alpha = addClip(app, writeAudio(dir.path() / "a.wav", 480));
	app.call("clip.set", {{"clip", bass}, {"name", "Bass"}});
	app.call("clip.set", {{"clip", alpha}, {"name", "alpha"}});
	const uint64_t drums = app.call("poolFolder.add", {{"name", "Drums"}}).at("folder").at("id").get<uint64_t>();
	const std::string k = "c" + std::to_string(kick);
	const std::string d = "f" + std::to_string(drums);
	openPool(app, 5);

	// Names sort as the user reads them, whatever their letter case.
	CHECK(rows(app) == std::vector<std::string>{"f0", d, "c" + std::to_string(alpha), "c" + std::to_string(bass), k});

	// A clip pressed on and let go outside the panel isn't dragged: neither a click into a folder's name nor a drag
	// from outside onto the folder moves it.
	const ui::Target kickRow{"#pool listitem span.used", rowIndex(app, k), std::nullopt};
	const ui::Target outside{"#event-display", 0, std::nullopt};
	app.ui().drag(std::nullopt, kickRow, outside);
	click(app, "#pool listitem input", rowIndex(app, d) - 1); // the Audio folder has no field
	CHECK(folderOf(app, kick) == 0);
	app.ui().drag(std::nullopt, kickRow, outside);
	app.ui().drag(std::nullopt, outside, {"#pool listitem span.used", rowIndex(app, d), std::nullopt});
	CHECK(folderOf(app, kick) == 0);

	// A name typed away keeps the old one.
	click(app, "#pool listitem input", rowIndex(app, d) - 1);
	app.ui().key(std::nullopt, "End");
	for (int i = 0; i < 5; ++i)
		app.ui().key(std::nullopt, "Backspace");
	app.ui().key(std::nullopt, "Enter");
	CHECK(elements(app, "#pool listitem input").at(static_cast<size_t>(rowIndex(app, d) - 1)).value("value", "") ==
		"Drums");
	CHECK(app.call("pool.get").at("folders").at(0).at("name") == "Drums");

	// While another tab shows, the panel doesn't read the pool; once it shows again, it does.
	app.appUi().docking().showTab("mixer");
	addClip(app, writeAudio(dir.path() / "late.wav", 480));
	CHECK_FALSE(app.waitUntil([&app] { return rows(app).size() == 6; }, std::chrono::milliseconds(300)));
	openPool(app, 6);
}

TEST_CASE("the pool panel shows why Conform Files from the Media menu failed", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	test::writeWav(dir.path() / "slow.wav", {std::vector<float>(441, 0.25f), 1, 44100});
	addClip(app, (dir.path() / "slow.wav").u8string());

	// The project was never saved, so there is no media folder to conform into; the pool opens to say so.
	click(app, "#menu-bar button[menu=media-menu]");
	click(app, "#media-menu menuitem[command=\"media.conform\"]");
	REQUIRE(app.waitUntil([&app] { return !message(app).empty(); }));
	CHECK(message(app).find("Conform Files failed: save the project first") == 0);
	CHECK(has(elements(app, "#pool-message").at(0), "error"));
	CHECK(app.waitUntil([&app] { return elements(app, "#pool").at(0).at("visible") == true; }));
}

TEST_CASE("the pool panel shows what the Conform Files job did once it is done", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	std::filesystem::create_directories(dir.path() / "project");
	app.call(
		"project.save", {{"path", (dir.path() / "project" / "Song.dawproject").u8string()}, {"embedMedia", false}});
	test::writeWav(dir.path() / "slow.wav", {std::vector<float>(44100, 0.25f), 1, 44100});
	const uint64_t slow = addClip(app, (dir.path() / "slow.wav").u8string());

	// Conform Files starts a job; the message line follows it to its result.
	click(app, "#menu-bar button[menu=media-menu]");
	click(app, "#media-menu menuitem[command=\"media.conform\"]");
	REQUIRE(app.waitUntil(
		[&app] { return message(app) == "Conformed 1 file to the project's sample rate."; }, std::chrono::seconds(30)));
	CHECK(!has(elements(app, "#pool-message").at(0), "error"));
	CHECK(app.call("pool.get").at("clips").at(0).at("id") == slow);
	CHECK(app.call("pool.get").at("clips").at(0).at("sampleRate") == 48000.0);
}

TEST_CASE(
	"Prepare Archive from the Media menu copies the media into the project folder and says so in the pool", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	addClip(app, writeAudio(dir.path() / "library" / "take.wav", 480));
	addMissing(app, dir.path() / "library" / "gone.wav");

	// Unsaved, the project has no folder to copy into.
	const auto prepareArchive = [&app]
	{
		click(app, "#menu-bar button[menu=media-menu]");
		click(app, "#media-menu menuitem[command=\"pool.prepareArchive\"]");
	};
	prepareArchive();
	REQUIRE(app.waitUntil([&app] { return !message(app).empty(); }));
	CHECK(message(app).find("Prepare Archive failed: save the project first") == 0);

	// Saved, it asks before it copies files from outside the project folder (reference manual p. 135), counting only
	// those that are there and saying how many are missing (#1001), and Cancel copies nothing.
	std::filesystem::create_directories(dir.path() / "project");
	app.call(
		"project.save", {{"path", (dir.path() / "project" / "Song.dawproject").u8string()}, {"embedMedia", false}});
	const std::string before = message(app);
	prepareArchive();
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#prepare-archive-dialog").empty(); }));
	CHECK(elements(app, "#prepare-archive-message").at(0).value("text", "") ==
		"1 file of the project is outside the project folder. Copy it into its Audio folder? 1 more is missing and "
		"can't be copied.");
	click(app, "#prepare-archive-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#prepare-archive-dialog").empty(); }));
	CHECK(message(app) == before);
	CHECK(!std::filesystem::exists(dir.path() / "project" / "Audio"));

	// Proceed copies them.
	prepareArchive();
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#prepare-archive-dialog").empty(); }));
	click(app, "#prepare-archive-dialog button.primary");
	REQUIRE(app.waitUntil([&app]
		{ return message(app) == "Copied 1 file into the project folder; 1 missing file wasn't copied."; },
		std::chrono::seconds(30)));
	CHECK(std::filesystem::exists(dir.path() / "project" / "Audio" / "take.wav"));

	// With only missing files from outside, which it can't copy, it asks nothing.
	prepareArchive();
	REQUIRE(app.waitUntil(
		[&app] { return message(app) == "Copied 0 files into the project folder; 1 missing file wasn't copied."; }));
	CHECK(elements(app, "#prepare-archive-dialog").empty());

	// Nor without files from outside.
	app.call("media.removeMissing");
	prepareArchive();
	REQUIRE(app.waitUntil([&app] { return message(app) == "Copied 0 files into the project folder."; }));
	CHECK(elements(app, "#prepare-archive-dialog").empty());
}

TEST_CASE("the pool panel says which media Prepare Archive left alone because they changed meanwhile", "[ui]")
{
	test::UiApp app({640, 480});
	ui::EngineBinding binding{app.engine()};
	Rml::ElementDocument& document = app.show("other.rml");
	ui::PoolPanel panel(binding, *document.GetElementById("box"), [](const std::string&) {}, [] { return true; });
	panel.report("media.prepareArchive",
		{0, {{"copied", Json::array({1})}, {"missing", Json::array()}, {"changed", Json::array({2, 3})}}, {}});
	CHECK(document.GetElementById("pool-message")->GetInnerRML() ==
		"Copied 1 file into the project folder; 2 files changed meanwhile and were left as they are.");
}

TEST_CASE("the pool panel shows the Conform Files job's progress and that it was cancelled", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	std::filesystem::create_directories(dir.path() / "project");
	app.call(
		"project.save", {{"path", (dir.path() / "project" / "Song.dawproject").u8string()}, {"embedMedia", false}});
	// A minute of stereo at 44.1 kHz into a 192 kHz project keeps even a Release build converting for a while.
	app.call("project.set", {{"sampleRate", 192000.0}});
	const std::filesystem::path slow = dir.path() / "slow.wav";
	test::writeWav(slow, {std::vector<float>(size_t{44100} * 60 * 2, 0.25f), 2, 44100});
	addClip(app, slow.u8string());
	openPool(app, 2);
	const Json listTop = elements(app, "#pool").at(0).at("box").at("y");

	// While it converts, the message line says how far it got and with which file.
	click(app, "#menu-bar button[menu=media-menu]");
	click(app, "#media-menu menuitem[command=\"media.conform\"]");
	REQUIRE(app.waitUntil(
		[&app]
		{
			const std::string text = message(app);
			return text.rfind("Conform Files: ", 0) == 0 && text.find(" %, slow.wav") != std::string::npos;
		},
		std::chrono::seconds(30)));
	const Json jobs = app.call("job.list").at("jobs");
	REQUIRE(jobs.size() == 1);
	CHECK(jobs.at(0).at("kind") == "media.conform");

	// Meanwhile the line offers Stop (reference manual p. 751), which cancels it; cancelled, it says so, doesn't offer
	// Stop any more and leaves the media file as it was.
	REQUIRE(offersStop(app));
	CHECK(elements(app, "#pool").at(0).at("box").at("y") == listTop); // the list doesn't move for Stop
	click(app, "#pool-stop");
	REQUIRE(app.waitUntil([&app] { return message(app) == "Conform Files was cancelled."; }, std::chrono::seconds(30)));
	CHECK(!has(elements(app, "#pool-message").at(0), "error"));
	CHECK(!offersStop(app));
	CHECK(elements(app, "#pool-stop").at(0).at("visible") == false);
	const Json media = app.call("project.get").at("media").at(0);
	CHECK(media.at("path") == slow.u8string());
	CHECK(media.at("sampleRate") == 44100.0);
}

TEST_CASE("the pool panel shows what Find Missing Files' job found, and why a job failed", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	std::filesystem::create_directories(dir.path() / "project");
	app.call(
		"project.save", {{"path", (dir.path() / "project" / "Song.dawproject").u8string()}, {"embedMedia", false}});
	addMissing(app, dir.path() / "lost" / "kick.wav");
	addMissing(app, dir.path() / "lost" / "snare.wav");
	std::filesystem::create_directories(dir.path() / "found");
	test::writeWav(dir.path() / "found" / "kick.wav", {std::vector<float>(48000, 0.25f), 1, 48000});

	// The folder dialog can't be answered in a test: media.locate goes the way it sends it, through the UI's binding.
	app.keys().run("media.locate", {{"folder", (dir.path() / "found").u8string()}}, app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return message(app) == "Found 1 of 2 missing files."; }, std::chrono::seconds(30)));
	CHECK(!has(elements(app, "#pool-message").at(0), "error"));
	CHECK(!offersStop(app));
	CHECK(app.call("pool.get").at("missingFiles") == 1);

	// A job that fails says why: the media folder can't be made where a file is.
	std::ofstream(dir.path() / "project" / "Audio").put('x');
	test::writeWav(dir.path() / "slow.wav", {std::vector<float>(4410, 0.25f), 1, 44100});
	addClip(app, (dir.path() / "slow.wav").u8string());
	click(app, "#menu-bar button[menu=media-menu]");
	click(app, "#media-menu menuitem[command=\"media.conform\"]");
	REQUIRE(app.waitUntil(
		[&app] { return message(app).rfind("Conform Files failed: ", 0) == 0; }, std::chrono::seconds(30)));
	CHECK(has(elements(app, "#pool-message").at(0), "error"));
	CHECK(!offersStop(app));
}

TEST_CASE("the pool panel asks how a job ended that ended before its command's result came", "[ui]")
{
	// A panel of its own, with a binding of its own, so that the test decides when the result comes.
	const test::TempDir dir;
	test::UiApp app({640, 480});
	ui::EngineBinding binding{app.engine()};
	Rml::ElementDocument& document = app.show("other.rml");
	ui::PoolPanel panel(binding, *document.GetElementById("box"), [](const std::string&) {}, [] { return true; });
	const auto line = [&document] { return document.GetElementById("pool-message")->GetInnerRML(); };
	const auto running = [&document]
	{ return document.GetElementById("pool-stop")->GetParentNode()->IsClassSet("running"); };
	addMissing(app, dir.path() / "lost" / "kick.wav");
	addMissing(app, dir.path() / "lost" / "snare.wav");
	std::filesystem::create_directories(dir.path() / "found");
	test::writeWav(dir.path() / "found" / "kick.wav", {std::vector<float>(48000, 0.25f), 1, 48000});

	// The job ends, and its job.finished goes by, before the panel hears that media.locate started it.
	bool finished = false;
	const ui::Subscription heard = binding.on("job.finished", [&finished](const engine::Event&) { finished = true; });
	const Json job = app.call("media.locate", {{"folder", (dir.path() / "found").u8string()}}).at("job");
	REQUIRE(app.waitUntil(
		[&binding, &finished]
		{
			binding.update();
			return finished;
		}));
	CHECK(app.call("job.get", {{"job", job.at("id")}}).at("job").at("state") == "done");
	panel.report("media.locate", {0, {{"job", job}}, {}});
	CHECK(line() == "Find Missing Files...");
	CHECK(running());
	REQUIRE(app.waitUntil(
		[&binding, &line]
		{
			binding.update();
			return line() == "Found 1 of 2 missing files.";
		}));
	CHECK(!running());

	// Files found that changed meanwhile were looked for too.
	panel.report("media.locate",
		{0, {{"relinked", Json::array({1})}, {"missing", Json::array()}, {"changed", Json::array({2, 3})}}, {}});
	CHECK(line() == "Found 1 of 3 missing files.");
}
