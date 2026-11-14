// SPDX-License-Identifier: GPL-3.0-or-later
// Track versions in the UI (#165, #886, docs/features/track-versions.md): the Project menu's Track Versions on the
// selected tracks, the Track Versions pop-up menu at a track's name, Rename Version's dialog, the inspector's Track
// Versions section and the active version's name in the track list, driven with ui::Automation as MCP's UI tools drive
// them.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/tracklist.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	std::string text(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("text", std::string());
	}

	std::string attribute(test::UiApp& app, const std::string& selector, const char* name)
	{
		const Json attributes = element(app, selector).value("attributes", Json::object());
		return attributes.contains(name) ? attributes.at(name).get<std::string>() : std::string();
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const char* name)
	{
		const Json classes = element(app, selector).value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
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

	// Double-clicks a display field, types into it and presses Enter, as the inspector's name fields are edited.
	void type(test::UiApp& app, const std::string& selector, const std::string& text)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
		app.ui().key(std::nullopt, "Enter");
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

	// An audio track, and its versions' names.
	uint64_t add(test::UiApp& app, const char* name)
	{
		return app.call("track.add", {{"type", "audio"}, {"name", name}}).at("track").at("id").get<uint64_t>();
	}

	std::string activeName(test::UiApp& app, uint64_t id)
	{
		const Json json = track(app, id);
		for (const Json& version : json.at("versions"))
			if (version.at("id") == json.at("activeVersion"))
				return version.at("name").get<std::string>();
		FAIL("no active version on track " << id);
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

	// The Track Versions pop-up menu of a track, at the arrow to the right of its name.
	void openPopup(test::UiApp& app, uint64_t track)
	{
		click(app, row(track) + " .track-versions-toggle");
		REQUIRE(app.waitUntil([&] { return !elements(app, "#track-versions-menu menuitem").empty(); }));
	}
}

TEST_CASE("Project > Track Versions makes, activates and deletes versions of the selected tracks", "[ui]")
{
	test::UiApp app({1000, 600});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	const uint64_t first = add(app, "Vox");
	const uint64_t second = add(app, "Vox");
	const uint64_t other = add(app, "Vox");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list listitem").size() == 3; }));
	app.shell().step(std::chrono::milliseconds(0));

	// New Version on the two selected tracks; the track list shows the new version's name.
	click(app, row(first) + " .track-name");
	click(app, row(second) + " .track-name", {"ctrl"});
	openVersions(app);
	click(app, "menuitem[command=\"trackVersions.new\"]");
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
	app.keys().run("trackVersions.selectSameId", Json::object(), app.main(), nullptr);
	CHECK(app.appUi().trackList()->selection() == std::vector<uint64_t>{first, second});

	// Delete Version deletes the active versions: back to one, without a name in the list.
	app.keys().run("trackVersions.remove", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return track(app, first).at("versions").size() == 1; }));
	CHECK(track(app, second).at("versions").size() == 1);
	REQUIRE(app.waitUntil([&] { return elements(app, row(first) + " .track-version").empty(); }));
}

TEST_CASE("the Track Versions pop-up menu at a track's name", "[ui]")
{
	test::UiApp app({1000, 600});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	const uint64_t first = add(app, "Vox");
	const uint64_t second = add(app, "Gtr");
	app.call("track.add", {{"type", "marker"}}); // a track without versions has no arrow
	REQUIRE(app.waitUntil([&] { return elements(app, "#track-list listitem").size() == 3; }));
	REQUIRE(app.waitUntil([&] { return elements(app, row(first) + " .track-versions-toggle").size() == 1; }));
	CHECK(elements(app, "#track-list listitem[type=marker] .track-versions-toggle").empty());

	// The arrow keeps a selection it is in, so that the menu's functions work on every selected track (p. 234).
	click(app, row(first) + " .track-name");
	click(app, row(second) + " .track-name", {"ctrl"});
	openPopup(app, first);
	CHECK(app.appUi().trackList()->selection() == std::vector<uint64_t>{first, second});
	// One version each: its list has one item, and Delete Version isn't available.
	REQUIRE(elements(app, "#track-versions-menu menuitem[version]").size() == 1);
	CHECK(hasClass(app, "#track-versions-menu menuitem[version=\"1\"]", "checked"));
	CHECK(element(app, "#track-versions-menu menuitem[versions-action=\"trackVersions.remove\"]")
			.value("attributes", Json::object())
			.contains("disabled"));
	CHECK(elements(app, "#track-versions-menu menuitem[versions-action]").size() == 6);

	// New Version from the pop-up menu gives both tracks one.
	click(app, "#track-versions-menu menuitem[versions-action=\"trackVersions.new\"]");
	REQUIRE(app.waitUntil([&] { return track(app, second).at("versions").size() == 2; }));
	const int id = track(app, first).at("activeVersion").get<int>();
	CHECK(track(app, second).at("activeVersion") == id);

	// The list now has both versions and a click activates one on both tracks.
	REQUIRE(app.waitUntil([&] { return !elements(app, row(second) + " .track-version").empty(); }));
	openPopup(app, second);
	REQUIRE(elements(app, "#track-versions-menu menuitem[version]").size() == 2);
	CHECK(hasClass(app, "#track-versions-menu menuitem[version=\"" + std::to_string(id) + "\"]", "checked"));
	CHECK_FALSE(element(app, "#track-versions-menu menuitem[versions-action=\"trackVersions.remove\"]")
			.value("attributes", Json::object())
			.contains("disabled"));
	click(app, "#track-versions-menu menuitem[version=\"1\"]");
	REQUIRE(app.waitUntil([&] { return track(app, first).at("activeVersion") == 1; }));
	CHECK(track(app, second).at("activeVersion") == 1);

	// Show Version Name in Track List hides the name after the track's, and shows it again.
	const auto shown = [&app, first]
	{
		const Json version = elements(app, row(first) + " .track-version");
		return !version.empty() && version.at(0).value("text", "") == "v1";
	};
	REQUIRE(app.waitUntil(shown));
	openPopup(app, first);
	CHECK(hasClass(app, "#track-versions-menu menuitem[versions-action=showVersionName]", "checked"));
	click(app, "#track-versions-menu menuitem[versions-action=showVersionName]");
	REQUIRE(app.waitUntil([&] { return elements(app, row(first) + " .track-version").empty(); }));
	CHECK(elements(app, row(second) + " .track-version").empty());
	CHECK(app.call("settings.get", {{"key", "ui.showVersionName"}}).at("value") == false);
	openPopup(app, first);
	CHECK_FALSE(hasClass(app, "#track-versions-menu menuitem[versions-action=showVersionName]", "checked"));
	click(app, "#track-versions-menu menuitem[versions-action=showVersionName]");
	REQUIRE(app.waitUntil(shown));
	CHECK(app.call("settings.get", {{"key", "ui.showVersionName"}}).at("value") == true);
}

TEST_CASE("Rename Version and the inspector's Track Versions section", "[ui]")
{
	test::UiApp app({1200, 600});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.appUi().docking().open("inspector");
	app.shell().step(std::chrono::milliseconds(0));
	const uint64_t first = add(app, "Vox");
	const uint64_t second = add(app, "Gtr");
	app.call("track.add", {{"type", "marker"}});
	REQUIRE(app.waitUntil([&] { return elements(app, "#track-list listitem").size() == 3; }));
	click(app, row(first) + " .track-name");
	click(app, row(second) + " .track-name", {"ctrl"});

	// With one version: no indicator, one item in the list, and nothing to delete.
	REQUIRE(app.waitUntil([&] { return !hasClass(app, ".inspector [section=trackVersions]", "unavailable"); }));
	CHECK_FALSE(hasClass(app, ".inspector [section=trackVersions]", "multiple"));
	REQUIRE(elements(app, ".inspector .inspector-version").size() == 1);
	CHECK(text(app, ".inspector .inspector-version-id") == "1");
	CHECK(attribute(app, ".inspector textdisplay[control=versionName]", "value") == "v1");
	CHECK(hasClass(app, ".inspector .inspector-version-remove", "unavailable"));
	// A track without versions has no section.
	click(app, "#track-list listitem[type=marker] .track-name");
	REQUIRE(app.waitUntil([&] { return hasClass(app, ".inspector [section=trackVersions]", "unavailable"); }));
	click(app, row(first) + " .track-name");
	click(app, row(second) + " .track-name", {"ctrl"});

	// Rename Version's dialog names the active version of both selected tracks (p. 236).
	openVersions(app);
	click(app, "menuitem[command=\"trackVersions.rename\"]");
	REQUIRE(app.waitUntil([&] { return elements(app, "#rename-version-name").size() == 1; }));
	CHECK(attribute(app, "#rename-version-name", "value") == "v1"); // the name it renames, ready to type over
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Chorus");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return activeName(app, first) == "Chorus"; }));
	CHECK(activeName(app, second) == "Chorus");
	REQUIRE(app.waitUntil([&] { return elements(app, "#rename-version-name").empty(); }));

	// The inspector's section shows the active version and its list; a click in the list activates one on both.
	REQUIRE(app.waitUntil(
		[&] { return attribute(app, ".inspector textdisplay[control=versionName]", "value") == "Chorus"; }));
	CHECK(text(app, ".inspector .inspector-version-id") == "1");
	REQUIRE(elements(app, ".inspector .inspector-version").size() == 1);
	CHECK(hasClass(app, ".inspector .inspector-version[version=\"1\"]", "selected"));
	app.keys().run("trackVersions.duplicate", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return track(app, second).at("versions").size() == 2; }));
	const int id = track(app, first).at("activeVersion").get<int>();
	REQUIRE(app.waitUntil([&] { return elements(app, ".inspector .inspector-version").size() == 2; }));
	CHECK(hasClass(app, ".inspector [section=trackVersions]", "multiple"));
	CHECK(text(app, ".inspector .inspector-version-id") == std::to_string(id));
	CHECK_FALSE(hasClass(app, ".inspector .inspector-version-remove", "unavailable"));
	// The name typed into the section renames the active version of every selected track.
	type(app, ".inspector textdisplay[control=versionName]", "Bridge");
	REQUIRE(app.waitUntil([&] { return activeName(app, first) == "Bridge"; }));
	CHECK(activeName(app, second) == "Bridge");
	// A click in the list activates that version on both tracks.
	click(app, ".inspector .inspector-version[version=\"1\"]");
	REQUIRE(app.waitUntil([&] { return track(app, first).at("activeVersion") == 1; }));
	CHECK(track(app, second).at("activeVersion") == 1);
	REQUIRE(app.waitUntil([&] { return hasClass(app, ".inspector .inspector-version[version=\"1\"]", "selected"); }));
	// New and Delete work on both tracks too.
	click(app, ".inspector .inspector-version-add");
	REQUIRE(app.waitUntil([&] { return track(app, second).at("versions").size() == 3; }));
	CHECK(track(app, first).at("versions").size() == 3);
	click(app, ".inspector .inspector-version-remove");
	REQUIRE(app.waitUntil([&] { return track(app, first).at("versions").size() == 2; }));
	CHECK(track(app, second).at("versions").size() == 2);
}

TEST_CASE("the track version commands are a key command category of their own", "[ui]")
{
	test::UiApp app({1000, 760});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.keys().run("window.keyCommands", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#key-commands").empty(); }));
	// A search opens the categories (reference manual p. 231, docs/features/key-commands.md).
	click(app, "#keycommands-search");
	app.ui().text(std::nullopt, "trackVersions.");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(app.waitUntil([&] { return elements(app, "#keycommands-list listitem.command").size() == 6; }));
	CHECK(element(app, "#kc-area-trackVersions").at("visible") == true);
	CHECK(elements(app, "#kc-area-track").empty()); // not the track category
}
