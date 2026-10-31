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
	REQUIRE(app.waitUntil([&app, &d, &k] { return rows(app).size() == 5 && rows(app)[2] == k; }));
	CHECK(rows(app)[1] == d);

	// New Folder adds a folder inside the selected one; Remove Unused Media removes the clips no event plays.
	click(app, "#pool listitem span.used", rowIndex(app, d));
	click(app, "#pool-new-folder");
	REQUIRE(app.waitUntil([&app] { return app.call("pool.get").at("folders").size() == 2; }));
	CHECK(app.call("pool.get").at("folders").at(1).at("parent") == drums);
	click(app, "#pool-remove-unused");
	REQUIRE(app.waitUntil([&app] { return app.call("pool.get").at("clips").size() == 1; }));
	REQUIRE(app.waitUntil([&app] { return rows(app).size() == 4; }));

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
