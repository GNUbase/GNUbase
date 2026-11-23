// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI devices and patch banks (#136, docs/features/midi-devices.md) in the app's UI, driven with ui::Automation as
// MCP's UI tools drive it: the inspector chooses a MIDI track's patch, and the MIDI device manager installs, shows,
// edits and removes devices, all through engine commands.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	std::string attribute(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		const Json attributes = found[0].value("attributes", Json::object());
		return attributes.contains(name) ? attributes.at(name).get<std::string>() : std::string();
	}

	void click(test::UiApp& app, const std::string& selector, int count = 1)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Opens a drop-down list, moves the selection down that many options with the arrow key, and closes it.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < down; ++i)
			app.ui().key(std::nullopt, "Down");
		click(app, select);
	}

	// The text of a drop-down list's selected option.
	std::string selected(test::UiApp& app, const std::string& selector)
	{
		return test::selectedOption(app, selector);
	}

	Json channel(test::UiApp& app)
	{
		return app.call("project.get").at("channels")[0];
	}

	Json devices(test::UiApp& app)
	{
		return app.call("settings.get", {{"key", "midi.devices"}}).at("value");
	}
}

TEST_CASE("the inspector chooses a MIDI track's patch by bank and program, or from its device's patches", "[ui]")
{
	test::UiApp app{{1200, 600}};
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.appUi().docking().open("inspector");
	app.shell().step(std::chrono::milliseconds(0));
	const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return display.project().at("tracks").size() == 1; }));
	app.appUi().trackList()->select({track});
	const auto shows = [&](const std::string& selector, const std::string& value)
	{ return app.waitUntil([&] { return attribute(app, ".inspector " + selector, "value") == value; }); };
	REQUIRE(shows("select[control=program]", "off"));
	CHECK(attribute(app, ".inspector textdisplay[control=bank]", "value") == "Off");
	CHECK(attribute(app, ".inspector select[control=device]", "value").empty());

	// Without a device: programs 1 to 128 and a bank typed, from 1.
	choose(app, ".inspector select[control=program]", 5);
	REQUIRE(app.waitUntil([&] { return channel(app).at("program") == 4; }));
	CHECK(channel(app).at("bank").is_null());
	click(app, ".inspector textdisplay[control=bank]", 2);
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "301");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return channel(app).at("bank") == 300; }));
	REQUIRE(shows("textdisplay[control=bank]", "301"));
	CHECK(shows("select[control=program]", "4:300"));
	CHECK(channel(app).at("program") == 4); // the program keeps its bank

	// With an installed device: its patches by name instead, the bank row gone, and the track's own patch listed.
	app.call("midiDevice.install", {{"name", "Sound Module"}, {"template", "gm"}});
	choose(app, ".inspector select[control=device]", 1);
	REQUIRE(app.waitUntil([&] { return channel(app).at("device") == "Sound Module"; }));
	REQUIRE(app.waitUntil(
		[&]
		{
			const Json classes = elements(app, ".inspector [row=bank]")[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), "unavailable") != classes.end();
		}));
	CHECK(selected(app, ".inspector select[control=program]") == "Program 5");
	app.call("channel.set", {{"channel", channel(app).at("id")}, {"program", 0}, {"bank", nullptr}});
	REQUIRE(shows("select[control=program]", "0:"));
	CHECK(selected(app, ".inspector select[control=program]") == "Piano / Acoustic Grand Piano");
	choose(app, ".inspector select[control=program]", 4);
	REQUIRE(app.waitUntil([&] { return channel(app).at("program") == 4; }));
	// The inspector shows the engine's program a step or more after the engine has it, and an answer from before
	// can show the program before the choice until then (#951).
	CHECK(app.waitUntil(
		[&] { return selected(app, ".inspector select[control=program]") == "Piano / Electric Piano 1"; }));

	// A device that is removed shows as not installed.
	app.call("midiDevice.remove", {{"name", "Sound Module"}});
	REQUIRE(app.waitUntil([&] { return selected(app, ".inspector select[control=device]") != "Sound Module"; }));
	CHECK(selected(app, ".inspector select[control=device]") == "Sound Module (not installed)");
}

TEST_CASE("the MIDI device manager installs, shows and removes MIDI devices", "[ui]")
{
	test::UiApp app{{900, 640}};
	app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#midi-devices").empty(); }));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#installed-devices listitem").empty());

	// A GM device: its bank, folders and patches.
	const auto install = [&](const std::string& name)
	{
		click(app, "#device-name");
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, name);
		click(app, "#install-device");
	};
	install("Sound Module");
	REQUIRE(app.waitUntil([&] { return devices(app).contains("Sound Module"); }));
	REQUIRE(app.waitUntil([&] { return elements(app, "#installed-devices listitem").size() == 1; }));
	CHECK(elements(app, "#patch-banks listitem").size() == 1 + 16 + 128);
	CHECK_THAT(elements(app, "#patch-banks listitem")[2].at("text").get<std::string>(),
		ContainsSubstring("Acoustic Grand Piano") && ContainsSubstring("Program 1"));

	// Define New: a device without banks, selected once installed; a name that is taken is refused.
	choose(app, "#device-template", 1);
	install("My Synth");
	REQUIRE(app.waitUntil([&] { return devices(app).contains("My Synth"); }));
	CHECK(devices(app).at("My Synth").at("banks").empty());
	REQUIRE(app.waitUntil([&] { return elements(app, "#installed-devices listitem").size() == 2; }));
	CHECK(elements(app, "#patch-banks listitem").empty());
	install("My Synth");
	REQUIRE(app.waitUntil(
		[&]
		{
			return elements(app, "#midi-devices-message")[0].value("text", std::string()).find("already") !=
				std::string::npos;
		}));

	// Remove Device: the selected one.
	click(app, "#remove-device");
	REQUIRE(app.waitUntil([&] { return !devices(app).contains("My Synth"); }));
	CHECK(devices(app).contains("Sound Module"));
	click(app, "#close");
	REQUIRE(app.waitUntil([&] { return elements(app, "#midi-devices").empty(); }));
}

TEST_CASE("the MIDI device manager follows external settings and groups interleaved folders", "[ui]")
{
	test::UiApp app{{900, 640}};
	Json value = {{"Synth",
		{{"banks",
			{{{"name", "Bank"},
				{"patches",
					{{{"name", "First"}, {"program", 0}, {"folder", "Keys"}}, {{"name", "Root"}, {"program", 1}},
						{{"name", "Other"}, {"program", 2}, {"folder", "Strings"}},
						{{"name", "Last"}, {"program", 3}, {"folder", "Keys"}}}}}}}}}};
	app.call("settings.set", {{"key", "midi.devices"}, {"value", value}});
	app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#midi-devices").empty(); }));
	SECTION("each folder appears once, with its patches together")
	{
		const Json rows = elements(app, "#patch-banks listitem");
		REQUIRE(rows.size() == 7);
		CHECK_THAT(rows[1].at("text").get<std::string>(), ContainsSubstring("Keys"));
		CHECK_THAT(rows[2].at("text").get<std::string>(), ContainsSubstring("First"));
		CHECK_THAT(rows[3].at("text").get<std::string>(), ContainsSubstring("Last"));
		CHECK_THAT(rows[4].at("text").get<std::string>(), ContainsSubstring("Root"));
		CHECK_THAT(rows[5].at("text").get<std::string>(), ContainsSubstring("Strings"));
	}
	SECTION("external edits preserve the selected device and follow removal and reopening")
	{
		app.call("midiDevice.install", {{"name", "Another"}});
		REQUIRE(app.waitUntil([&] { return elements(app, "#installed-devices listitem").size() == 2; }));
		CHECK(attribute(app, "#installed-devices", "value") == "2"); // Synth is still selected
		value = devices(app);
		value.at("Synth").at("banks")[0]["name"] = "Changed";
		app.call("settings.set", {{"key", "midi.devices"}, {"value", value}});
		REQUIRE(app.waitUntil([&] { return elements(app, "#patch-banks listitem")[0].at("text") == "Changed"; }));
		app.call("midiDevice.remove", {{"name", "Synth"}});
		REQUIRE(app.waitUntil([&] { return elements(app, "#patch-banks listitem").empty(); }));
		click(app, "#close");
		REQUIRE(app.waitUntil([&] { return elements(app, "#midi-devices").empty(); }));
		app.call("midiDevice.install", {{"name", "Closed"}});
		app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&] { return elements(app, "#installed-devices listitem").size() == 2; }));
		app.call("midiDevice.remove", {{"name", "Another"}});
		CHECK(app.waitUntil([&] { return elements(app, "#installed-devices listitem").size() == 1; }));
	}
}

TEST_CASE("the MIDI device manager edits banks and presets only with Enable Edit", "[ui]")
{
	test::UiApp app{{900, 640}};
	app.call("midiDevice.install", {{"name", "Synth"}});
	app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#midi-devices").empty(); }));
	const auto type = [&](const std::string& id, const std::string& text)
	{
		click(app, id);
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
	};
	const auto banks = [&] { return devices(app).at("Synth").at("banks"); };
	REQUIRE(elements(app, "#enable-edit").size() == 1);
	click(app, "#enable-edit");
	click(app, "#create-bank");
	REQUIRE(app.waitUntil([&] { return banks().size() == 1; }));
	type("#patch-name", "Custom Bank");
	click(app, "#apply-patch");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("name") == "Custom Bank"; }));
	click(app, "#new-preset");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches").size() == 1; }));
	type("#patch-name", "My <Piano>");
	type("#patch-folder", "Keys & Pads");
	type("#patch-program", "128");
	type("#patch-bank", "16384");
	click(app, "#apply-patch");
	const Json expected = {{"name", "My <Piano>"}, {"folder", "Keys & Pads"}, {"program", 127}, {"bank", 16383}};
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches")[0] == expected; }));
	CHECK_THAT(
		elements(app, "#patch-banks .patch-name")[0].at("text").get<std::string>(), ContainsSubstring("My <Piano>"));
	// Invalid numbers report an error without changing settings.
	type("#patch-program", "129");
	click(app, "#apply-patch");
	CHECK(banks()[0].at("patches")[0] == expected);
	CHECK(!elements(app, "#midi-devices-message")[0].value("text", std::string()).empty());
	type("#patch-program", "1");
	type("#patch-bank", "");
	click(app, "#apply-patch");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches")[0].at("program") == 0; }));
	CHECK(!banks()[0].at("patches")[0].contains("bank"));
	click(app, "#enable-edit");
	CHECK(elements(app, "#create-bank")[0].at("attributes").contains("disabled"));
	click(app, "#enable-edit");
	click(app, "#remove-patch");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches").empty(); }));
	// A folder groups presets: rename and remove the group as a whole.
	click(app, "#new-preset");
	type("#patch-folder", "Keys");
	click(app, "#apply-patch");
	click(app, "#new-preset");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches").size() == 2; }));
	click(app, "#patch-banks listitem[value=folder1-1]");
	type("#patch-name", "Renamed Keys");
	click(app, "#apply-patch");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches")[0].at("folder") == "Renamed Keys"; }));
	CHECK(banks()[0].at("patches")[1].at("folder") == "Renamed Keys");
	click(app, "#remove-patch");
	REQUIRE(app.waitUntil([&] { return banks()[0].at("patches").empty(); }));
	click(app, "#patch-banks listitem[value=bank1]");
	app.ui().key(std::nullopt, "Backspace");
	CHECK(app.waitUntil([&] { return banks().empty(); }));
}

TEST_CASE("the MIDI device manager adds a checked range of presets from a selected template", "[ui]")
{
	test::UiApp app{{900, 640}};
	const Json patch = {{"name", "Brass"}, {"program", 125}, {"bank", 300}, {"folder", "Winds"}};
	app.call("settings.set",
		{{"key", "midi.devices"}, {"value", {{"Synth", {{"banks", {{{"name", "Bank"}, {"patches", {patch}}}}}}}}}});
	app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#midi-devices").empty(); }));
	click(app, "#enable-edit");
	click(app, "#patch-banks listitem[value=patch1-1]");
	const auto count = [&](const std::string& text)
	{
		click(app, "#preset-count");
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
		click(app, "#multiple-presets");
	};
	const auto patches = [&] { return devices(app).at("Synth").at("banks")[0].at("patches"); };
	REQUIRE(elements(app, "#multiple-presets").size() == 1);
	for (const char* invalid : {"words", "0", "1.5", "4"}) // the last range would exceed program 128
	{
		count(invalid);
		CHECK(patches().size() == 1);
		CHECK(!elements(app, "#midi-devices-message")[0].value("text", std::string()).empty());
	}
	count("3");
	REQUIRE(app.waitUntil([&] { return patches().size() == 4; }));
	for (int i = 1; i <= 3; ++i)
	{
		const Json added = patches()[i];
		CHECK(added.at("program") == 124 + i);
		CHECK(added.at("name") == "Brass " + std::to_string(i));
		CHECK(added.at("bank") == 300);
		CHECK(added.at("folder") == "Winds");
	}
}

TEST_CASE("the inspector chooses the MIDI channel a MIDI track's patch goes out on", "[ui]")
{
	test::UiApp app{{1200, 600}};
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.appUi().docking().open("inspector");
	app.shell().step(std::chrono::milliseconds(0));
	const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return display.project().at("tracks").size() == 1; }));
	app.appUi().trackList()->select({track});
	const auto shows = [&](const std::string& value)
	{
		return app.waitUntil(
			[&] { return attribute(app, ".inspector select[control=midiChannel]", "value") == value; });
	};
	REQUIRE(shows("any"));
	CHECK(selected(app, ".inspector select[control=midiChannel]") == "Any");

	// Any, then channels 1 to 16.
	choose(app, ".inspector select[control=midiChannel]", 10);
	REQUIRE(app.waitUntil([&] { return channel(app).at("midiChannel") == 9; }));
	REQUIRE(shows("9"));
	CHECK(selected(app, ".inspector select[control=midiChannel]") == "10");
	app.call("channel.set", {{"channel", channel(app).at("id")}, {"midiChannel", nullptr}});
	CHECK(shows("any"));
}

TEST_CASE("the track list's MIDI channel control chooses the MIDI channel of a MIDI track", "[ui]")
{
	test::UiApp app{{1200, 600}};
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.call("settings.set",
		{{"key", "ui.trackControls"},
			{"value", {{"midi", {"mute", "solo", "midiChannel"}}, {"audio", {"mute", "midiChannel"}}}}});
	app.call("track.add", {{"type", "midi"}});
	app.call("track.add", {{"type", "audio"}});
	REQUIRE(app.waitUntil([&] { return display.project().at("tracks").size() == 2; }));
	const std::string list = ".track-list select[control=midiChannel]";
	// MIDI tracks only: the audio track's row doesn't show it, although its controls name it.
	REQUIRE(app.waitUntil([&] { return elements(app, ".track-list [control=mute]").size() == 2; }));
	REQUIRE(app.waitUntil([&] { return elements(app, list).size() == 1; }));
	REQUIRE(app.waitUntil([&] { return attribute(app, list, "value") == "any"; }));
	CHECK(selected(app, list) == "Any");

	choose(app, list, 3);
	REQUIRE(app.waitUntil([&] { return channel(app).at("midiChannel") == 2; }));
	CHECK(app.waitUntil([&] { return attribute(app, list, "value") == "2"; }));
	CHECK(selected(app, list) == "3");
	app.call("channel.set", {{"channel", channel(app).at("id")}, {"midiChannel", nullptr}});
	CHECK(app.waitUntil([&] { return attribute(app, list, "value") == "any"; }));
}

TEST_CASE("the MIDI device manager's Bank Assignment gives a MIDI channel one bank in the program selector", "[ui]")
{
	// Reference manual p. 1109: a device with several banks shows the bank assigned to the track's MIDI channel.
	test::UiApp app{{1200, 640}};
	app.appUi().showProject();
	app.appUi().docking().open("inspector");
	app.call("midiDevice.install", {{"name", "Synth"}});
	app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#midi-devices").empty(); }));
	const auto shown = [&]
	{
		const Json classes = elements(app, "#bank-assignment")[0].value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), "unavailable") == classes.end();
	};
	CHECK(!shown()); // a single bank needs none
	app.call("midiDevice.set",
		{{"name", "Synth"},
			{"banks",
				{{{"name", "Patches"}, {"patches", {{{"name", "Piano"}, {"program", 0}}}}},
					{{"name", "Drums"}, {"patches", {{{"name", "Kit"}, {"program", 0}, {"bank", 1}}}}}}}});
	REQUIRE(app.waitUntil(shown));
	CHECK(selected(app, "#assign-bank") == "All Banks");
	choose(app, "#assign-channel", 9);
	choose(app, "#assign-bank", 2);
	REQUIRE(app.waitUntil([&] { return devices(app).at("Synth").value("bankAssignment", Json()) == Json{{"10", 1}}; }));
	choose(app, "#assign-channel", 1); // channel 11 has none
	CHECK(app.waitUntil([&] { return selected(app, "#assign-bank") == "All Banks"; }));
	click(app, "#close");

	const Json added = app.call("track.add", {{"type", "midi"}});
	app.call("channel.set", {{"channel", added.at("channel").at("id")}, {"device", "Synth"}, {"midiChannel", 9}});
	app.appUi().trackList()->select({added.at("track").at("id").get<uint64_t>()});
	const auto programs = [&]
	{
		std::vector<std::string> names;
		for (const Json& option : test::selectOptions(app, ".inspector select[control=program]"))
			names.push_back(option.at("text").get<std::string>());
		return names;
	};
	CHECK(app.waitUntil([&] { return programs() == std::vector<std::string>{"Off", "Kit"}; }));
	app.call("channel.set", {{"channel", added.at("channel").at("id")}, {"midiChannel", 0}});
	CHECK(
		app.waitUntil([&] { return programs() == std::vector<std::string>{"Off", "Patches / Piano", "Drums / Kit"}; }));
	// Removing a bank in the manager keeps the channels on the banks they had.
	app.keys().run("window.midiDeviceManager", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return !elements(app, "#patch-banks listitem[value=bank1]").empty(); }));
	click(app, "#enable-edit");
	click(app, "#patch-banks listitem[value=bank1]");
	click(app, "#remove-patch");
	REQUIRE(app.waitUntil([&] { return devices(app).at("Synth").at("banks").size() == 1; }));
	CHECK(devices(app).at("Synth").value("bankAssignment", Json()) == Json{{"10", 0}});
}
