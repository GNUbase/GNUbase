// SPDX-License-Identifier: GPL-3.0-or-later
// Track visibility in the UI (#166, docs/features/track-visibility.md): the Visibility tab's Track page, Sync
// Visibility of Project and Mixer and the Track Visibility Configurations, driven with ui::Automation as MCP's UI tools
// drive them.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/docking.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	bool has(test::UiApp& app, Window window, const std::string& selector, const std::string& name)
	{
		const Json found = elements(app, window, selector);
		return found.size() == 1 && has(found.at(0), name);
	}

	void click(
		test::UiApp& app, Window window, const std::string& selector, const std::vector<std::string>& modifiers = {})
	{
		app.ui().click(window, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
		app.shell().step(std::chrono::milliseconds(0));
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

	std::string item(uint64_t track)
	{
		return ".track-visibility-item[track=\"" + std::to_string(track) + "\"]";
	}

	std::string row(uint64_t track)
	{
		return "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	}

	Json hidden(test::UiApp& app)
	{
		return app.call("trackVisibility.get").at("hidden");
	}

	// The channels a mixer's strips show, in order.
	std::vector<uint64_t> strips(test::UiApp& app, Window window, const std::string& within = "")
	{
		std::vector<uint64_t> ids;
		for (const Json& strip : elements(app, window, within + " .mixer-channel"))
			ids.push_back(std::stoull(strip.at("attributes").at("channel").get<std::string>()));
		return ids;
	}

	bool shows(const std::vector<uint64_t>& ids, uint64_t id)
	{
		return std::find(ids.begin(), ids.end(), id) != ids.end();
	}
}

TEST_CASE("the Visibility tab's Track page hides tracks in the track list and the lower zone's mixer", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	const Added keys = addTrack(app, "instrument", "Keys");
	app.appUi().docking().open("visibility");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, ".track-visibility-item").size() == 3; }));
	CHECK(has(app, std::nullopt, item(bass.track), "checked"));
	CHECK(elements(app, std::nullopt, item(bass.track)).at(0).value("text", "") == "Bass");

	// A click hides a track (reference manual p. 81): the track list and the event display leave it out.
	click(app, std::nullopt, item(bass.track));
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{bass.track}; }));
	REQUIRE(app.waitUntil([&] { return has(app, std::nullopt, row(bass.track), "filtered"); }));
	CHECK(!has(app, std::nullopt, item(bass.track), "checked"));
	CHECK(!has(app, std::nullopt, row(guitar.track), "filtered"));

	// The lower zone's mixer follows (p. 82), and a channel hidden there hides its track.
	app.ui().key(std::nullopt, "Alt+F3");
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("mixer"); }));
	REQUIRE(app.waitUntil([&] { return shows(strips(app, std::nullopt, "#lower-zone"), guitar.channel); }));
	CHECK(!shows(strips(app, std::nullopt, "#lower-zone"), bass.channel));
	app.call("mixer.set", {{"mixer", "lower"}, {"hidden", {keys.channel}}});
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{keys.track}; }));
	REQUIRE(app.waitUntil([&] { return !has(app, std::nullopt, item(keys.track), "checked"); }));
	CHECK(has(app, std::nullopt, item(bass.track), "checked"));

	// Shift-click shows only that track.
	click(app, std::nullopt, item(keys.track), {"shift"});
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{guitar.track, bass.track}; }));
	REQUIRE(app.waitUntil([&] { return !has(app, std::nullopt, row(keys.track), "filtered"); }));
	CHECK(has(app, std::nullopt, row(guitar.track), "filtered"));

	// The tabs at the bottom switch to the lower zone editor's visibility and back (p. 80).
	CHECK(has(app, std::nullopt, ".visibility-page[page=editor]", "hidden"));
	click(app, std::nullopt, ".visibility-tab[page=editor]");
	CHECK(!has(app, std::nullopt, ".visibility-page[page=editor]", "hidden"));
	CHECK(has(app, std::nullopt, ".visibility-page[page=track]", "hidden"));
	CHECK(has(app, std::nullopt, ".visibility-tab[page=editor]", "checked"));
	click(app, std::nullopt, ".visibility-tab[page=track]");
	CHECK(!has(app, std::nullopt, ".visibility-page[page=track]", "hidden"));
}

TEST_CASE("Sync Visibility of Project and Mixer links the track visibility with a mixer window's", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	app.call("trackVisibility.set", {{"hidden", {bass.track}}});
	app.ui().key(std::nullopt, "F3");
	REQUIRE(app.waitUntil([&] { return app.appUi().mixer(1) != nullptr; }));
	const Window mixer = app.appUi().mixerWindow(1);
	REQUIRE(app.waitUntil([&] { return shows(strips(app, mixer), bass.channel); })); // not synced yet

	// The Track page's = menu syncs the first mixer window (reference manual p. 82): it takes the tracks' visibility.
	app.appUi().docking().open("visibility");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, ".track-visibility-item").size() == 2; }));
	click(app, std::nullopt, "#track-visibility-sync");
	click(app, std::nullopt, "#track-visibility-sync-menu menuitem[sync=window1]");
	REQUIRE(app.waitUntil([&] { return app.call("trackVisibility.get").at("sync") == "window1"; }));
	REQUIRE(app.waitUntil([&] { return !shows(strips(app, mixer), bass.channel); }));
	REQUIRE(app.waitUntil([&] { return has(app, std::nullopt, "#track-visibility-sync", "checked"); }));

	// The mixer window's Visibility tab shows the link (p. 442) and hides tracks with their channels.
	app.appUi().docking().setVisible("mixer1", "left", true);
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(app.waitUntil([&] { return has(app, mixer, "#mixer-visibility-sync", "checked"); }));
	click(app, mixer, ".mixer-visibility-item[visibility=" + std::to_string(guitar.channel) + "]");
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{guitar.track, bass.track}; }));
	REQUIRE(app.waitUntil([&] { return has(app, std::nullopt, row(guitar.track), "filtered"); }));

	// Its button switches the link off; the window then keeps its own visibility.
	click(app, mixer, "#mixer-visibility-sync");
	REQUIRE(app.waitUntil([&] { return app.call("trackVisibility.get").at("sync").get<std::string>().empty(); }));
	REQUIRE(app.waitUntil([&] { return !has(app, mixer, "#mixer-visibility-sync", "checked"); }));
	app.call("trackVisibility.set", {{"hidden", Json::array()}});
	REQUIRE(app.waitUntil([&] { return !has(app, std::nullopt, row(guitar.track), "filtered"); }));
	CHECK(!shows(strips(app, mixer), guitar.channel));
}

TEST_CASE("Track Visibility Configurations add, apply, update, rename and delete configurations", "[ui]")
{
	test::UiApp app({1200, 700});
	// The toolbar has room for the configurations at this dp ratio, also where CI's Linux display makes the window 1024
	// pixels wide.
	app.setDpRatio(0.8F);
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	const auto configurations = [&app] { return app.call("trackVisibility.get").at("configurations"); };
	const auto text = [&app]
	{ return elements(app, std::nullopt, "#track-visibility-configurations").at(0).value("text", ""); };
	const auto choose = [&app](const std::string& selector)
	{
		click(app, std::nullopt, "#track-visibility-configurations");
		click(app, std::nullopt, "#track-visibility-configurations-menu " + selector);
	};
	app.call("trackVisibility.set", {{"hidden", {bass.track}}});

	// Add Configuration: the tracks hidden now, under the name typed (reference manual p. 455).
	choose("menuitem[visibility-configuration=add]");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, "#visibility-configuration-name").size() == 1; }));
	app.ui().text(std::nullopt, "No Bass");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return configurations().size() == 1; }));
	CHECK(configurations().at(0) == Json{{"name", "No Bass"}, {"hidden", {bass.track}}});
	REQUIRE(app.waitUntil([&] { return text() == "No Bass"; }));

	// A change shows with an asterisk; Update Configuration takes it.
	app.call("trackVisibility.set", {{"hidden", {guitar.track, bass.track}}});
	REQUIRE(app.waitUntil([&] { return text() == "No Bass *"; }));
	choose("menuitem[visibility-configuration=update]");
	REQUIRE(app.waitUntil([&] { return configurations().at(0).at("hidden") == Json{guitar.track, bass.track}; }));
	REQUIRE(app.waitUntil([&] { return text() == "No Bass"; }));

	// Choosing a configuration from the menu applies it.
	app.call("trackVisibility.set",
		{{"configurations", {configurations().at(0), {{"name", "All"}, {"hidden", Json::array()}}}},
			{"configuration", "All"}});
	REQUIRE(app.waitUntil([&] { return text() == "All"; }));
	choose("menuitem[configuration=\"No Bass\"]");
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{guitar.track, bass.track}; }));

	// Rename Configuration only renames the active one, also while it is modified; Delete Configuration deletes it.
	app.call("trackVisibility.set", {{"hidden", {bass.track}}});
	REQUIRE(app.waitUntil([&] { return text() == "No Bass *"; }));
	choose("menuitem[visibility-configuration=rename]");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, "#visibility-configuration-name").size() == 1; }));
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Nothing");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return configurations().at(0).at("name") == "Nothing"; }));
	REQUIRE(app.waitUntil([&] { return text() == "Nothing *"; }));
	CHECK(hidden(app) == Json{bass.track});
	CHECK(configurations().at(0).at("hidden") == Json{guitar.track, bass.track});
	choose("menuitem[visibility-configuration=delete]");
	REQUIRE(app.waitUntil([&] { return configurations().size() == 1; }));
	CHECK(configurations().at(0).at("name") == "All");
	REQUIRE(app.waitUntil([&] { return text() == "Configurations"; }));
}

TEST_CASE("the Track page keeps working after a hidden track is removed", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	app.appUi().docking().open("visibility");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, ".track-visibility-item").size() == 2; }));
	click(app, std::nullopt, item(bass.track));
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{bass.track}; }));

	// Undoing its addition removes the hidden track; the next click mustn't send its id, which the engine refuses.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, ".track-visibility-item").size() == 1; }));
	click(app, std::nullopt, item(guitar.track));
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{guitar.track}; }));

	// A frame can take project.get's answer after the undo before trackVisibility.get's, with the visibility from
	// before it still in hand (#907, #1366): an event with that visibility stands in for it here. The click sends only
	// the tracks that are there.
	app.engine().registerCommand({"test.staleVisibility", 1, "Announces the track visibility from before the undo.",
									 engine::CommandInfo{}.parameters, Json::object(), false},
		[gone = bass.track](engine::CommandContext& context)
		{
			context.emit("trackVisibility.changed",
				{{"hidden", {gone}}, {"sync", ""}, {"configurations", Json::array()}, {"configuration", ""},
					{"modified", false}});
			return Json::object();
		});
	app.call("trackVisibility.set", {{"hidden", Json::array()}});
	app.call("test.staleVisibility");
	app.shell().step(std::chrono::milliseconds(0));
	click(app, std::nullopt, item(guitar.track));
	REQUIRE(app.waitUntil([&] { return hidden(app) == Json{guitar.track}; }));
}

// #906: a name that trackVisibility.set would refuse keeps the dialog open with the reason.
TEST_CASE("the configuration name dialog stays open for a name that is taken or empty", "[ui]")
{
	test::UiApp app({1200, 700});
	app.setDpRatio(0.8F);
	app.appUi().showProject();
	const auto configurations = [&app] { return app.call("trackVisibility.get").at("configurations"); };
	const auto choose = [&app](const std::string& selector)
	{
		click(app, std::nullopt, "#track-visibility-configurations");
		click(app, std::nullopt, "#track-visibility-configurations-menu " + selector);
	};
	const auto dialogOpen = [&app]
	{ return elements(app, std::nullopt, "#visibility-configuration-name").size() == 1; };
	const auto error = [&app]
	{ return elements(app, std::nullopt, "#visibility-configuration-error").at(0).value("text", ""); };
	app.call("trackVisibility.set",
		{{"configurations", {{{"name", "One"}, {"hidden", Json::array()}}}}, {"configuration", "One"}});
	REQUIRE(app.waitUntil([&] { return configurations().size() == 1; }));

	// Add Configuration with a name that is taken: the dialog stays open with the reason, and nothing is added.
	choose("menuitem[visibility-configuration=add]");
	REQUIRE(app.waitUntil(dialogOpen));
	app.ui().text(std::nullopt, "One");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return !error().empty(); }));
	CHECK_THAT(error(), Catch::Matchers::ContainsSubstring("One"));
	CHECK(dialogOpen());
	CHECK(configurations().size() == 1);

	// With a name of its own it closes and adds the configuration.
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Two");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return configurations().size() == 2; }));
	REQUIRE(app.waitUntil([&] { return !dialogOpen(); }));

	// Rename Configuration to the other's name stays open too; to its own it closes and changes nothing.
	REQUIRE(app.waitUntil([&] { return configurations().at(1).at("name") == "Two"; }));
	choose("menuitem[visibility-configuration=rename]");
	REQUIRE(app.waitUntil(dialogOpen));
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "One");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return !error().empty(); }));
	CHECK(dialogOpen());
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Two");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return !dialogOpen(); }));
	CHECK(configurations().at(1).at("name") == "Two");

	// A name that is empty keeps it open as well; Cancel closes it without a change.
	choose("menuitem[visibility-configuration=add]");
	REQUIRE(app.waitUntil(dialogOpen));
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return !error().empty(); }));
	CHECK(dialogOpen());
	app.ui().key(std::nullopt, "Escape");
	REQUIRE(app.waitUntil([&] { return !dialogOpen(); }));
	CHECK(configurations().size() == 2);
}

// #906: the Track page indents tracks by their folder level, however deep the folders nest.
TEST_CASE("the Track page indents tracks by their folder level, however deep", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	// Five folders, one in the next, and a track in the last: the levels 0 to 5.
	std::vector<uint64_t> folders;
	uint64_t parent = 0;
	for (int depth = 1; depth <= 5; ++depth)
	{
		Json parameters = {{"type", "folder"}, {"name", "Folder " + std::to_string(depth)}};
		if (parent != 0)
		{
			parameters["parent"] = parent;
			parameters["index"] = 0;
		}
		parent = app.call("track.add", parameters).at("track").at("id").get<uint64_t>();
		folders.push_back(parent);
	}
	const uint64_t deep = app.call("track.add", {{"type", "audio"}, {"name", "Deep"}, {"parent", parent}, {"index", 0}})
							  .at("track")
							  .at("id")
							  .get<uint64_t>();
	app.appUi().docking().open("visibility");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, ".track-visibility-item").size() == 6; }));
	const auto indent = [&app](uint64_t track)
	{
		const Json found = elements(app, std::nullopt, item(track) + " .track-visibility-indent");
		return found.empty() ? 0.0 : found.at(0).at("box").at("width").get<double>();
	};
	CHECK(indent(folders.at(0)) == 0.0); // the top level has no indent
	double previous = 0.0;
	for (const uint64_t track : std::vector<uint64_t>(folders.begin() + 1, folders.end()))
	{
		CHECK(indent(track) > previous);
		previous = indent(track);
	}
	CHECK(indent(deep) > previous);
	// The levels step alike: the deepest track indents five times as much as the first folder's track.
	CHECK(std::abs(indent(deep) - (5.0 * indent(folders.at(1)))) <= 1.0);
}
