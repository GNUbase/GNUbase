// SPDX-License-Identifier: GPL-3.0-or-later
// Track versions in the UI (#165, docs/features/track-versions.md): the Project menu's Track Versions on the selected
// tracks, and the active version's name in the track list, driven with ui::Automation as MCP's UI tools drive them.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/tracklist.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

	std::string row(uint64_t track)
	{
		return "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	}

	void click(test::UiApp& app, const std::string& selector, const std::vector<std::string>& modifiers = {})
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
		app.shell().step(std::chrono::milliseconds(0));
	}

	Json track(test::UiApp& app, uint64_t id)
	{
		const Json project = app.call("project.get");
		for (const Json& item : project.at("tracks"))
			if (item.at("id") == id)
				return item;
		FAIL("no track " << id);
	}

	// Project > Track Versions, and with a submenu one of its items.
	void openVersions(test::UiApp& app, const char* submenu = nullptr)
	{
		click(app, "#menu-bar button[menu=project-menu]");
		app.ui().move(std::nullopt, {"#track-versions-submenu", 0, std::nullopt});
		app.shell().step(std::chrono::milliseconds(0));
		if (submenu != nullptr)
		{
			app.ui().move(std::nullopt, {submenu, 0, std::nullopt});
			app.shell().step(std::chrono::milliseconds(0));
		}
	}
}

TEST_CASE("Project > Track Versions makes, activates and deletes versions of the selected tracks", "[ui]")
{
	test::UiApp app({1000, 600});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	const auto add = [&app]
	{ return app.call("track.add", {{"type", "audio"}, {"name", "Vox"}}).at("track").at("id").get<uint64_t>(); };
	const uint64_t first = add();
	const uint64_t second = add();
	const uint64_t other = add();
	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list listitem").size() == 3; }));
	app.shell().step(std::chrono::milliseconds(0));

	// New Version on the two selected tracks; the track list shows the new version's name.
	click(app, row(first) + " .track-name");
	click(app, row(second) + " .track-name", {"ctrl"});
	openVersions(app);
	click(app, "menuitem[command=\"track.newVersion\"]");
	REQUIRE(app.waitUntil([&] { return track(app, second).at("versions").size() == 2; }));
	const Json id = track(app, first).at("activeVersion");
	CHECK(track(app, second).at("activeVersion") == id);
	CHECK(track(app, other).at("versions").size() == 1);
	REQUIRE(app.waitUntil([&] { return !elements(app, row(first) + " .track-version").empty(); }));
	CHECK(elements(app, row(first) + " .track-version").at(0).value("text", "") == "v2");
	CHECK(elements(app, row(other) + " .track-version").empty());

	// Activate Version lists the first selected track's versions, the active one checked, and activates one on both.
	const Json items = elements(app, "#track-versions-list menuitem");
	REQUIRE(items.size() == 2);
	CHECK(items.at(1).at("classes").dump().find("checked") != std::string::npos);
	openVersions(app, "#track-versions-list-submenu");
	click(app, "#track-versions-list menuitem[version=\"1\"]");
	REQUIRE(app.waitUntil([&] { return track(app, second).at("activeVersion") == 1; }));
	CHECK(track(app, first).at("activeVersion") == 1);

	// Select Tracks with Same Version ID selects the tracks that have a version with the active one's ID.
	click(app, row(first) + " .track-name");
	app.call("trackVersion.activate", {{"tracks", {first}}, {"version", id}});
	REQUIRE(app.waitUntil(
		[&]
		{
			const Json version = elements(app, row(first) + " .track-version");
			return !version.empty() && version.at(0).value("text", "") == "v2";
		}));
	app.keys().run("track.selectSameVersionId", Json::object(), app.main(), nullptr);
	CHECK(app.appUi().trackList()->selection() == std::vector<uint64_t>{first, second});

	// Delete Version deletes the active versions: back to one, without a name in the list.
	app.keys().run("track.deleteVersion", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return track(app, first).at("versions").size() == 1; }));
	CHECK(track(app, second).at("versions").size() == 1);
	REQUIRE(app.waitUntil([&] { return elements(app, row(first) + " .track-version").empty(); }));
}
