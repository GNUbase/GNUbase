// SPDX-License-Identifier: GPL-3.0-or-later
// The preferences dialog (#162, docs/features/preferences.md): settings.set with several settings at once, preference
// presets in the engine, and the dialog that shows and changes the user settings in the app's UI, driven with
// ui::Automation as MCP's UI tools drive it.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/settings/usersettings.h"
#include "testutils/testutils.h"
#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json run(engine::Client& client, const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(engine::Client& client, const std::string& command, Json parameters)
	{
		const engine::Result result = client.call(command, std::move(parameters));
		// Not REQUIRE_FALSE: Catch2's Normal | FalseTest trips clang-analyzer's EnumCastOutOfRange check
		REQUIRE(!result.ok());
		return result.error;
	}

	Json setting(engine::Client& client, const std::string& key)
	{
		return run(client, "settings.get", {{"key", key}}).at("value");
	}

	// The data of the settings.changed events so far, one after the other.
	Json changes(engine::Client& client)
	{
		Json found = Json::array();
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<engine::Event>(&*message);
				event != nullptr && event->name == "settings.changed")
				found.push_back(event->data);
		return found;
	}

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

	bool checked(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("attributes", Json::object()).contains("checked");
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Replaces the text of a text field.
	void type(test::UiApp& app, const std::string& selector, const std::string& text)
	{
		click(app, selector);
		app.ui().key(std::nullopt, "Ctrl+A");
		app.ui().text(std::nullopt, text);
	}

	// Opens a drop-down list, moves the selection that many options down (up when negative) with the arrow keys, and
	// closes it. Its options aren't in the DOM that the UI tools see.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < std::abs(down); ++i)
			app.ui().key(std::nullopt, down > 0 ? "Down" : "Up");
		click(app, select);
	}

	bool open(test::UiApp& app)
	{
		return !elements(app, "#preferences").empty();
	}

	// Opens the dialog through its UI command, as Edit > Preferences does.
	void openDialog(test::UiApp& app)
	{
		app.keys().run("window.preferences", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return open(app); }));
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool closed(test::UiApp& app)
	{
		return app.waitUntil([&app] { return !open(app); });
	}

	// Shows a page of the dialog, as a click in its list of pages does.
	void page(test::UiApp& app, const std::string& id)
	{
		click(app, "#pages-" + id);
		REQUIRE(element(app, "#page-" + id).at("visible") == true);
	}

	// A setting's entry in settings.list: its schema, default and value.
	Json entryOf(const Json& list, const std::string& key)
	{
		for (const Json& entry : list.at("settings"))
			if (entry.at("key") == key)
				return entry;
		return Json::object();
	}

	// The user settings that aren't preferences: they have their own place in the UI, where the user changes them,
	// or are kept by the UI itself. Every other user setting must be on a page of the dialog.
	const std::set<std::string>& elsewhere()
	{
		static const std::set<std::string> keys = {
			"audio.backend",        // Studio Setup
			"audio.blockSize",      // Studio Setup
			"audio.busPresets",     // Audio Connections' presets
			"audio.device",         // Studio Setup
			"audio.fallbackDevice", // Studio Setup
			"audio.listenDim",      // the control room's listen settings
			"audio.listenLevel",    // the control room's listen settings
			"audio.portSettings",   // Studio Setup
			"audio.sampleRate",     // Studio Setup
			// the Import Options dialog
			"editing.importAudioConvert",
			"editing.importAudioCopy",
			"editing.importAudioNameFormat",
			"editing.importAudioOneTrack",
			"editing.importAudioSplitChannels",
			"editing.tempoType",             // the tempo track's and tempo editor's type of new tempo events
			"meter.resetOnStart",            // the loudness meter's own setting
			"meter.rmsAes17",                // the master meter's own settings
			"meter.rmsResolution",           // the master meter's own settings
			"midi.devices",                  // the MIDI device manager
			"midi.portSettings",             // Studio Setup
			"preferences.presets",           // the dialog's presets themselves
			"transport.cycleFollowsMarkers", // the markers window and the Transport menu
			"transport.tapTempoMode",        // the tempo track's Tap Tempo Mode menu
			"ui.controllerLanePresets",      // the MIDI editors' controller lane presets
			"ui.keyCommandPresets",          // the key commands dialog's presets
			"ui.keyCommands",                // the key commands dialog
			"ui.layout",                     // the windows' zones and panels, as the user arranges them
			"ui.macros",                     // the key commands dialog's macros
			"ui.markerColumns",              // the markers window
			"ui.markerIds",                  // the marker track
			"ui.markersAutoScroll",          // the markers window
			"ui.markersSyncSelection",       // the markers window
			"ui.timeDisplay",                // the time display window
			"ui.timeFormats",                // the transport bar's displays
			"ui.trackControls",              // the track controls settings dialog
			"ui.trackWidths",                // the track list's splitters
			"ui.zoomPresets",                // the zoom presets
		};
		return keys;
	}
}

TEST_CASE("settings.set changes several settings at once, all of them or none", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	const Json result =
		run(client, "settings.set", {{"values", {{"audio.mutePreSends", true}, {"ui.folderData", "never"}}}});
	CHECK(result == Json({{"values", {{"audio.mutePreSends", true}, {"ui.folderData", "never"}}}}));
	CHECK(setting(client, "audio.mutePreSends") == true);
	CHECK(changes(client) == Json::array({{{"values", {{"audio.mutePreSends", true}, {"ui.folderData", "never"}}}}}));

	// One that doesn't fit, or isn't a setting, changes none.
	CHECK_THAT(reject(client, "settings.set", {{"values", {{"audio.mutePreSends", false}, {"ui.scale", 40}}}}),
		ContainsSubstring("ui.scale"));
	CHECK_THAT(reject(client, "settings.set", {{"values", {{"audio.mutePreSends", false}, {"ui.zoom", 1}}}}),
		ContainsSubstring("ui.zoom"));
	CHECK(setting(client, "audio.mutePreSends") == true);
	// Unchanged values send nothing.
	run(client, "settings.set", {{"values", {{"audio.mutePreSends", true}}}});
	CHECK(changes(client).empty());

	// Either key and value or values.
	CHECK_THAT(reject(client, "settings.set", Json::object()), ContainsSubstring("values"));
	CHECK_THAT(reject(client, "settings.set", {{"key", "ui.scale"}}), ContainsSubstring("value"));
	CHECK_THAT(reject(client, "settings.set", {{"key", "ui.scale"}, {"value", 1.5}, {"values", Json::object()}}),
		ContainsSubstring("values"));
	CHECK(run(client, "settings.set", {{"key", "ui.scale"}, {"value", 1.5}}) ==
		Json({{"key", "ui.scale"}, {"value", 1.5}}));
}

TEST_CASE("preference presets are saved, renamed and removed in the user settings", "[unit]")
{
	// Reference manual pp. 1593-1595: presets of complete or partial preferences.
	const test::TempDir dir;
	engine::Engine engine(audio::makeBackends(), dir.path());
	engine::Client client(engine);
	const Json mixing = {{"audio.mutePreSends", true}, {"ui.folderData", "never"}};
	run(client, "preferences.savePreset", {{"name", "Mixing"}, {"values", mixing}});
	CHECK(setting(client, "preferences.presets") == Json({{"Mixing", mixing}}));
	CHECK(changes(client) == Json::array({{{"values", {{"preferences.presets", {{"Mixing", mixing}}}}}}}));
	// Saving it changes no preference.
	CHECK(setting(client, "audio.mutePreSends") == false);

	// Values must be preferences that fit their settings.
	CHECK_THAT(reject(client, "preferences.savePreset", {{"name", "x"}, {"values", {{"ui.zoom", 1}}}}),
		ContainsSubstring("ui.zoom"));
	CHECK_THAT(reject(client, "preferences.savePreset", {{"name", "x"}, {"values", {{"ui.scale", 40}}}}),
		ContainsSubstring("ui.scale"));
	CHECK_THAT(reject(client, "preferences.savePreset",
				   {{"name", "x"}, {"values", {{"preferences.presets", Json::object()}}}}),
		ContainsSubstring("preferences.presets"));
	CHECK_THAT(reject(client, "preferences.savePreset", {{"name", ""}, {"values", mixing}}), ContainsSubstring("name"));

	// A preset of the same name is replaced.
	const Json partial = {{"ui.scale", 1.5}};
	run(client, "preferences.savePreset", {{"name", "Big"}, {"values", partial}});
	run(client, "preferences.savePreset", {{"name", "Big"}, {"values", {{"ui.scale", 2.0}}}});
	CHECK(setting(client, "preferences.presets").at("Big") == Json({{"ui.scale", 2.0}}));

	run(client, "preferences.renamePreset", {{"name", "Mixing"}, {"newName", "Mix"}});
	CHECK(setting(client, "preferences.presets") == Json({{"Big", {{"ui.scale", 2.0}}}, {"Mix", mixing}}));
	CHECK_THAT(
		reject(client, "preferences.renamePreset", {{"name", "Mix"}, {"newName", "Big"}}), ContainsSubstring("Big"));
	CHECK_THAT(reject(client, "preferences.renamePreset", {{"name", "Mixing"}, {"newName", "Other"}}),
		ContainsSubstring("Mixing"));
	run(client, "preferences.removePreset", {{"name", "Big"}});
	CHECK(setting(client, "preferences.presets") == Json({{"Mix", mixing}}));
	CHECK_THAT(reject(client, "preferences.removePreset", {{"name", "Big"}}), ContainsSubstring("Big"));

	// A preset applies with settings.set, all of its values in one write; the next start still has it.
	run(client, "settings.set", {{"values", setting(client, "preferences.presets").at("Mix")}});
	CHECK(setting(client, "ui.folderData") == "never");
	engine::Engine restarted(audio::makeBackends(), dir.path());
	engine::Client again(restarted);
	CHECK(setting(again, "preferences.presets") == Json({{"Mix", mixing}}));
}

TEST_CASE("every user setting is on a page of the preferences dialog or has a place of its own", "[ui]")
{
	test::UiApp app({1000, 760});
	openDialog(app);
	std::set<std::string> shown;
	for (const Json& control : elements(app, ".preference"))
		shown.insert(control.at("attributes").at("setting").get<std::string>());
	for (const engine::settings::Definition& definition : engine::settings::userFormat().definitions)
	{
		INFO(definition.key);
		CHECK(shown.count(definition.key) + elsewhere().count(definition.key) == 1);
	}
	for (const std::string& key : shown)
	{
		INFO(key);
		CHECK(elsewhere().count(key) == 0);
	}
}

TEST_CASE("the preferences dialog shows each preference and changes it through the engine", "[ui]")
{
	test::UiApp app({1000, 760});
	const Json before = app.call("settings.list");
	openDialog(app);
	CHECK(text(app, "#page-editing .page-title") == "Editing");
	CHECK(checked(app, "#pref-ui-removeTracksWarning"));
	CHECK(element(app, "#pref-editing-defaultTimeBase").at("value") == "musical");
	CHECK(element(app, "#pref-project-autoSaveInterval").at("value") == "15");

	// Every control of every page, changed: a check box clicked, another choice, a number at its minimum (or maximum
	// if it is there already).
	std::set<std::string> changed;
	for (const Json& item : elements(app, "#preferences-pages listitem"))
	{
		const std::string id = item.at("attributes").at("value").get<std::string>();
		page(app, id);
		for (const Json& control : elements(app, "#page-" + id + " .preference"))
		{
			const std::string selector = "#" + control.at("id").get<std::string>();
			const std::string key = control.at("attributes").at("setting").get<std::string>();
			INFO(selector);
			changed.insert(key);
			const std::string tag = control.at("tag").get<std::string>();
			const std::string kind = control.value("attributes", Json::object()).value("type", "");
			if (kind == "checkbox")
			{
				const bool was = checked(app, selector);
				click(app, selector);
				CHECK(checked(app, selector) != was);
			}
			else if (tag == "select")
			{
				const Json was = control.at("value");
				choose(app, selector, 1);
				if (element(app, selector).at("value") == was)
					choose(app, selector, -1); // it was the last choice
				CHECK(element(app, selector).at("value") != was);
			}
			else
			{
				const Json schema = entryOf(before, key).at("schema");
				const std::string minimum = schema.at("minimum").dump();
				const std::string value =
					control.at("value") == minimum ? schema.at("maximum").dump() : schema.at("minimum").dump();
				type(app, selector, value);
			}
		}
	}
	CHECK(changed.size() > 20);
	click(app, "#preferences-ok");
	CHECK(closed(app));
	for (const std::string& key : changed)
	{
		INFO(key);
		CHECK(app.call("settings.get", {{"key", key}}).at("value") != entryOf(before, key).at("value"));
	}

	// The dialog shows the values the engine has now.
	openDialog(app);
	CHECK_FALSE(checked(app, "#pref-ui-removeTracksWarning"));
	CHECK(element(app, "#pref-project-autoSaveInterval").at("value") == "1");
}

TEST_CASE("the preferences dialog's Apply, Cancel and Defaults", "[ui]")
{
	test::UiApp app({1000, 760});
	openDialog(app);
	page(app, "general");
	type(app, "#pref-project-autoSaveFiles", "9");
	click(app, "#preferences-apply");
	CHECK(open(app)); // Apply keeps the dialog open
	CHECK(app.call("settings.get", {{"key", "project.autoSaveFiles"}}).at("value") == 9);

	// Cancel drops what changed since.
	type(app, "#pref-project-autoSaveFiles", "3");
	click(app, "#preferences-cancel");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "project.autoSaveFiles"}}).at("value") == 9);

	// Defaults puts the page's preferences, and only those, back to their defaults, which OK then sends.
	app.call("settings.set", {{"key", "audio.mutePreSends"}, {"value", true}});
	openDialog(app);
	page(app, "general");
	click(app, "#preferences-defaults");
	CHECK(element(app, "#pref-project-autoSaveFiles").at("value") == "5");
	CHECK(app.call("settings.get", {{"key", "project.autoSaveFiles"}}).at("value") == 9); // not yet
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "project.autoSaveFiles"}}).at("value") == 5);
	CHECK(app.call("settings.get", {{"key", "audio.mutePreSends"}}).at("value") == true);

	// What the engine refuses keeps the dialog open with the message, and changes nothing.
	openDialog(app);
	page(app, "general");
	type(app, "#pref-project-autoSaveFiles", "500");
	type(app, "#pref-project-autoSaveInterval", "30");
	click(app, "#preferences-ok");
	CHECK(open(app));
	CHECK_THAT(text(app, "#preferences-message"), ContainsSubstring("project.autoSaveFiles"));
	CHECK(app.call("settings.get", {{"key", "project.autoSaveInterval"}}).at("value") == 15);
	type(app, "#pref-project-autoSaveFiles", "many");
	click(app, "#preferences-ok");
	CHECK(open(app));
	CHECK_THAT(text(app, "#preferences-message"), ContainsSubstring("Maximum Number of Auto Save Files"));
}

TEST_CASE("the preferences dialog's preferences take effect", "[ui]")
{
	test::UiApp app({1000, 760});

	// Default Track Time Type (p. 1595): new tracks are linear.
	openDialog(app);
	choose(app, "#pref-editing-defaultTimeBase", 1); // Time Linear
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("track.add", {{"type", "audio"}}).at("track").at("timeBase") == "linear");

	// The tool modifiers (p. 1601): the event display reads them.
	openDialog(app);
	page(app, "editing-tool-modifiers");
	CHECK(element(app, "#pref-tool-dragDrop-copy").at("value") == "alt");
	choose(app, "#pref-tool-dragDrop-copy", -1); // ctrl, before alt
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "ui.toolModifiers"}}).at("value") == Json({{"dragDrop.copy", "ctrl"}}));

	// The MIDI filter's Record section (p. 1613): recording leaves out what it filters.
	openDialog(app);
	page(app, "midi-filter");
	click(app, "#pref-filter-type-programs");
	click(app, "#pref-filter-channel-10");
	choose(app, "#preferences-filter-controller", 7); // CC 7
	click(app, "#preferences-filter-add");
	CHECK(elements(app, "#preferences-filter-controllers listitem").size() == 1);
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "midi.recordFilter"}}).at("value") ==
		Json({{"types", {"programs"}}, {"channels", {10}}, {"controllers", {7}}}));
	openDialog(app);
	page(app, "midi-filter");
	click(app, "#preferences-filter-controllers listitem");
	click(app, "#preferences-filter-remove");
	click(app, "#pref-filter-type-programs");
	click(app, "#pref-filter-channel-10");
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "midi.recordFilter"}}).at("value") == Json::object());

	// The theme (User Interface): the UI switches to it.
	openDialog(app);
	page(app, "user-interface");
	CHECK(element(app, "#pref-ui-theme").at("value") == "gnubase");
	choose(app, "#pref-ui-theme", -1); // Dark, before GNUbase
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "ui.theme"}}).at("value") == "dark");
	CHECK(app.waitUntil([&app] { return app.shell().theme() == "dark"; }));
}

TEST_CASE("the preferences dialog saves, chooses, renames and deletes preference presets", "[ui]")
{
	test::UiApp app({1000, 760});
	openDialog(app);
	CHECK(element(app, "#preferences-preset").at("value") == "");
	// A preset holds what the dialog shows, applied or not.
	page(app, "general");
	type(app, "#pref-project-autoSaveFiles", "8");
	type(app, "#preferences-preset-name", "Whole");
	click(app, "#preferences-save-preset");
	const Json presets = app.call("settings.get", {{"key", "preferences.presets"}}).at("value");
	REQUIRE(presets.contains("Whole"));
	CHECK(presets.at("Whole").at("project.autoSaveFiles") == 8);
	CHECK(presets.at("Whole").contains("ui.removeTracksWarning")); // every page
	CHECK(element(app, "#preferences-preset").at("value") == "Whole");

	// Save marked preferences only: the pages marked in the Save column.
	click(app, "#preferences-marked-only");
	click(app, "#save-general");
	type(app, "#preferences-preset-name", "General");
	click(app, "#preferences-save-preset");
	CHECK(app.call("settings.get", {{"key", "preferences.presets"}}).at("value").at("General") ==
		Json({{"project.autoSave", true}, {"project.autoSaveFiles", 8}, {"project.autoSaveInterval", 15}}));
	click(app, "#preferences-cancel");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "project.autoSaveFiles"}}).at("value") == 5);

	// Choosing a preset puts its values into the dialog; OK applies them.
	app.call("settings.set", {{"key", "audio.mutePreSends"}, {"value", true}});
	openDialog(app);
	choose(app, "#preferences-preset", 1); // General, after No Preset
	CHECK(element(app, "#preferences-preset").at("value") == "General");
	page(app, "general");
	CHECK(element(app, "#pref-project-autoSaveFiles").at("value") == "8");
	page(app, "audio");
	CHECK(checked(app, "#pref-audio-mutePreSends")); // a partial preset leaves the others as they are
	click(app, "#preferences-ok");
	CHECK(closed(app));
	CHECK(app.call("settings.get", {{"key", "project.autoSaveFiles"}}).at("value") == 8);

	// Rename and Delete act on the chosen preset.
	openDialog(app);
	choose(app, "#preferences-preset", 2); // Whole, after General
	type(app, "#preferences-preset-name", "Whole Set");
	click(app, "#preferences-rename-preset");
	CHECK(app.call("settings.get", {{"key", "preferences.presets"}}).at("value").contains("Whole Set"));
	CHECK(element(app, "#preferences-preset").at("value") == "Whole Set");
	click(app, "#preferences-delete-preset");
	CHECK(app.call("settings.get", {{"key", "preferences.presets"}}).at("value") ==
		Json({{"General",
			{{"project.autoSave", true}, {"project.autoSaveFiles", 8}, {"project.autoSaveInterval", 15}}}}));
	CHECK(element(app, "#preferences-preset").at("value") == "");
}
