// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI devices and patch banks (#136, docs/features/midi-devices.md) in the app's UI, driven with ui::Automation as
// MCP's UI tools drive it: the inspector chooses a MIDI track's patch, and the MIDI device manager installs, shows and
// removes devices, all through engine commands.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

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

	// Opens a drop-down list, moves the selection down that many options with the arrow key, and closes it. Its
	// options aren't in the DOM that the UI tools see.
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
		Rml::Element* element = nullptr;
		for (int i = 0; element == nullptr && i < app.main().context().GetNumDocuments(); ++i)
			element = app.main().context().GetDocument(i)->QuerySelector(selector);
		auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(element);
		REQUIRE(select != nullptr);
		const Rml::Element* option = select->GetOption(select->GetSelection());
		REQUIRE(option != nullptr);
		return option->GetInnerRML();
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
	REQUIRE(app.waitUntil([&] { return display.project().is_object() && display.project().at("tracks").size() == 1; }));
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

TEST_CASE("the inspector chooses the MIDI channel a MIDI track's patch goes out on", "[ui]")
{
	test::UiApp app{{1200, 600}};
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.appUi().docking().open("inspector");
	app.shell().step(std::chrono::milliseconds(0));
	const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return display.project().is_object() && display.project().at("tracks").size() == 1; }));
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
