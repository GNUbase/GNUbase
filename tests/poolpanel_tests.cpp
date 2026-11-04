// SPDX-License-Identifier: GPL-3.0-or-later
// The pool panel (#93, docs/features/pool.md) in the app's project window, driven with ui::Automation as MCP's UI
// tools drive it. The OS's file and folder dialogs are left out, since tests can't answer them; tests/pool_tests.cpp
// checks the engine's pool commands.
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
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
	app.call("job.cancel", {{"job", jobs.at(0).at("id")}});

	// Cancelled, it says so and leaves the media file as it was.
	REQUIRE(app.waitUntil([&app] { return message(app) == "Conform Files was cancelled."; }, std::chrono::seconds(30)));
	CHECK(!has(elements(app, "#pool-message").at(0), "error"));
	const Json media = app.call("project.get").at("media").at(0);
	CHECK(media.at("path") == slow.u8string());
	CHECK(media.at("sampleRate") == 44100.0);
}
