// SPDX-License-Identifier: GPL-3.0-or-later
// The track list (#81, docs/features/track-list.md) in the app's project window, driven with ui::Automation as MCP's
// UI tools drive it: every control changes the engine's project, every change from elsewhere shows, and tracks are
// selected, renamed, moved, sized, folded, duplicated, disabled and removed.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool hasClass(const Json& element, const std::string& name)
	{
		const Json classes = element.value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
	}

	std::string row(uint64_t track)
	{
		return "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	}

	// The project window with the track list, at a dp ratio of 1.
	struct Tracks
	{
		Tracks() : display(app.appUi().showProject()), list(*app.appUi().trackList())
		{
			app.setDpRatio(1.0F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		uint64_t add(const Json& parameters)
		{
			return app.call("track.add", parameters).at("track").at("id").get<uint64_t>();
		}

		// The object with that id in a list of the project.
		Json find(const char* list, const Json& id)
		{
			const Json objects = app.call("project.get").at(list);
			const auto found = std::find_if(
				objects.begin(), objects.end(), [&id](const Json& object) { return object.at("id") == id; });
			REQUIRE(found != objects.end());
			return *found;
		}

		Json track(uint64_t id) { return find("tracks", id); }
		Json channel(uint64_t trackId) { return find("channels", track(trackId).at("channel")); }

		std::vector<uint64_t> order()
		{
			std::vector<uint64_t> ids;
			const Json project = app.call("project.get");
			for (const Json& track : project.at("tracks"))
				ids.push_back(track.at("id").get<uint64_t>());
			return ids;
		}

		// Waits until the list shows that many rows.
		void rows(size_t count)
		{
			REQUIRE(app.waitUntil([&] { return elements(app, "#track-list listitem").size() == count; }));
			app.shell().step(std::chrono::milliseconds(0));
		}

		void click(const std::string& selector, const std::vector<std::string>& modifiers = {}, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json box(const std::string& selector)
		{
			const Json found = elements(app, selector);
			REQUIRE(found.size() == 1);
			return found[0].at("box");
		}

		test::UiApp app{{1000, 500}};
		ui::EventDisplay& display;
		ui::TrackList& list;
	};
}

TEST_CASE("the track list shows a row per track, and its controls change the tracks' channels", "[ui]")
{
	Tracks tracks;
	const uint64_t audio = tracks.add({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t instrument = tracks.add({{"type", "instrument"}});
	const uint64_t group = tracks.add({{"type", "group"}});
	const uint64_t marker = tracks.add({{"type", "marker"}});
	tracks.rows(4);
	CHECK(elements(tracks.app, row(audio) + " .track-name").at(0).value("text", "") == "Vox");
	// The rows are as high as the event display's lanes and level with them.
	const Json first = tracks.box(row(audio));
	CHECK(first.at("height").get<int>() == static_cast<int>(ui::g_trackHeight));
	CHECK(first.at("y") == elements(tracks.app, "event-lanes.events").at(0).at("box").at("y"));

	// Each type's controls: audio tracks record and monitor, instrument tracks don't, marker tracks have none.
	CHECK(elements(tracks.app, row(audio) + " button[control]").size() == 5);
	CHECK(elements(tracks.app, row(instrument) + " button[control]").size() == 3);
	CHECK(elements(tracks.app, row(instrument) + " button.record").empty());
	CHECK(elements(tracks.app, row(group) + " button.mute").size() == 1);
	CHECK(elements(tracks.app, row(marker) + " button[control]").empty());

	// Every button sets its channel's switch, and shows it.
	for (const char* control : {"mute", "solo", "record", "monitor"})
	{
		INFO(control);
		const std::string key = std::string(control) == "record" ? "recordEnable" : control;
		const std::string button = row(audio) + " button." + control;
		tracks.click(button);
		REQUIRE(tracks.app.waitUntil([&] { return tracks.channel(audio).at(key) == true; }));
		REQUIRE(tracks.app.waitUntil([&] { return hasClass(elements(tracks.app, button).at(0), "checked"); }));
		// From another client, too.
		tracks.app.call("channel.set", {{"channel", tracks.channel(audio).at("id")}, {key, false}});
		REQUIRE(tracks.app.waitUntil([&] { return !hasClass(elements(tracks.app, button).at(0), "checked"); }));
	}

	// Read switches the track's automation lanes; without lanes it's disabled.
	const std::string read = row(instrument) + " button.read";
	CHECK(elements(tracks.app, read).at(0).at("attributes").contains("disabled"));
	const Json lane = tracks.app.call("automationLane.add", {{"track", instrument}, {"target", {{"type", "volume"}}}});
	REQUIRE(tracks.app.waitUntil([&] { return hasClass(elements(tracks.app, read).at(0), "checked"); }));
	tracks.click(read);
	REQUIRE(tracks.app.waitUntil(
		[&]
		{
			return tracks.app.call("automationLane.list", {{"track", instrument}}).at("lanes")[0].at("read") == false;
		}));
	CHECK(lane.at("lane").at("read") == true);
}

TEST_CASE("tracks are selected with clicks and the arrow keys, renamed with a double click and moved by a drag", "[ui]")
{
	Tracks tracks;
	const uint64_t a = tracks.add({{"type", "audio"}, {"name", "A"}});
	const uint64_t b = tracks.add({{"type", "audio"}, {"name", "B"}});
	const uint64_t c = tracks.add({{"type", "midi"}, {"name", "C"}});
	tracks.rows(3);

	tracks.click(row(a) + " .track-name");
	CHECK(tracks.list.selection() == std::vector<uint64_t>{a});
	CHECK(hasClass(elements(tracks.app, row(a)).at(0), "selected"));
	tracks.click(row(c) + " .track-name", {"ctrl"});
	CHECK(tracks.list.selection() == std::vector<uint64_t>{a, c});
	tracks.click(row(a) + " .track-name");
	tracks.click(row(c) + " .track-name", {"shift"});
	CHECK(tracks.list.selection() == std::vector<uint64_t>{a, b, c});

	// The arrow keys step through the tracks, in the list and wherever the project window has the focus.
	tracks.click(row(a) + " .track-name");
	tracks.app.ui().key(std::nullopt, "Down");
	tracks.app.shell().step(std::chrono::milliseconds(0));
	CHECK(tracks.list.selection() == std::vector<uint64_t>{b});
	tracks.app.keys().run("track.selectNext", Json::object(), tracks.app.main(), nullptr);
	CHECK(tracks.list.selection() == std::vector<uint64_t>{c});
	tracks.app.keys().run("track.selectPrevious", Json::object(), tracks.app.main(), nullptr);
	CHECK(tracks.list.selection() == std::vector<uint64_t>{b});

	// A double click on the name renames the track with Enter; Escape keeps the name.
	tracks.click(row(b) + " .track-name", {}, 2);
	REQUIRE(hasClass(elements(tracks.app, row(b)).at(0), "renaming"));
	CHECK(elements(tracks.app, row(b) + " input").at(0).at("visible").get<bool>());
	tracks.app.ui().key(std::nullopt, "Ctrl+A");
	tracks.app.ui().text(std::nullopt, "Bass");
	tracks.app.ui().key(std::nullopt, "Enter");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(b).at("name") == "Bass"; }));
	REQUIRE(tracks.app.waitUntil(
		[&] { return elements(tracks.app, row(b) + " .track-name").at(0).value("text", "") == "Bass"; }));
	tracks.list.rename(c);
	tracks.app.shell().step(std::chrono::milliseconds(0));
	tracks.app.ui().text(std::nullopt, "Strings");
	tracks.app.ui().key(std::nullopt, "Escape");
	tracks.app.shell().step(std::chrono::milliseconds(0));
	CHECK(!hasClass(elements(tracks.app, row(c)).at(0), "renaming"));
	CHECK(tracks.track(c).at("name") == "C");

	// A drag moves the selected tracks: A below the middle of C goes last, in one undo step.
	tracks.click(row(a) + " .track-name");
	const Json from = tracks.box(row(a));
	const Json to = tracks.box(row(c));
	const ui::Point start{from.at("x").get<int>() + 20, from.at("y").get<int>() + 10};
	const ui::Point end{to.at("x").get<int>() + 20, to.at("y").get<int>() + 30};
	tracks.app.ui().drag(std::nullopt, {"", 0, start}, {"", 0, end});
	REQUIRE(tracks.app.waitUntil([&] { return tracks.order() == std::vector<uint64_t>{b, c, a}; }));
	tracks.app.call("history.undo");
	CHECK(tracks.order() == std::vector<uint64_t>{a, b, c});
}

TEST_CASE(
	"track heights follow drags of a row's lower edge, with Ctrl every track's, and the wheel scrolls the list", "[ui]")
{
	Tracks tracks;
	std::vector<uint64_t> ids;
	for (int i = 0; i < 30; ++i)
		ids.push_back(tracks.add({{"type", "audio"}}));
	tracks.rows(30);

	const Json first = tracks.box(row(ids[0]));
	const int bottom = first.at("y").get<int>() + first.at("height").get<int>() - 2;
	const int x = first.at("x").get<int>() + 40;
	tracks.app.ui().drag(std::nullopt, {"", 0, ui::Point{x, bottom}}, {"", 0, ui::Point{x, bottom + 20}});
	CHECK(tracks.display.trackHeight(ids[0]) == Approx(ui::g_trackHeight + 20.0));
	CHECK(tracks.display.trackHeight(ids[1]) == Approx(ui::g_trackHeight));
	tracks.app.shell().step(std::chrono::milliseconds(0));
	CHECK(tracks.box(row(ids[0])).at("height").get<int>() == static_cast<int>(ui::g_trackHeight) + 20);
	CHECK(tracks.display.laneTops()[1] == Approx(ui::g_trackHeight + 20.0));

	const Json second = tracks.box(row(ids[1]));
	const int lower = second.at("y").get<int>() + second.at("height").get<int>() - 2;
	tracks.app.ui().drag(std::nullopt, {"", 0, ui::Point{x, lower}}, {"", 0, ui::Point{x, lower - 10}}, {"ctrl"});
	for (const uint64_t id : ids)
		CHECK(tracks.display.trackHeight(id) == Approx(ui::g_trackHeight - 10.0));

	// The wheel over the list scrolls the event display, and the rows with it.
	const int y = tracks.box(row(ids[0])).at("y").get<int>();
	tracks.app.ui().wheel(std::nullopt, {row(ids[0]), 0, std::nullopt}, 0.0F, 2.0F);
	tracks.app.shell().step(std::chrono::milliseconds(0));
	CHECK(tracks.display.view().top > 0.0);
	CHECK(tracks.box(row(ids[0])).at("y").get<int>() == y - static_cast<int>(tracks.display.view().top));
}

TEST_CASE("folders fold in the list and the event display; the menus duplicate, disable and remove tracks", "[ui]")
{
	Tracks tracks;
	const uint64_t folder = tracks.add({{"type", "folder"}, {"name", "Band"}});
	const uint64_t inside = tracks.add({{"type", "audio"}, {"parent", folder}});
	const uint64_t full = tracks.add({{"type", "audio"}, {"name", "Full"}});
	const uint64_t clip =
		tracks.app
			.call(
				"media.add", {{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 96000}})
			.at("media")
			.at("id")
			.get<uint64_t>();
	tracks.app.call("audioEvent.add",
		{{"track", full}, {"clip", tracks.app.call("clip.add", {{"media", clip}}).at("clip").at("id")},
			{"position", 0}});
	tracks.rows(3);
	CHECK(tracks.display.laneTracks().size() == 3);

	// The folder's toggle folds it: the track inside goes from the list and the event display.
	tracks.click(row(folder) + " .tree-toggle");
	CHECK(tracks.list.folded(folder));
	REQUIRE(tracks.app.waitUntil([&] { return !elements(tracks.app, row(inside)).at(0).at("visible").get<bool>(); }));
	CHECK(tracks.display.laneTracks() == std::vector<uint64_t>{folder, full});
	tracks.app.keys().run("track.unfoldAll", Json::object(), tracks.app.main(), nullptr);
	CHECK(tracks.display.laneTracks().size() == 3);
	tracks.app.keys().run("track.flipFolding", Json::object(), tracks.app.main(), nullptr);
	CHECK(tracks.list.folded(folder));
	tracks.app.keys().run("track.flipFolding", Json::object(), tracks.app.main(), nullptr);
	CHECK(!tracks.list.folded(folder));

	// The context menu, on a right click, works on the selected tracks.
	const auto menu = [&tracks](uint64_t track, const char* command)
	{
		tracks.app.ui().click(
			std::nullopt, {row(track) + " .track-name", 0, std::nullopt}, ui::MouseButton::Right, 1, {});
		tracks.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(elements(tracks.app, "#track-list-menu").at(0).at("visible").get<bool>());
		tracks.click(std::string("#track-list-menu > [command=\"") + command + "\"]");
	};
	menu(full, "track.duplicateSelected");
	tracks.rows(4);
	const std::vector<uint64_t> order = tracks.order();
	REQUIRE(order.size() == 4);
	CHECK(order[2] == full);
	CHECK(tracks.track(order[3]).at("name") == "Full");

	menu(full, "track.disableSelected");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(full).at("enabled") == false; }));
	REQUIRE(tracks.app.waitUntil([&] { return hasClass(elements(tracks.app, row(full)).at(0), "disabled"); }));
	menu(full, "track.enableSelected");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(full).at("enabled") == true; }));

	// Remove Empty Tracks leaves the tracks with events and the folders with tracks; Remove Selected Tracks removes the
	// selection, in one undo step.
	menu(full, "track.removeEmpty");
	tracks.rows(3);
	CHECK(tracks.order() == std::vector<uint64_t>{folder, full, order[3]});
	tracks.click(row(full) + " .track-name");
	tracks.click(row(order[3]) + " .track-name", {"ctrl"});
	tracks.app.keys().run("track.removeSelected", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(1);
	tracks.app.call("history.undo");
	tracks.rows(3);
}

TEST_CASE("the track controls settings dialog chooses and orders each track type's controls", "[ui]")
{
	Tracks tracks;
	const uint64_t audio = tracks.add({{"type", "audio"}});
	const uint64_t midi = tracks.add({{"type", "midi"}});
	tracks.rows(2);
	const auto controls = [&tracks](uint64_t track)
	{
		std::vector<std::string> names;
		for (const Json& button : elements(tracks.app, row(track) + " button[control]"))
			names.push_back(button.at("attributes").at("control").get<std::string>());
		return names;
	};
	CHECK(controls(audio) == std::vector<std::string>{"mute", "solo", "record", "monitor", "read"});

	const auto open = [&tracks] { return !elements(tracks.app, "#track-controls-dialog").empty(); };
	tracks.click("#track-controls-settings");
	REQUIRE(tracks.app.waitUntil(open));
	// Audio: hide Monitor, show Switch Time Base and move it up before Read; Mute stays.
	tracks.click("#visible-controls > [value=monitor]");
	tracks.click("#controls-remove");
	tracks.click("#visible-controls > [value=mute]");
	tracks.click("#controls-remove");
	tracks.click("#hidden-controls > [value=timeBase]");
	tracks.click("#controls-add");
	tracks.click("#controls-up");
	CHECK(elements(tracks.app, "#visible-controls > listitem").size() == 5);
	// MIDI: back to the defaults after hiding Read.
	tracks.click("#controls-types > [value=midi]");
	tracks.click("#visible-controls > [value=read]");
	tracks.click("#controls-remove");
	tracks.click("#controls-reset");
	CHECK(elements(tracks.app, "#visible-controls > listitem").size() == 3);
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return !open(); }));

	REQUIRE(tracks.app.waitUntil(
		[&] { return controls(audio) == std::vector<std::string>{"mute", "solo", "record", "timeBase", "read"}; }));
	CHECK(controls(midi) == std::vector<std::string>{"mute", "solo", "read"});
	CHECK(tracks.app.call("settings.get", {{"key", "ui.trackControls"}}).at("value").at("audio") ==
		Json{"mute", "solo", "record", "timeBase", "read"});
	// Switch Time Base switches the track's time base.
	tracks.click(row(audio) + " button.timeBase");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(audio).at("timeBase") == "linear"; }));
}
