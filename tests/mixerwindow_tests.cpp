// SPDX-License-Identifier: GPL-3.0-or-later
// The mixer windows and the lower zone's mixer (#106, docs/features/mixer-window.md) in the app, driven with
// ui::Automation as MCP's UI tools drive them. tests/mixerviews_tests.cpp checks what the engine keeps.
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/keycommands.h"
#include "ui/renderer.h"
#include "ui/shell.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/Types.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	using Window = std::optional<ui::WindowId>;

	Json elements(test::UiApp& app, Window window, const std::string& selector)
	{
		return app.ui().dom(window, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, Window window, const std::string& selector)
	{
		const Json found = elements(app, window, selector);
		REQUIRE(found.size() == 1);
		return found.at(0);
	}

	void click(test::UiApp& app, Window window, const std::string& selector, int index = 0)
	{
		app.ui().click(window, {selector, index, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	// The channels the strips show, in order.
	std::vector<uint64_t> strips(test::UiApp& app, Window window, const std::string& within = "")
	{
		std::vector<uint64_t> ids;
		for (const Json& strip : elements(app, window, within + " .mixer-channel"))
			ids.push_back(std::stoull(strip.at("attributes").at("channel").get<std::string>()));
		return ids;
	}

	std::string strip(uint64_t channel)
	{
		return ".mixer-channel[channel=" + std::to_string(channel) + "]";
	}

	struct Added
	{
		uint64_t track = 0;
		uint64_t channel = 0;
	};

	Added addTrack(test::UiApp& app, const std::string& type, const std::string& name)
	{
		const Json added = app.call("track.add", {{"type", type}, {"name", name}});
		return {added.at("track").at("id").get<uint64_t>(), added.at("channel").at("id").get<uint64_t>()};
	}

	uint64_t bus(test::UiApp& app, const std::string& direction)
	{
		const Json project = app.call("project.get");
		for (const Json& item : project.at("busses"))
			if (item.at("direction") == direction)
				return item.at("id").get<uint64_t>();
		FAIL("no " << direction << " bus");
	}

	Json channelJson(test::UiApp& app, uint64_t id)
	{
		const Json project = app.call("project.get");
		for (const Json& item : project.at("channels"))
			if (item.at("id") == id)
				return item;
		FAIL("no channel " << id);
	}

	// The element the selector finds in the window's documents.
	Rml::Element* findElement(test::UiApp& app, Window window, const std::string& selector)
	{
		Rml::Context& context = app.shell().find(window.value_or(0))->context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* found = context.GetDocument(i)->QuerySelector(selector))
				return found;
		return nullptr;
	}

	ui::WindowId openMixer(test::UiApp& app, int number)
	{
		app.keys().run(number == 1 ? std::string("window.mixer") : "window.mixer" + std::to_string(number),
			Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&] { return app.appUi().mixer(number) != nullptr; }));
		return app.appUi().mixerWindow(number);
	}
}

TEST_CASE("a mixer window opens with F3 and follows the track list live", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added keys = addTrack(app, "midi", "Keys");
	const uint64_t input = bus(app, "input");
	const uint64_t output = bus(app, "output");

	// F3 and Studio > Mixer (reference manual p. 437).
	CHECK(elements(app, std::nullopt, "#studio-menu menuitem[command=\"window.mixer\"]").size() == 1);
	CHECK(app.keys().keysFor("window.mixer") == std::vector<std::string>{"F3"});
	app.ui().key(std::nullopt, "F3");
	REQUIRE(app.waitUntil([&] { return app.appUi().mixer(1) != nullptr; }));
	const Window window = app.appUi().mixerWindow(1);
	REQUIRE(app.waitUntil(
		[&] { return strips(app, window) == std::vector<uint64_t>{input, guitar.channel, keys.channel, output}; }));
	CHECK(element(app, window, strip(guitar.channel) + " .mixer-channel-name").at("text") == "Guitar");
	CHECK(element(app, window, strip(guitar.channel)).at("attributes").at("type") == "audio");
	CHECK(element(app, window, strip(keys.channel)).at("attributes").at("type") == "midi");
	// MIDI channels have mute and solo but no fader; busses neither.
	CHECK(elements(app, window, strip(keys.channel) + " fader").empty());
	CHECK(elements(app, window, strip(keys.channel) + " button[control=mute]").size() == 1);
	CHECK(elements(app, window, strip(input) + " button").empty());

	// Tracks that come, change, move and go show at once, whichever client changes them.
	const Added group = addTrack(app, "group", "Drums Bus");
	REQUIRE(app.waitUntil(
		[&]
		{
			return strips(app, window) ==
				std::vector<uint64_t>{input, guitar.channel, keys.channel, group.channel, output};
		}));
	app.call("track.set", {{"track", guitar.track}, {"name", "Lead Guitar"}});
	REQUIRE(app.waitUntil([&]
		{ return element(app, window, strip(guitar.channel) + " .mixer-channel-name").at("text") == "Lead Guitar"; }));
	app.call("track.move", {{"track", group.track}, {"parent", 0}, {"index", 0}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return strips(app, window) ==
				std::vector<uint64_t>{input, group.channel, guitar.channel, keys.channel, output};
		}));
	app.call("track.remove", {{"track", keys.track}});
	REQUIRE(app.waitUntil(
		[&] { return strips(app, window) == std::vector<uint64_t>{input, group.channel, guitar.channel, output}; }));

	// The fader and the buttons send channel.set, and show the engine's values.
	app.call("channel.set", {{"channel", guitar.channel}, {"volume", -6.0}});
	REQUIRE(app.waitUntil(
		[&] { return element(app, window, strip(guitar.channel) + " .mixer-volume").at("text") == "-6.0"; }));
	CHECK(element(app, window, strip(guitar.channel) + " fader").at("attributes").at("value") == "-6");
	app.ui().drag(window, {strip(guitar.channel) + " .fader-cap", 0, std::nullopt},
		{strip(guitar.channel) + " .mixer-volume", 0, std::nullopt});
	REQUIRE(app.waitUntil([&] { return channelJson(app, guitar.channel).at("volume").get<double>() < -30.0; }));
	// The drag is one undo step.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&] { return channelJson(app, guitar.channel).at("volume") == -6.0; }));
	click(app, window, strip(guitar.channel) + " button[control=mute]");
	REQUIRE(app.waitUntil([&] { return channelJson(app, guitar.channel).at("mute") == true; }));
	REQUIRE(app.waitUntil(
		[&] { return has(element(app, window, strip(guitar.channel) + " button[control=mute]"), "checked"); }));
	app.call("channel.set", {{"channel", guitar.channel}, {"solo", true}});
	REQUIRE(app.waitUntil(
		[&] { return has(element(app, window, strip(guitar.channel) + " button[control=solo]"), "checked"); }));

	// F3 again closes it.
	app.ui().key(window, "F3");
	REQUIRE(app.waitUntil([&] { return app.appUi().mixer(1) == nullptr; }));
	CHECK(app.shell().find(*window) == nullptr);
}

TEST_CASE("a channel strip's name shows its track's color, which the style sheets don't know", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Json added = app.call("track.add", {{"type", "audio"}, {"name", "Guitar"}, {"color", "#204060"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	const auto channel = added.at("channel").at("id").get<uint64_t>();
	const Added keys = addTrack(app, "midi", "Keys");
	const uint64_t output = bus(app, "output");
	const Window window = openMixer(app, 1);
	const std::string guitar = strip(channel) + " .mixer-channel-name";
	REQUIRE(app.waitUntil([&] { return elements(app, window, guitar).size() == 1; }));

	// The track's color is user data: code sets it as the name's background color (reference manual p. 1621), and
	// the classes tell the style sheets that a color shows and whether it's bright, for the text over it.
	CHECK(element(app, window, guitar).at("attributes").at("color") == "#204060");
	CHECK(has(element(app, window, guitar), "colored"));
	CHECK(!has(element(app, window, guitar), "bright"));
	Rml::Element* name = findElement(app, window, guitar);
	REQUIRE(name != nullptr);
	REQUIRE(name->GetLocalProperty("background-color") != nullptr);
	CHECK(name->GetLocalProperty("background-color")->Get<Rml::Colourb>() == Rml::Colourb(0x20, 0x40, 0x60, 255));

	// Tracks without a color and busses show the theme's.
	for (const uint64_t other : {keys.channel, output})
	{
		const std::string selector = strip(other) + " .mixer-channel-name";
		CHECK(!has(element(app, window, selector), "colored"));
		Rml::Element* plain = findElement(app, window, selector);
		REQUIRE(plain != nullptr);
		CHECK(plain->GetLocalProperty("background-color") == nullptr);
	}

	// The strip follows the track's color, whichever client changes it.
	app.call("track.set", {{"track", track}, {"color", "#f0e060"}});
	REQUIRE(app.waitUntil([&] { return element(app, window, guitar).at("attributes").at("color") == "#f0e060"; }));
	CHECK(has(element(app, window, guitar), "bright"));
	name = findElement(app, window, guitar);
	REQUIRE(name != nullptr);
	REQUIRE(name->GetLocalProperty("background-color") != nullptr);
	CHECK(name->GetLocalProperty("background-color")->Get<Rml::Colourb>() == Rml::Colourb(0xf0, 0xe0, 0x60, 255));
	app.call("track.set", {{"track", keys.track}, {"color", "#e12ad8"}});
	REQUIRE(app.waitUntil(
		[&] { return has(element(app, window, strip(keys.channel) + " .mixer-channel-name"), "colored"); }));

	// Back to the default color: the theme's again.
	app.call("track.set", {{"track", track}, {"color", nullptr}});
	REQUIRE(app.waitUntil([&] { return !has(element(app, window, guitar), "colored"); }));
	CHECK(!element(app, window, guitar).at("attributes").contains("color"));
	CHECK(!has(element(app, window, guitar), "bright"));
	name = findElement(app, window, guitar);
	REQUIRE(name != nullptr);
	CHECK(name->GetLocalProperty("background-color") == nullptr);
}

TEST_CASE("the lower zone's mixer in the default theme looks as its golden image", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	// Strips with dark and bright track colors, one without, a group, and the busses; a muted and a soloed channel.
	const Json guitar = app.call("track.add", {{"type", "audio"}, {"name", "Guitar"}, {"color", "#e12ad8"}});
	const Json vocals = app.call("track.add", {{"type", "audio"}, {"name", "Vocals"}, {"color", "#10dff2"}});
	addTrack(app, "midi", "Keys");
	app.call("track.add", {{"type", "group"}, {"name", "Drums"}, {"color", "#8248ef"}});
	app.call("channel.set", {{"channel", guitar.at("channel").at("id")}, {"mute", true}, {"volume", -6.0}});
	app.call("channel.set", {{"channel", vocals.at("channel").at("id")}, {"solo", true}, {"pan", -0.5}});
	app.ui().key(std::nullopt, "Alt+F3");
	REQUIRE(app.waitUntil([&] { return strips(app, std::nullopt, "#lower-zone").size() == 6; }));
	app.appUi().docking().setSize("project", "lower", 320); // tall enough for the whole strips
	REQUIRE(app.waitUntil(
		[&]
		{
			return has(
				element(app, std::nullopt,
					"#lower-zone " + strip(guitar.at("channel").at("id").get<uint64_t>()) + " button[control=mute]"),
				"checked");
		}));
	app.shell().step(std::chrono::milliseconds(0));

	// The strips only, so that changes elsewhere in the window or the zone don't touch this image.
	const ui::Image window = app.screenshot("lower-zone-mixer-window", {1200, 700});
	const Json box = element(app, std::nullopt, "#lower-zone .mixer-channels").at("box");
	const int x = box.at("x").get<int>();
	const int y = box.at("y").get<int>();
	const int width = box.at("width").get<int>();
	const int height = box.at("height").get<int>();
	REQUIRE(x >= 0);
	REQUIRE(y >= 0);
	REQUIRE(x + width <= window.width);
	REQUIRE(y + height <= window.height);
	ui::Image zone{width, height, {}};
	for (int row = y; row < y + height; ++row)
	{
		const auto begin = window.pixels.begin() + (((static_cast<std::ptrdiff_t>(row) * window.width) + x) * 4);
		zone.pixels.insert(zone.pixels.end(), begin, begin + (static_cast<std::ptrdiff_t>(width) * 4));
	}
	test::UiApp::checkGolden("lower-zone-mixer", zone);
}

TEST_CASE("each mixer window and the lower zone's mixer hide channels of their own", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added piano = addTrack(app, "instrument", "Piano");
	const uint64_t input = bus(app, "input");
	const uint64_t output = bus(app, "output");
	const std::vector<uint64_t> all = {input, guitar.channel, piano.channel, output};
	const Window first = openMixer(app, 1);
	const Window second = openMixer(app, 2);
	CHECK(first != second);
	REQUIRE(app.waitUntil([&] { return strips(app, first) == all && strips(app, second) == all; }));

	// The Visibility tab in the first window's left zone hides a channel there only (reference manual p. 442).
	app.appUi().docking().setVisible("mixer1", "left", true);
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(app.waitUntil([&] { return elements(app, first, ".mixer-visibility-item").size() == all.size(); }));
	CHECK(has(
		element(app, first, ".mixer-visibility-item[visibility=" + std::to_string(piano.channel) + "]"), "checked"));
	click(app, first, ".mixer-visibility-item[visibility=" + std::to_string(piano.channel) + "]");
	REQUIRE(app.waitUntil([&] { return strips(app, first) == std::vector<uint64_t>{input, guitar.channel, output}; }));
	CHECK(app.call("mixer.get", {{"mixer", "window1"}}).at("hidden") == Json{piano.channel});
	CHECK(!has(
		element(app, first, ".mixer-visibility-item[visibility=" + std::to_string(piano.channel) + "]"), "checked"));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(strips(app, second) == all);

	// The channel type filter (p. 454) hides every channel of a type; its button shows that one is hidden.
	CHECK(!has(element(app, second, "#mixer-filter"), "checked"));
	click(app, second, "#mixer-filter");
	click(app, second, "#mixer-filter-menu menuitem[channel-type=audio]");
	REQUIRE(app.waitUntil([&] { return strips(app, second) == std::vector<uint64_t>{input, piano.channel, output}; }));
	CHECK(app.call("mixer.get", {{"mixer", "window2"}}).at("hiddenTypes") == Json{"audio"});
	CHECK(has(element(app, second, "#mixer-filter"), "checked"));
	CHECK(!has(element(app, second, "#mixer-filter-menu menuitem[channel-type=audio]"), "checked"));
	app.call("mixer.set", {{"mixer", "window2"}, {"hiddenTypes", {"input", "output"}}});
	REQUIRE(app.waitUntil([&] { return strips(app, second) == std::vector<uint64_t>{guitar.channel, piano.channel}; }));

	// The visibility agents' Show All Channels (p. 456) shows the hidden channels again.
	click(app, first, "#mixer-agents");
	click(app, first, "#mixer-agents-menu menuitem[agent=showAll]");
	REQUIRE(app.waitUntil([&] { return strips(app, first) == all; }));

	// The lower zone's mixer (pp. 434-436): Alt+F3 and Studio > Mixer in Project Window show it, with a view of its
	// own; again hides the zone.
	CHECK(elements(app, std::nullopt, "#studio-menu menuitem[command=\"window.lowerZoneMixer\"]").size() == 1);
	// Opt+F3 on macOS.
	CHECK(
		app.keys().keysFor("window.lowerZoneMixer") == std::vector<std::string>{ui::keyName(ui::parseKeys("Alt+F3"))});
	app.ui().key(std::nullopt, "Alt+F3");
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("mixer"); }));
	REQUIRE(app.appUi().lowerZoneMixer() != nullptr);
	REQUIRE(app.waitUntil([&] { return strips(app, std::nullopt, "#lower-zone") == all; }));
	CHECK(elements(app, std::nullopt, "#lower-zone #mixer-left-zone")
			.empty()); // the project window's left zone isn't its
	app.call("mixer.set", {{"mixer", "lower"}, {"hidden", {output}}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return strips(app, std::nullopt, "#lower-zone") ==
				std::vector<uint64_t>{input, guitar.channel, piano.channel};
		}));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(strips(app, first) == all);
	app.ui().key(std::nullopt, "Alt+F3");
	REQUIRE(app.waitUntil([&] { return !app.appUi().docking().visible("project", "lower"); }));

	// A window the user closed lets go of its mixer.
	app.shell().close(*second);
	REQUIRE(app.waitUntil([&] { return app.appUi().mixer(2) == nullptr; }));
}

TEST_CASE("mixer channels show notes and pictures, and a channel dragged onto another moves its track", "[ui]")
{
	const test::TempDir dir;
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added a = addTrack(app, "audio", "A");
	const Added b = addTrack(app, "audio", "B");
	const Added c = addTrack(app, "audio", "C");
	const Window window = openMixer(app, 1);
	REQUIRE(app.waitUntil([&] { return strips(app, window).size() == 5; }));

	// Set up Window Layout (p. 439): the pictures and the notepad are hidden until the pane shows them.
	click(app, window, "#mixer-layout");
	CHECK(has(element(app, window, "#mixer-layout-pane"), "open"));
	for (const char* section : {"picture", "notes", "fader"})
		CHECK(elements(app, window, std::string("#mixer-layout-pane [setup-section=") + section + "]").size() == 1);
	CHECK(has(element(app, window, strip(a.channel) + " .mixer-notes"), "hidden"));
	CHECK(!has(element(app, window, strip(a.channel) + " .mixer-fader"), "hidden"));
	click(app, window, "#mixer-layout-pane [setup-section=notes]");
	click(app, window, "#mixer-layout-pane [setup-section=picture]");
	REQUIRE(app.waitUntil([&] { return !has(element(app, window, strip(a.channel) + " .mixer-notes"), "hidden"); }));
	CHECK(!has(element(app, window, strip(c.channel) + " .mixer-picture"), "hidden"));
	app.ui().key(window, "Escape");

	// Notes (p. 497) from the engine, and typed into the notepad, sent when it loses the focus.
	app.call("channel.set", {{"channel", b.channel}, {"notes", "Room mic"}});
	REQUIRE(app.waitUntil([&]
		{ return element(app, window, strip(b.channel) + " .mixer-notes-field").value("value", "") == "Room mic"; }));
	click(app, window, strip(a.channel) + " .mixer-notes-field");
	app.ui().text(window, "Bass DI");
	click(app, window, strip(c.channel) + " .mixer-channel-name");
	REQUIRE(app.waitUntil([&] { return channelJson(app, a.channel).at("notes") == "Bass DI"; }));

	// A track's picture (p. 497) shows in its channel's picture section.
	const std::string picture = (dir.path() / "guitar.png").generic_u8string();
	ui::Image image;
	image.width = 8;
	image.height = 8;
	image.pixels.assign(size_t{8} * 8 * 4, 200);
	REQUIRE(ui::writePng(image, dir.path() / "guitar.png"));
	app.call("track.set", {{"track", c.track}, {"picture", picture}});
	REQUIRE(app.waitUntil([&] { return elements(app, window, strip(c.channel) + " .mixer-picture img").size() == 1; }));
	CHECK(element(app, window, strip(c.channel) + " .mixer-picture img").at("attributes").at("src") == picture);
	app.call("track.set", {{"track", c.track}, {"picture", ""}});
	REQUIRE(app.waitUntil([&] { return elements(app, window, strip(c.channel) + " .mixer-picture img").empty(); }));

	// A channel's name dragged onto another channel takes its place (p. 496): its track moves there.
	app.ui().drag(window, {strip(a.channel) + " .mixer-channel-name", 0, std::nullopt},
		{strip(c.channel) + " .mixer-channel-name", 0, std::nullopt});
	const auto order = [&]
	{
		std::vector<uint64_t> ids;
		const Json project = app.call("project.get");
		for (const Json& track : project.at("tracks"))
			ids.push_back(track.at("id").get<uint64_t>());
		return ids;
	};
	REQUIRE(app.waitUntil([&] { return order() == std::vector<uint64_t>{b.track, c.track, a.track}; }));
	REQUIRE(app.waitUntil([&] { return strips(app, window).at(3) == a.channel; }));
	app.ui().drag(window, {strip(a.channel) + " .mixer-channel-name", 0, std::nullopt},
		{strip(b.channel) + " .mixer-channel-name", 0, std::nullopt});
	REQUIRE(app.waitUntil([&] { return order() == std::vector<uint64_t>{a.track, b.track, c.track}; }));
	// Busses keep their places.
	app.ui().drag(
		window, {".mixer-channel-name", 0, std::nullopt}, {strip(c.channel) + " .mixer-channel-name", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(order() == std::vector<uint64_t>{a.track, b.track, c.track});
}

TEST_CASE("the mixer's setup pane saves and recalls presets, and Reset All brings the default order back", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added a = addTrack(app, "audio", "A");
	const Window window = openMixer(app, 1);
	REQUIRE(app.waitUntil([&] { return strips(app, window).size() == 3; }));
	ui::Docking& docking = app.appUi().docking();
	const std::vector<std::string> defaults = docking.sections("mixer1", "channel");
	REQUIRE(defaults.size() > 1);
	const auto openPane = [&app, &window]
	{
		if (!has(element(app, window, "#mixer-layout-pane"), "open"))
			click(app, window, "#mixer-layout");
		REQUIRE(has(element(app, window, "#mixer-layout-pane"), "open"));
	};
	const auto notesShown = [&] { return !has(element(app, window, strip(a.channel) + " .mixer-notes"), "hidden"); };

	// The notes shown and the last section first.
	openPane();
	click(app, window, "#mixer-layout-pane [setup-section=notes]");
	REQUIRE(app.waitUntil(notesShown));
	docking.moveSection("mixer1", "channel", defaults.back(), defaults.front());
	const std::vector<std::string> moved = docking.sections("mixer1", "channel");
	REQUIRE(moved.front() == defaults.back());

	// Save Preset, in the mixer window.
	openPane();
	click(app, window, "#mixer-layout-pane [setup-preset-save=channel]");
	REQUIRE(app.waitUntil([&] { return !elements(app, window, "#setup-preset-name").empty(); }));
	click(app, window, "#setup-preset-name");
	app.ui().text(window, "Notes");
	REQUIRE(
		app.waitUntil([&] { return !element(app, window, "#setup-preset-ok").at("attributes").contains("disabled"); }));
	click(app, window, "#setup-preset-ok");
	REQUIRE(app.waitUntil(
		[&app] { return app.call("settings.get", {{"key", "ui.setupPresets"}}).at("value").contains("channel"); }));

	// Reset All: the default order and sections.
	openPane();
	click(app, window, "#mixer-layout-pane [setup-reset=channel]");
	REQUIRE(app.waitUntil([&] { return !notesShown(); }));
	CHECK(docking.sections("mixer1", "channel") == defaults);

	// The preset brings its order and sections back.
	openPane();
	click(app, window, "#mixer-layout-pane [setup-preset=Notes]");
	REQUIRE(app.waitUntil(notesShown));
	CHECK(docking.sections("mixer1", "channel") == moved);
}

TEST_CASE("the channel strip's pre, equalizer and strip sections and the panner send channel.set", "[ui]")
{
	test::UiApp app({1200, 900});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added piano = addTrack(app, "midi", "Piano");
	const Window window = openMixer(app, 1);
	REQUIRE(app.waitUntil([&] { return strips(app, window).size() == 4; }));
	const auto control = [&](const std::string& param) { return strip(guitar.channel) + " [param=\"" + param + "\"]"; };

	// The fader section has the panner (reference manual p. 465); MIDI channels don't.
	app.ui().wheel(window, {control("pan"), 0, std::nullopt}, 0.0F, -10.0F);
	REQUIRE(app.waitUntil([&] { return channelJson(app, guitar.channel).at("pan") > 0.0; }));
	CHECK(elements(app, window, strip(piano.channel) + " .mixer-pan").empty());

	// Set up Window Layout shows the sections, which are hidden until then; MIDI channels' are empty.
	click(app, window, "#mixer-layout");
	for (const std::string section : {"pre", "eq", "strip"})
	{
		const std::string inStrip = strip(guitar.channel) + " [section=" + section + "]";
		CHECK(elements(app, window, "#mixer-layout-pane [setup-section=" + section + "]").size() == 1);
		CHECK(has(element(app, window, inStrip), "hidden"));
		click(app, window, "#mixer-layout-pane [setup-section=" + section + "]");
		REQUIRE(app.waitUntil([&] { return !has(element(app, window, inStrip), "hidden"); }));
		CHECK(elements(app, window, strip(piano.channel) + " [section=" + section + "] [param]").empty());
	}
	app.ui().key(window, "Escape");

	// The toggles and knobs send channel.set.
	for (const char* param : {"pre.phase", "pre.lowCut.on", "eq.bands.3.on", "strip.compressor.on"})
		click(app, window, control(param));
	REQUIRE(
		app.waitUntil([&] { return channelJson(app, guitar.channel).at("strip").at("compressor").at("on") == true; }));
	const Json channel = channelJson(app, guitar.channel);
	CHECK(channel.at("pre").at("phase") == true);
	CHECK(channel.at("pre").at("lowCut").at("on") == true);
	CHECK(channel.at("eq").at("bands")[2].at("on") == true);
	REQUIRE(app.waitUntil([&] { return has(element(app, window, control("pre.phase")), "checked"); }));
	app.ui().wheel(window, {control("eq.bands.3.gain"), 0, std::nullopt}, 0.0F, -5.0F);
	REQUIRE(app.waitUntil([&] { return channelJson(app, guitar.channel).at("eq").at("bands")[2].at("gain") > 0.0; }));

	// And they show what the engine has, whoever changed it.
	app.call("channel.set",
		{{"channel", guitar.channel}, {"pre", {{"gain", 6.5}, {"phase", false}}},
			{"eq", {{"bands", {{{"band", 1}, {"frequency", 250.0}}}}}}});
	REQUIRE(
		app.waitUntil([&] { return element(app, window, control("pre.gain")).at("attributes").at("value") == "6.5"; }));
	CHECK(element(app, window, control("eq.bands.1.frequency")).at("attributes").at("value") == "250");
	CHECK(!has(element(app, window, control("pre.phase")), "checked"));
}

TEST_CASE(
	"the sends section picks a send's destination, switches it on and before the fader and sets its level", "[ui]")
{
	test::UiApp app({1200, 900});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added reverb = addTrack(app, "effect", "Reverb");
	const Window window = openMixer(app, 1);
	REQUIRE(app.waitUntil([&] { return strips(app, window).size() == 4; }));
	const auto control = [&](uint64_t channel, const std::string& param)
	{ return strip(channel) + " [param=\"" + param + "\"]"; };
	click(app, window, "#mixer-layout");
	click(app, window, "#mixer-layout-pane [setup-section=sends]");
	REQUIRE(app.waitUntil(
		[&] { return !has(element(app, window, strip(guitar.channel) + " [section=sends]"), "hidden"); }));
	app.ui().key(window, "Escape");
	const auto send = [&] { return channelJson(app, guitar.channel).at("sends")[1]; };

	// The destinations are No Destination and the group and effect channels: Reverb comes next.
	click(app, window, control(guitar.channel, "sends.2.destination"));
	app.ui().key(window, "Down");
	click(app, window, control(guitar.channel, "sends.2.destination"));
	REQUIRE(app.waitUntil([&] { return send().at("destination") == reverb.channel; }));
	click(app, window, control(guitar.channel, "sends.2.on"));
	click(app, window, control(guitar.channel, "sends.2.preFader"));
	REQUIRE(app.waitUntil([&] { return send().at("preFader") == true; }));
	CHECK(send().at("on") == true);
	app.ui().wheel(window, {control(guitar.channel, "sends.2.level"), 0, std::nullopt}, 0.0F, 5.0F);
	REQUIRE(app.waitUntil([&] { return send().at("level") < 0.0; }));
	// Ctrl-click gives it the default send level (reference manual p. 552).
	app.call("settings.set", {{"key", "audio.defaultSendLevel"}, {"value", -12.0}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return element(app, window, control(guitar.channel, "sends.2.level"))
					   .at("attributes")
					   .value("default", "") == "-12";
		}));
	app.ui().click(
		window, {control(guitar.channel, "sends.2.level"), 0, std::nullopt}, ui::MouseButton::Left, 1, {"ctrl"});
	REQUIRE(app.waitUntil([&] { return send().at("level") == -12.0; }));
	// The knob reaches every default send level that track.add gives a send: up to the project's maximum volume and
	// below the knob's usual -60 dB.
	app.call("project.set", {{"maxVolume", 12.0}});
	app.call("settings.set", {{"key", "audio.defaultSendLevel"}, {"value", 10.0}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return element(app, window, control(guitar.channel, "sends.2.level")).at("attributes").value("max", "") ==
				"12";
		}));
	app.ui().click(
		window, {control(guitar.channel, "sends.2.level"), 0, std::nullopt}, ui::MouseButton::Left, 1, {"ctrl"});
	REQUIRE(app.waitUntil([&] { return send().at("level") == 10.0; }));
	app.call("settings.set", {{"key", "audio.defaultSendLevel"}, {"value", -80.0}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return element(app, window, control(guitar.channel, "sends.2.level")).at("attributes").value("min", "") ==
				"-80";
		}));
	app.ui().click(
		window, {control(guitar.channel, "sends.2.level"), 0, std::nullopt}, ui::MouseButton::Left, 1, {"ctrl"});
	REQUIRE(app.waitUntil([&] { return send().at("level") == -80.0; }));

	// What another client changes shows.
	app.call(
		"channel.set", {{"channel", guitar.channel}, {"sends", {{{"send", 2}, {"destination", 0}, {"on", false}}}}});
	REQUIRE(app.waitUntil(
		[&]
		{
			return element(app, window, control(guitar.channel, "sends.2.destination")).at("attributes").at("value") ==
				"0";
		}));
	CHECK(!has(element(app, window, control(guitar.channel, "sends.2.on")), "checked"));
}

TEST_CASE(
	"solo, mute and listen in the mixer: exclusive solo, solo defeat, listen and the Deactivate All buttons", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	const Added keys = addTrack(app, "midi", "Keys");
	const Window window = openMixer(app, 1);
	REQUIRE(app.waitUntil([&] { return strips(app, window).size() == 5; }));
	const auto button = [&](const Added& added, const char* control)
	{ return element(app, window, strip(added.channel) + " button[control=" + control + "]"); };
	const auto toolbar = [&](const char* state)
	{ return element(app, window, std::string(".mixer-toolbar button[deactivate=") + state + "]"); };
	const auto clickWith = [&](const std::string& selector, const char* modifier)
	{
		app.ui().click(window, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {modifier});
		app.shell().step(std::chrono::milliseconds(0));
	};
	// Audio channels listen; MIDI channels have no listen button.
	CHECK(elements(app, window, strip(keys.channel) + " button[control=listen]").empty());
	CHECK_FALSE(has(toolbar("solo"), "checked"));

	// A click solos, Ctrl-click solos exclusively (reference manual p. 466).
	click(app, window, strip(guitar.channel) + " button[control=solo]");
	REQUIRE(app.waitUntil([&] { return has(toolbar("solo"), "checked"); }));
	CHECK(channelJson(app, guitar.channel).at("solo") == true);
	clickWith(strip(bass.channel) + " button[control=solo]", "ctrl");
	REQUIRE(app.waitUntil([&] { return channelJson(app, bass.channel).at("solo") == true; }));
	CHECK(channelJson(app, guitar.channel).at("solo") == false);
	// Alt-click switches solo defeat, which the button shows.
	clickWith(strip(guitar.channel) + " button[control=solo]", "alt");
	REQUIRE(app.waitUntil([&] { return has(button(guitar, "solo"), "defeated"); }));
	CHECK(channelJson(app, guitar.channel).at("soloDefeat") == true);
	CHECK_FALSE(has(button(guitar, "solo"), "checked"));
	// Deactivate All Solo States takes both.
	click(app, window, ".mixer-toolbar button[deactivate=solo]");
	REQUIRE(app.waitUntil([&] { return !has(toolbar("solo"), "checked"); }));
	CHECK(channelJson(app, bass.channel).at("solo") == false);
	CHECK(channelJson(app, guitar.channel).at("soloDefeat") == false);

	// Listen and mute, and their Deactivate All buttons.
	click(app, window, strip(guitar.channel) + " button[control=listen]");
	click(app, window, strip(keys.channel) + " button[control=mute]");
	REQUIRE(app.waitUntil([&] { return has(button(guitar, "listen"), "checked") && has(toolbar("mute"), "checked"); }));
	CHECK(channelJson(app, guitar.channel).at("listen") == true);
	CHECK(has(toolbar("listen"), "checked"));
	click(app, window, ".mixer-toolbar button[deactivate=listen]");
	click(app, window, ".mixer-toolbar button[deactivate=mute]");
	REQUIRE(app.waitUntil([&] { return !has(toolbar("listen"), "checked") && !has(toolbar("mute"), "checked"); }));
	CHECK(channelJson(app, guitar.channel).at("listen") == false);
	CHECK(channelJson(app, keys.channel).at("mute") == false);
}

namespace
{
	double valueOf(test::UiApp& app, Window window, const std::string& selector)
	{
		return test::controlValue(app, window, selector);
	}

	int64_t historyPosition(test::UiApp& app)
	{
		return app.call("history.get").at("position").get<int64_t>();
	}

	// Shows the strips' sends section.
	void showSends(test::UiApp& app, Window window, uint64_t channel)
	{
		click(app, window, "#mixer-layout");
		click(app, window, "#mixer-layout-pane [setup-section=sends]");
		REQUIRE(
			app.waitUntil([&] { return !has(element(app, window, strip(channel) + " [section=sends]"), "hidden"); }));
		app.ui().key(window, "Escape");
	}
}

TEST_CASE("mixer controls changed while the engine answers late follow the mouse, as one undo step", "[ui]")
{
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	addTrack(app, "effect", "Reverb");
	const Window window = openMixer(app, 1);
	const std::string fader = strip(guitar.channel) + " fader";
	REQUIRE(app.waitUntil([&] { return elements(app, window, fader).size() == 1; }));
	const auto channel = [&] { return channelJson(app, guitar.channel); };

	SECTION("the fader")
	{
		const int64_t position = historyPosition(app);
		test::LateDrag drag(app, window, fader);
		const double dragged = drag.move(30, 1);
		CHECK(dragged < -5.0);
		drag.release();
		// Then the control and the engine agree, on where the mouse left it, in one undo step.
		REQUIRE(app.waitUntil([&] { return channel().at("volume") == Catch::Approx(dragged).margin(0.001); }));
		CHECK(valueOf(app, window, fader) == Catch::Approx(dragged).margin(0.001));
		const std::string text =
			element(app, window, strip(guitar.channel) + " .mixer-volume").at("text").get<std::string>();
		CHECK(std::stod(text) == Catch::Approx(dragged).margin(0.051));
		CHECK(historyPosition(app) == position + 1);
	}

	SECTION("the fader moved faster than the engine answers")
	{
		const auto sent = [&]
		{
			const Json commands = app.call("engine.commandLog").at("commands");
			return std::count_if(commands.begin(), commands.end(),
				[](const Json& command) { return command.at("command") == "history.transaction"; });
		};
		const auto before = sent();
		const int64_t position = historyPosition(app);
		Rml::Context& context = app.shell().find(window.value_or(0))->context();
		const Json box = element(app, window, fader).at("box");
		const int x = box.at("x").get<int>() + (box.at("width").get<int>() / 2);
		const int y = box.at("y").get<int>() + (box.at("height").get<int>() / 2);
		context.ProcessMouseMove(x, y, 0);
		context.ProcessMouseButtonDown(0, 0);
		int moved = 0;
		// Three moves for each answer: the newest waits, replacing the one before.
		const double dragged = test::lateSteps(app, window, fader, 10, 1,
			[&]
			{
				for (int move = 0; move < 3; ++move)
				{
					moved += 2;
					context.ProcessMouseMove(x, y + moved, 0);
				}
			});
		context.ProcessMouseButtonUp(0, 0);
		REQUIRE(app.waitUntil([&] { return channel().at("volume") == Catch::Approx(dragged).margin(0.001); }));
		CHECK(valueOf(app, window, fader) == Catch::Approx(dragged).margin(0.001));
		// Not one for each of the 30 moves: at most two for each round of three, one sent at once if nothing was in
		// flight, and the waiting one, the newest of the round, once the engine answered.
		CHECK(sent() - before <= 20);
		CHECK(historyPosition(app) == position + 1);
	}

	SECTION("the panner's wheel turns")
	{
		const std::string pan = strip(guitar.channel) + " [param=pan]";
		const int64_t position = historyPosition(app);
		const double turned = test::lateSteps(
			app, window, pan, 10, 1, [&] { app.ui().wheel(window, {pan, 0, std::nullopt}, 0.0F, 1.0F); });
		CHECK(turned == Catch::Approx(-0.1).margin(1e-5));
		REQUIRE(app.waitUntil([&] { return channel().at("pan") == Catch::Approx(turned).margin(1e-5); }));
		CHECK(historyPosition(app) == position + 1);
	}

	SECTION("a send's level")
	{
		showSends(app, window, guitar.channel);
		const std::string knob = strip(guitar.channel) + " [param=\"sends.1.level\"]";
		const int64_t position = historyPosition(app);
		test::LateDrag drag(app, window, knob);
		const double dragged = drag.move(30, 1);
		CHECK(dragged < -5.0);
		drag.release();
		REQUIRE(app.waitUntil(
			[&] { return channel().at("sends")[0].at("level") == Catch::Approx(dragged).margin(0.001); }));
		CHECK(valueOf(app, window, knob) == Catch::Approx(dragged).margin(0.001));
		CHECK(historyPosition(app) == position + 1);
	}
}

TEST_CASE("a mixer control shows what another client sets at once, and during a drag once the drag ends", "[ui]")
{
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	addTrack(app, "effect", "Reverb");
	const Window window = openMixer(app, 1);
	const std::string fader = strip(guitar.channel) + " fader";
	const std::string pan = strip(guitar.channel) + " [param=pan]";
	REQUIRE(app.waitUntil([&] { return elements(app, window, fader).size() == 1; }));
	showSends(app, window, guitar.channel);
	const std::string knob = strip(guitar.channel) + " [param=\"sends.1.level\"]";

	// Without a gesture, a value set through MCP shows at once.
	app.call(
		"channel.set", {{"channel", guitar.channel}, {"volume", -12.0}, {"sends", {{{"send", 1}, {"level", -9}}}}});
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, fader) == -12.0; }));
	CHECK(valueOf(app, window, knob) == -9.0);
	CHECK(element(app, window, strip(guitar.channel) + " .mixer-volume").at("text") == "-12.0");

	// During a drag the fader stays where the mouse put it; it shows the engine's value when the drag ends.
	test::LateDrag drag(app, window, fader);
	const double dragged = drag.move(5, -1);
	CHECK(dragged > -12.0);
	app.call("channel.set", {{"channel", guitar.channel}, {"volume", -20.0}, {"pan", 0.5}});
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, pan) == 0.5; })); // the mixer has the project
	CHECK(valueOf(app, window, fader) == Catch::Approx(dragged).margin(0.001));
	const std::string text =
		element(app, window, strip(guitar.channel) + " .mixer-volume").at("text").get<std::string>();
	CHECK(std::stod(text) == Catch::Approx(dragged).margin(0.051)); // the dB text with it
	drag.release(false);
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, fader) == -20.0; }));
	CHECK(channelJson(app, guitar.channel).at("volume") == -20.0);
	CHECK(element(app, window, strip(guitar.channel) + " .mixer-volume").at("text") == "-20.0");
}

TEST_CASE("the other views of a channel follow a control dragged while the engine answers late", "[ui]")
{
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	app.appUi().docking().open("inspector");
	app.appUi().trackList()->select({guitar.track});
	const Window first = openMixer(app, 1);
	const Window second = openMixer(app, 2);
	const Window main = std::nullopt;
	const std::string fader = strip(guitar.channel) + " fader";
	const std::string field = ".inspector valuefield[control=volume]";
	REQUIRE(app.waitUntil(
		[&]
		{
			return elements(app, first, fader).size() == 1 && elements(app, second, fader).size() == 1 &&
				elements(app, main, field).size() == 1;
		}));
	using View = std::pair<Window, std::string>;

	// Only the control that sends waits for the engine. Moved faster than the engine answers, it always has a step on
	// its way, and the other views still show the steps the engine took during the drag (#716).
	const auto drag = [&](const View& dragged, const std::vector<View>& others)
	{
		const double before = valueOf(app, dragged.first, dragged.second);
		test::LateDrag drag(app, dragged.first, dragged.second);
		const double moved = drag.move(5, 1, 3);
		REQUIRE(moved < before);
		for (const View& other : others)
		{
			INFO(other.second);
			CHECK(valueOf(app, other.first, other.second) < before);
		}
		drag.release();
		REQUIRE(app.waitUntil(
			[&] { return channelJson(app, guitar.channel).at("volume") == Catch::Approx(moved).margin(1e-4); }));
		for (const View& other : others)
			CHECK(app.waitUntil(
				[&] { return valueOf(app, other.first, other.second) == Catch::Approx(moved).margin(1e-4); }));
	};
	SECTION("the inspector's volume")
	{
		drag({main, field}, {{first, fader}, {second, fader}});
	}
	SECTION("a mixer's fader")
	{
		drag({first, fader}, {{second, fader}, {main, field}});
	}
}

TEST_CASE("a VCA strip has a fader and buttons and picks the link group it controls; strips show their group", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	const Added vca = addTrack(app, "vca", "Band");
	const uint64_t group = app.call("linkGroup.add", {{"channels", {guitar.channel, bass.channel}}, {"name", "Rhythm"}})
							   .at("group")
							   .at("id")
							   .get<uint64_t>();
	const Window window = openMixer(app, 1);
	REQUIRE(app.waitUntil([&] { return strips(app, window).size() == 5; }));
	CHECK(element(app, window, strip(vca.channel)).at("attributes").at("type") == "vca");

	// The display line shows the number and the name of the link group (reference manual p. 458).
	REQUIRE(app.waitUntil(
		[&] { return element(app, window, strip(guitar.channel) + " .mixer-link").at("text") == "1 Rhythm"; }));
	CHECK(element(app, window, strip(vca.channel) + " .mixer-link").value("text", std::string()).empty());

	// A VCA fader has a fader, M, S and L, and no panner (p. 515).
	CHECK(elements(app, window, strip(vca.channel) + " fader").size() == 1);
	CHECK(elements(app, window, strip(vca.channel) + " .mixer-pan").empty());
	CHECK(elements(app, window, strip(vca.channel) + " button[control=listen]").size() == 1);

	// VCA Channel Controls Link Group: None, then the groups.
	const std::string select = strip(vca.channel) + " .mixer-vca-group";
	CHECK(element(app, window, select).at("attributes").at("value") == "0");
	click(app, window, select);
	app.ui().key(window, "Down");
	click(app, window, select);
	REQUIRE(app.waitUntil([&] { return app.call("project.get").at("linkGroups").at(0).at("vca") == vca.channel; }));

	// Its mute goes on to the group's channels.
	click(app, window, strip(vca.channel) + " button[control=mute]");
	REQUIRE(app.waitUntil(
		[&] { return has(element(app, window, strip(bass.channel) + " button[control=mute]"), "checked"); }));
	CHECK(channelJson(app, guitar.channel).at("mute") == true);

	// What another client changes shows.
	app.call("linkGroup.set", {{"group", group}, {"vca", 0}});
	REQUIRE(app.waitUntil([&] { return element(app, window, select).at("attributes").at("value") == "0"; }));
}

TEST_CASE("a mixer step the engine refuses shows the engine's value again and leaves no undo step", "[ui]")
{
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Window window = openMixer(app, 1);
	const std::string fader = strip(guitar.channel) + " fader";
	REQUIRE(app.waitUntil([&] { return elements(app, window, fader).size() == 1; }));
	app.call("channel.set", {{"channel", guitar.channel}, {"volume", -12.0}});
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, fader) == -12.0; }));
	const int64_t position = historyPosition(app);

	// Two wheel turns: the first step goes, the second waits for its answer. Removing the track in between makes the
	// engine refuse the second: the channel its command changes is gone.
	app.ui().wheel(window, {fader, 0, std::nullopt}, 0.0F, 1.0F);
	app.ui().wheel(window, {fader, 0, std::nullopt}, 0.0F, 1.0F);
	app.call("track.remove", {{"track", guitar.track}});
	REQUIRE(app.waitUntil([&] { return elements(app, window, strip(guitar.channel)).empty(); }));
	// The refused step shows in the command log, as the inspector's refused change does.
	REQUIRE(app.waitUntil(
		[&]
		{
			const Json commands = app.call("engine.commandLog").at("commands");
			return std::any_of(commands.begin(), commands.end(), [](const Json& command)
				{ return command.at("command") == "history.transaction" && command.contains("error"); });
		}));

	// Undo brings the track back: the strip is built again and its fader shows the engine's volume, the value the
	// first turn took, not the refused second's and not the widget's default.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&] { return elements(app, window, fader).size() == 1; }));
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, fader) == Catch::Approx(-12.1).margin(1e-4); }));
	CHECK(channelJson(app, guitar.channel).at("volume").get<double>() == Catch::Approx(-12.1).margin(1e-4));
	CHECK(historyPosition(app) == position + 1); // the gesture's one step; the refused one left none
}

TEST_CASE("a strip made again while its fader's step is in flight shows the engine's volume at once", "[ui]")
{
	test::UiApp app({1024, 600});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Window window = openMixer(app, 1);
	const std::string fader = strip(guitar.channel) + " fader";
	REQUIRE(app.waitUntil([&] { return elements(app, window, fader).size() == 1; }));
	app.call("channel.set", {{"channel", guitar.channel}, {"volume", -12.0}});
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, fader) == -12.0; }));
	const int64_t position = historyPosition(app);

	// A slow query, sent just before the shell step: the engine runs everything the UI sends after it only once it is
	// through, so the step hands on just what is queued already, the wheel turns' steps and the visibility changes,
	// and none of the project reads they bring on their way. What the rebuilt strip shows can be checked then.
	app.engine().registerCommand(
		{"test.slowRead", 1, "Reads slowly.", engine::CommandInfo{}.parameters, Json::object(), true},
		[](engine::CommandContext&)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			return Json::object();
		});

	// Two wheel turns: the first step goes and runs, the second waits for the first's answer. Hiding and showing the
	// output bus makes every strip again while the second step is still in flight, since its answer waits behind the
	// slow query.
	app.ui().wheel(window, {fader, 0, std::nullopt}, 0.0F, 1.0F);
	app.ui().wheel(window, {fader, 0, std::nullopt}, 0.0F, 1.0F);
	REQUIRE(app.appUi().binding().waiting("history.transaction") == 1);
	const uint64_t output = bus(app, "output");
	app.call("mixer.set", {{"mixer", "window1"}, {"hidden", {output}}});
	app.call("mixer.set", {{"mixer", "window1"}, {"hidden", Json::array()}});
	engine::Client(app.engine()).send("test.slowRead");
	app.shell().step(std::chrono::milliseconds(0));

	// The new fader has no value of its own, so it shows the engine's volume as the mixer's project has it, right
	// away, instead of the widget's default of 0 until the step in flight is answered (the HasAttribute exceptions
	// in Mixer::updateStrip).
	REQUIRE(app.waitUntil([&] { return elements(app, window, fader).size() == 1; }));
	CHECK(valueOf(app, window, fader) == -12.0);

	// Once the slow query is through, the fader follows the answered step and the engine and the control agree.
	REQUIRE(app.waitUntil([&] { return valueOf(app, window, fader) == Catch::Approx(-12.2).margin(1e-4); }));
	CHECK(channelJson(app, guitar.channel).at("volume").get<double>() == Catch::Approx(-12.2).margin(1e-4));
	CHECK(historyPosition(app) == position + 1); // the gesture's one undo step; visibility isn't one
}
