// SPDX-License-Identifier: GPL-3.0-or-later
// The mixer windows and the lower zone's mixer (#106, docs/features/mixer-window.md) in the app, driven with
// ui::Automation as MCP's UI tools drive them. tests/mixerviews_tests.cpp checks what the engine keeps.
#include "ui/automation.h"
#include "ui/renderer.h"
#include "uiapp.h"

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
	CHECK(app.keys().keysFor("window.lowerZoneMixer") == std::vector<std::string>{"Alt+F3"});
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
