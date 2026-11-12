// SPDX-License-Identifier: GPL-3.0-or-later
// The track list (#81, docs/features/track-list.md) in the app's project window, driven with ui::Automation as MCP's
// UI tools drive it: every control changes the engine's project, every change from elsewhere shows, and tracks are
// selected, renamed, moved, sized, folded, duplicated, disabled and removed.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

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

	std::string menuItem(const std::string& command)
	{
		return "#track-list-menu > [command=\"" + command + "\"]";
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
		void rows(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(5))
		{
			REQUIRE(app.waitUntil([&] { return elements(app, "#track-list listitem").size() == count; }, timeout));
			app.shell().step(std::chrono::milliseconds(0));
		}

		void click(const std::string& selector, const std::vector<std::string>& modifiers = {}, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void rightClick(uint64_t track)
		{
			app.ui().click(std::nullopt, {row(track) + " .track-name", 0, std::nullopt}, ui::MouseButton::Right, 1, {});
		}

		// Waits until the track list's context menu shows.
		void menuOpen()
		{
			REQUIRE(app.waitUntil([&] { return elements(app, "#track-list-menu").at(0).at("visible").get<bool>(); }));
		}

		bool itemDisabled(const std::string& command)
		{
			return elements(app, menuItem(command)).at(0).at("attributes").contains("disabled");
		}

		// Chooses the item of the open menu once it is enabled: it follows rows built again behind the menu when the
		// shell steps.
		void choose(const std::string& command)
		{
			REQUIRE(app.waitUntil([&] { return !itemDisabled(command); }));
			click(menuItem(command));
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

	// Each type's controls: audio and instrument tracks record and monitor, groups don't, marker tracks have their
	// own (markertrack_ui_tests.cpp).
	CHECK(elements(tracks.app, row(audio) + " button[control]").size() == 5);
	CHECK(elements(tracks.app, row(instrument) + " button[control]").size() == 5);
	CHECK(elements(tracks.app, row(instrument) + " button.record").size() == 1);
	CHECK(elements(tracks.app, row(group) + " button.mute").size() == 1);
	CHECK(elements(tracks.app, row(group) + " button.record").empty());
	CHECK(elements(tracks.app, row(marker) + " button[control]").size() == 3);
	CHECK(elements(tracks.app, row(marker) + " button.mute").empty());

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
	// An instrument track's record button arms it for MIDI recording.
	tracks.click(row(instrument) + " button.record");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.channel(instrument).at("recordEnable") == true; }));

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
	// Shift-click on a selected track deselects it (reference manual p. 219); on one that isn't selected, it selects
	// the range from the last one clicked.
	tracks.click(row(b) + " .track-name", {"shift"});
	CHECK(tracks.list.selection() == std::vector<uint64_t>{a, c});
	CHECK(!hasClass(elements(tracks.app, row(b)).at(0), "selected"));
	tracks.click(row(b) + " .track-name", {"shift"});
	CHECK(tracks.list.selection() == std::vector<uint64_t>{a, b});

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
	"Up and Down step the tracks and select their first parts, also from the event display; Alt+F2 renames", "[ui]")
{
	Tracks tracks;
	const uint64_t a = tracks.add({{"type", "midi"}, {"name", "A"}});
	const uint64_t b = tracks.add({{"type", "midi"}, {"name", "B"}}); // empty
	const uint64_t c = tracks.add({{"type", "midi"}, {"name", "C"}});
	const auto part = [&tracks](uint64_t track, int64_t position)
	{
		return tracks.app.call("midiPart.add", {{"track", track}, {"position", position}, {"length", 4 * g_quarter}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = part(a, 0);
	const uint64_t later = part(c, 8 * g_quarter); // added first, but the first part on C is the earlier one
	const uint64_t earlier = part(c, 0);
	tracks.rows(3);
	REQUIRE(tracks.app.waitUntil([&] { return tracks.display.project().at("midiParts").size() == 3; }));
	using Ids = std::vector<uint64_t>;

	// A click on A's part gives the event display the focus; the keys go to the project window's key commands.
	tracks.click(row(a) + " .track-name");
	const Json lanes = elements(tracks.app, "event-lanes.events").at(0).at("box");
	const Json rowA = tracks.box(row(a));
	tracks.app.ui().click(std::nullopt,
		{"", 0, ui::Point{lanes.at("x").get<int>() + 10, rowA.at("y").get<int>() + (rowA.at("height").get<int>() / 2)}},
		ui::MouseButton::Left, 1, {});
	tracks.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(tracks.display.selection() == Ids{first});
	const auto press = [&tracks](const char* keys)
	{
		tracks.app.ui().key(std::nullopt, keys);
		tracks.app.shell().step(std::chrono::milliseconds(0));
	};

	// Down selects B and keeps the part selected, as B has none; then C and its first part, by position.
	press("Down");
	CHECK(tracks.list.selection() == Ids{b});
	CHECK(tracks.display.selection() == Ids{first});
	press("Down");
	CHECK(tracks.list.selection() == Ids{c});
	CHECK(tracks.display.selection() == Ids{earlier});
	press("Up");
	press("Up");
	CHECK(tracks.list.selection() == Ids{a});
	CHECK(tracks.display.selection() == Ids{first});

	// In the track list, too.
	tracks.click(row(b) + " .track-name");
	CHECK(tracks.display.selection() == Ids{first});
	press("Down");
	CHECK(tracks.list.selection() == Ids{c});
	CHECK(tracks.display.selection() == Ids{earlier});
	// Shift extends the list's selection, and selects no parts.
	press("Shift+Up");
	CHECK(tracks.list.selection() == Ids{b, c});
	CHECK(tracks.display.selection() == Ids{earlier});

	// Without a selected part, they only select tracks.
	tracks.display.select({});
	press("Up");
	CHECK(tracks.list.selection() == Ids{a});
	CHECK(tracks.display.selection().empty());

	// With the user setting ui.upDownSelectsTracksOnly they leave the parts alone (p. 218).
	tracks.app.call("settings.set", {{"key", "ui.upDownSelectsTracksOnly"}, {"value", true}});
	REQUIRE(tracks.app.waitUntil([&] { return tracks.list.upDownSelectsTracksOnly(); }));
	tracks.display.select({later});
	press("Down");
	press("Down");
	CHECK(tracks.list.selection() == Ids{c});
	CHECK(tracks.display.selection() == Ids{later});
	press("Up");
	CHECK(tracks.list.selection() == Ids{b});
	CHECK(tracks.display.selection() == Ids{later});
	tracks.app.call("settings.set", {{"key", "ui.upDownSelectsTracksOnly"}, {"value", false}});
	REQUIRE(tracks.app.waitUntil([&] { return !tracks.list.upDownSelectsTracksOnly(); }));

	// Alt+F2 renames the selected track, also with the focus in the event display.
	tracks.click(row(a) + " .track-name");
	tracks.app.ui().click(std::nullopt,
		{"", 0, ui::Point{lanes.at("x").get<int>() + 10, rowA.at("y").get<int>() + (rowA.at("height").get<int>() / 2)}},
		ui::MouseButton::Left, 1, {});
	tracks.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(tracks.display.selection() == Ids{first});
	press("Alt+F2");
	REQUIRE(hasClass(elements(tracks.app, row(a)).at(0), "renaming"));
	tracks.app.ui().text(std::nullopt, "Lead");
	press("Enter");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(a).at("name") == "Lead"; }));
}

TEST_CASE(
	"Remove Selected Tracks warns before it removes tracks that aren't empty, unless the user turned it off", "[ui]")
{
	Tracks tracks;
	const uint64_t empty = tracks.add({{"type", "audio"}, {"name", "Empty"}});
	const uint64_t full = tracks.add({{"type", "midi"}, {"name", "Full"}});
	const uint64_t folder = tracks.add({{"type", "folder"}, {"name", "Band"}});
	const uint64_t inside = tracks.add({{"type", "midi"}, {"parent", folder}});
	for (const uint64_t track : {full, inside})
		tracks.app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 1000}});
	tracks.rows(4);
	const auto warning = [&tracks] { return elements(tracks.app, "#remove-tracks-dialog"); };
	const auto remove = [&tracks]
	{
		tracks.app.keys().run("track.removeSelected", Json::object(), tracks.app.main(), nullptr);
		tracks.app.shell().step(std::chrono::milliseconds(0));
	};
	CHECK(tracks.list.warnsBeforeRemoving());

	// Empty tracks go without a warning.
	tracks.click(row(empty) + " .track-name");
	remove();
	tracks.rows(3);
	CHECK(warning().empty());

	// A track with a part: the warning, and Cancel keeps it.
	tracks.click(row(full) + " .track-name");
	remove();
	REQUIRE(warning().size() == 1);
	CHECK(warning().at(0).at("visible").get<bool>());
	tracks.click("#remove-tracks-dialog button[result=\"cancel\"]");
	REQUIRE(tracks.app.waitUntil([&] { return warning().empty(); }));
	CHECK(tracks.order() == std::vector<uint64_t>{full, folder, inside});
	// Remove removes it, in one undo step.
	remove();
	tracks.click("#remove-tracks");
	tracks.rows(2);
	CHECK(warning().empty());
	tracks.app.call("history.undo");
	tracks.rows(3);

	// A folder warns for what the tracks inside it hold; "Don't warn again" turns the user setting off.
	tracks.click(row(folder) + " .track-name");
	remove();
	REQUIRE(warning().size() == 1);
	tracks.click("#remove-tracks-dont-warn");
	tracks.click("#remove-tracks");
	tracks.rows(1);
	CHECK(tracks.app.call("settings.get", {{"key", "ui.removeTracksWarning"}}).at("value") == false);
	REQUIRE(tracks.app.waitUntil([&] { return !tracks.list.warnsBeforeRemoving(); }));

	// Then tracks that aren't empty go without one.
	tracks.click(row(full) + " .track-name");
	remove();
	tracks.rows(0);
	CHECK(warning().empty());
	tracks.app.call("history.undo");
	tracks.rows(1);

	// Turned on again, by any client, it warns again.
	tracks.app.call("settings.set", {{"key", "ui.removeTracksWarning"}, {"value", true}});
	REQUIRE(tracks.app.waitUntil([&] { return tracks.list.warnsBeforeRemoving(); }));
	tracks.click(row(full) + " .track-name");
	remove();
	CHECK(warning().size() == 1);
	tracks.app.ui().key(std::nullopt, "Escape");
	REQUIRE(tracks.app.waitUntil([&] { return warning().empty(); }));
	CHECK(tracks.order() == std::vector<uint64_t>{full});
}

TEST_CASE("Remove Selected Tracks warns for every kind of content, each on a track that holds only that", "[ui]")
{
	Tracks tracks;
	const uint64_t event = tracks.add({{"type", "audio"}, {"name", "Event"}});
	const uint64_t media =
		tracks.app
			.call(
				"media.add", {{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 96000}})
			.at("media")
			.at("id")
			.get<uint64_t>();
	tracks.app.call("audioEvent.add",
		{{"track", event}, {"clip", tracks.app.call("clip.add", {{"media", media}}).at("clip").at("id")},
			{"position", 0}});
	const uint64_t audioPart = tracks.add({{"type", "audio"}, {"name", "Audio part"}});
	tracks.app.call("audioPart.add", {{"track", audioPart}, {"position", 0}, {"length", 1000}});
	const uint64_t midiPart = tracks.add({{"type", "midi"}, {"name", "MIDI part"}});
	tracks.app.call("midiPart.add", {{"track", midiPart}, {"position", 0}, {"length", 1000}});
	const uint64_t markers = tracks.add({{"type", "marker"}, {"name", "Markers"}});
	tracks.app.call("marker.add", {{"track", markers}, {"position", 0}});
	const uint64_t automated = tracks.add({{"type", "audio"}, {"name", "Automation"}});
	tracks.app.call("automationLane.add", {{"track", automated}, {"target", {{"type", "volume"}}}});
	tracks.rows(5);
	const std::vector<uint64_t> all = tracks.order();
	const auto warning = [&tracks] { return elements(tracks.app, "#remove-tracks-dialog"); };
	REQUIRE(tracks.app.waitUntil([&] { return tracks.list.warnsBeforeRemoving(); }));

	// Each one alone asks first; Escape keeps it.
	for (const uint64_t track : all)
	{
		INFO(tracks.track(track).at("name").get<std::string>());
		tracks.click(row(track) + " .track-name");
		REQUIRE(tracks.list.selection() == std::vector<uint64_t>{track});
		tracks.app.keys().run("track.removeSelected", Json::object(), tracks.app.main(), nullptr);
		tracks.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(warning().size() == 1);
		tracks.app.ui().key(std::nullopt, "Escape");
		REQUIRE(tracks.app.waitUntil([&] { return warning().empty(); }));
		CHECK(tracks.order() == all);
	}
}

TEST_CASE("Remove Selected Tracks announces only a marker track that stays as the active one", "[ui]")
{
	Tracks tracks;
	const uint64_t first = tracks.add({{"type", "marker"}});  // active, goes
	const uint64_t second = tracks.add({{"type", "marker"}}); // would become active, goes too
	const uint64_t kept = tracks.add({{"type", "marker"}});
	tracks.app.call("marker.activateTrack", {{"track", first}});
	tracks.rows(3);
	REQUIRE(tracks.app.waitUntil(
		[&] { return tracks.display.project().value("activeMarkerTrack", uint64_t{0}) == first; }));
	engine::Client listener(tracks.app.engine());

	tracks.click(row(first) + " .track-name");
	tracks.click(row(second) + " .track-name", {"ctrl"});
	tracks.app.keys().run("track.removeSelected", Json::object(), tracks.app.main(), nullptr);
	tracks.app.shell().step(std::chrono::milliseconds(0));
	tracks.rows(1); // empty: no warning
	CHECK(tracks.app.call("project.get").at("activeMarkerTrack") == kept);
	std::vector<uint64_t> announced;
	while (const auto message = listener.poll())
		if (const auto* event = std::get_if<engine::Event>(&*message))
			if (event->name == "marker.trackActivated")
				announced.push_back(event->data.at("track").get<uint64_t>());
	CHECK(announced == std::vector<uint64_t>{kept});
}

TEST_CASE(
	"track heights follow drags of a row's lower edge, with Ctrl every track's, and the wheel scrolls the list", "[ui]")
{
	Tracks tracks;
	std::vector<uint64_t> ids;
	ids.reserve(30);
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
		tracks.rightClick(track);
		tracks.menuOpen();
		tracks.choose(command);
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

	// Remove Empty Tracks leaves the tracks with events, and removes the empty track and with it the folder it leaves
	// empty; Remove Selected Tracks removes the selection, in one undo step.
	menu(full, "track.removeEmpty");
	tracks.rows(2);
	CHECK(tracks.order() == std::vector<uint64_t>{full, order[3]});
	tracks.click(row(full) + " .track-name");
	tracks.click(row(order[3]) + " .track-name", {"ctrl"});
	tracks.app.keys().run("track.removeSelected", Json::object(), tracks.app.main(), nullptr);
	tracks.app.shell().step(std::chrono::milliseconds(0));
	tracks.click("#remove-tracks"); // they aren't empty: the warning asks first
	tracks.rows(0);
	tracks.app.call("history.undo");
	tracks.rows(2);
}

// The engine answers whenever it gets to it, which on a busy machine is many steps later (#702).
TEST_CASE("the track list's context menu follows the selection and stands the rows changing behind it", "[ui]")
{
	Tracks tracks;
	const uint64_t kept = tracks.add({{"type", "audio"}, {"name", "Kept"}});
	const uint64_t gone = tracks.add({{"type", "audio"}, {"name", "Gone"}});
	tracks.rows(2);
	const std::string command = "track.duplicateSelected";
	const auto duplicate = [&tracks, &command]
	{ tracks.app.ui().click(std::nullopt, {menuItem(command), 0, std::nullopt}, ui::MouseButton::Left, 1, {}); };
	const auto answered = [&tracks]
	{
		tracks.app.call("engine.version"); // after what the UI sent
		tracks.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto names = [&tracks]
	{
		std::vector<std::string> all;
		const Json project = tracks.app.call("project.get");
		for (const Json& track : project.at("tracks"))
			all.push_back(track.at("name").get<std::string>());
		return all;
	};

	// The menu opens with the right click, and its items follow the selection at once, without a step of the shell
	// in between: a script's click on an item right after the right click runs it (#1069).
	tracks.rightClick(kept);
	CHECK(elements(tracks.app, "#track-list-menu").at(0).at("visible").get<bool>());
	CHECK(!tracks.itemDisabled(command));
	duplicate();
	tracks.rows(3);
	CHECK(names() == std::vector<std::string>{"Kept", "Kept", "Gone"});

	// The track goes while its menu is open and the list doesn't know yet: the engine refuses, and nothing changes.
	tracks.rightClick(gone);
	tracks.app.call("track.remove", {{"track", gone}});
	REQUIRE(!tracks.itemDisabled(command));
	duplicate();
	tracks.rows(2);
	CHECK(names() == std::vector<std::string>{"Kept", "Kept"});
	CHECK(tracks.list.selection().empty());

	// The rows are built again behind the open menu: the selection stays, and the command works on it.
	tracks.rightClick(kept);
	const uint64_t added = tracks.add({{"type", "midi"}, {"name", "Added"}});
	tracks.rows(3);
	tracks.menuOpen();
	tracks.choose(command);
	tracks.rows(4);
	CHECK(names() == std::vector<std::string>{"Kept", "Kept", "Kept", "Added"});

	// So too when they are built again without the selected track: the item goes grey, and its click does nothing.
	tracks.rightClick(added);
	tracks.app.call("track.remove", {{"track", added}});
	tracks.rows(3);
	tracks.menuOpen();
	CHECK(tracks.list.selection().empty());
	REQUIRE(tracks.app.waitUntil([&] { return tracks.itemDisabled(command); }));
	duplicate();
	answered();
	CHECK(names() == std::vector<std::string>{"Kept", "Kept", "Kept"});
}

// The menu bar's items for selected tracks, as the DOM that MCP's UI tools read has them (#1120).
TEST_CASE("the main menu's items follow what a key or a click changed without a step of the shell", "[ui]")
{
	Tracks tracks;
	const uint64_t a = tracks.add({{"type", "audio"}, {"name", "A"}});
	const uint64_t b = tracks.add({{"type", "audio"}, {"name", "B"}});
	tracks.rows(2);
	tracks.app.shell().step(std::chrono::milliseconds(0));
	const std::string item = "#project-menu menuitem[command=\"track.duplicateSelected\"]";
	const auto disabled = [&tracks, &item]
	{ return elements(tracks.app, item).at(0).at("attributes").contains("disabled"); };
	REQUIRE(tracks.list.selection().empty());
	REQUIRE(disabled());

	// A key that selects a track on its press: the items follow at once, before the key is released (#1173), and stay
	// so after its release.
	tracks.app.ui().key(std::nullopt, "Down", ui::KeyAction::Down);
	REQUIRE(tracks.list.selection().size() == 1);
	CHECK(!disabled());
	tracks.app.ui().key(std::nullopt, "Down", ui::KeyAction::Up);
	CHECK(!disabled());

	// A click selects its row in the click's default action, after the mouse button is released and after every
	// listener of the click ran: the items follow it at once.
	tracks.app.ui().click(std::nullopt, {row(b) + " .track-name", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(tracks.list.selection() == std::vector<uint64_t>{b});
	CHECK(!disabled());
	tracks.app.ui().click(std::nullopt, {row(b) + " .track-name", 0, std::nullopt}, ui::MouseButton::Left, 1, {"ctrl"});
	REQUIRE(tracks.list.selection().empty());
	CHECK(disabled());
	tracks.app.ui().click(std::nullopt, {row(a) + " .track-name", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(tracks.list.selection() == std::vector<uint64_t>{a});
	CHECK(!disabled());
}

// What a click's own listeners change (#1120): the toolbar's tool buttons drop the selection range through the event
// display's listener on the document, and send no change event.
TEST_CASE("the main menu's range items follow a tool button's click without a step of the shell", "[ui]")
{
	Tracks tracks;
	const uint64_t a = tracks.add({{"type", "audio"}, {"name", "A"}});
	tracks.rows(1);
	const std::string item = "#edit-menu menuitem[command=\"edit.cutTime\"]";
	const auto disabled = [&tracks, &item]
	{ return elements(tracks.app, item).at(0).at("attributes").contains("disabled"); };
	tracks.display.setTool(ui::Tool::RangeSelection);
	tracks.display.setRange(ui::RangeSelection{0, 48000, {a}});
	tracks.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(tracks.display.range().has_value());
	REQUIRE(!disabled());

	// The tool button takes the range away; the item is disabled right after the click, not at the next step.
	tracks.app.ui().click(
		std::nullopt, {"button[tool=objectSelection]", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(!tracks.display.range().has_value());
	CHECK(disabled());
}

TEST_CASE("track heights and folded folders are the project's: saved with it, not taken into another one", "[ui]")
{
	Tracks tracks;
	const auto addTracks = [&tracks]
	{
		const uint64_t folder = tracks.add({{"type", "folder"}});
		return std::vector<uint64_t>{
			folder, tracks.add({{"type", "audio"}, {"parent", folder}}), tracks.add({{"type", "audio"}})};
	};
	const std::vector<uint64_t> ids = addTracks();
	const uint64_t folder = ids[0];
	const uint64_t audio = ids[2];
	tracks.rows(3);

	// A drag of the audio track's lower edge, the folder's toggle and a click on the audio track's name.
	const Json box = tracks.box(row(audio));
	const int bottom = box.at("y").get<int>() + box.at("height").get<int>() - 2;
	const int x = box.at("x").get<int>() + 40;
	tracks.app.ui().drag(std::nullopt, {"", 0, ui::Point{x, bottom}}, {"", 0, ui::Point{x, bottom + 20}});
	tracks.click(row(folder) + " .tree-toggle");
	tracks.click(row(audio) + " .track-name");
	const Json saved = {{"heights", {{{"track", audio}, {"height", ui::g_trackHeight + 20.0}}}}, {"folded", {folder}}};
	REQUIRE(tracks.app.waitUntil([&] { return tracks.app.call("trackLayout.get") == saved; }));
	const test::TempDir dir;
	const std::string file = (dir.path() / "layout.dawproject").u8string();
	tracks.app.call("project.save", {{"path", file}});

	// A new project's tracks with the same ids start with the default height, unfolded and not selected.
	const auto fresh = [&tracks, folder, audio]
	{
		return tracks.display.trackHeight(audio) == Approx(ui::g_trackHeight) && !tracks.list.folded(folder) &&
			tracks.list.selection().empty() && tracks.display.laneTracks().size() == 3;
	};
	tracks.app.call("project.new");
	tracks.rows(0);
	REQUIRE(addTracks() == ids);
	tracks.rows(3);
	REQUIRE(tracks.app.waitUntil(fresh));

	// Loading the project brings its heights and folded folders back, but not a selection, and not the heights the
	// project before had for tracks the file has none for.
	tracks.app.call("trackLayout.set", {{"heights", {{{"track", folder}, {"height", 100.0}}}}});
	REQUIRE(tracks.app.waitUntil([&] { return tracks.display.trackHeight(folder) == Approx(100.0); }));
	tracks.click(row(audio) + " .track-name");
	REQUIRE(tracks.list.selection() == std::vector<uint64_t>{audio});
	tracks.app.call("project.load", {{"path", file}});
	REQUIRE(tracks.app.waitUntil(
		[&]
		{
			return tracks.display.trackHeight(audio) == Approx(ui::g_trackHeight + 20.0) &&
				tracks.display.trackHeight(folder) == Approx(ui::g_trackHeight) && tracks.list.folded(folder) &&
				tracks.list.selection().empty() && tracks.display.laneTracks() == std::vector<uint64_t>{folder, audio};
		}));
	tracks.app.shell().step(std::chrono::milliseconds(0));
	CHECK(tracks.box(row(audio)).at("height").get<int>() == static_cast<int>(ui::g_trackHeight) + 20);
}

// #991: trackLayout.set advances the project's revision, so the list sends only changes, and only those of the
// tracks it changed.
TEST_CASE("the track list sends the engine only the track heights and folded folders that change", "[ui]")
{
	Tracks tracks;
	std::mutex mutex;
	int changes = 0;
	const engine::Listener listener(tracks.app.engine(),
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "trackLayout.changed")
				++changes;
		});
	const uint64_t folder = tracks.add({{"type", "folder"}});
	tracks.add({{"type", "audio"}, {"parent", folder}});
	const uint64_t second = tracks.add({{"type", "folder"}});
	tracks.add({{"type", "audio"}, {"parent", second}});
	const uint64_t audio = tracks.add({{"type", "audio"}});
	const uint64_t other = tracks.add({{"type", "audio"}});
	tracks.rows(6);
	const test::TempDir dir;
	tracks.app.call("project.save", {{"path", (dir.path() / "layout.dawproject").u8string()}});
	const auto revision = tracks.app.call("project.revision").at("revision").get<uint64_t>();

	// A press on the audio track's lower edge without a drag, and Unfold Tracks with nothing folded, send nothing.
	const auto edge = [&tracks, audio]
	{
		const Json box = tracks.box(row(audio));
		return ui::Point{box.at("x").get<int>() + 40, box.at("y").get<int>() + box.at("height").get<int>() - 2};
	};
	const ui::Point at = edge();
	tracks.app.ui().press(std::nullopt, {"", 0, at}, ui::MouseButton::Left);
	tracks.app.ui().release(std::nullopt, {"", 0, at}, ui::MouseButton::Left);
	tracks.app.keys().run("track.unfoldAll", Json::object(), tracks.app.main(), nullptr);

	// Toggle Folding with both folders selected folds them with one set, which comes after whatever they sent.
	tracks.click(row(folder) + " .track-name");
	tracks.click(row(second) + " .track-name", {"ctrl"});
	REQUIRE(tracks.list.selection() == std::vector<uint64_t>{folder, second});
	tracks.app.keys().run("track.toggleFolding", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(
		tracks.app.waitUntil([&] { return tracks.app.call("trackLayout.get").at("folded") == Json{folder, second}; }));
	CHECK(tracks.app.call("project.revision").at("revision").get<uint64_t>() == revision + 1);
	{
		const std::scoped_lock lock(mutex);
		CHECK(changes == 1);
	}
	REQUIRE(tracks.app.waitUntil([&] { return tracks.display.laneTracks().size() == 4; }));
	tracks.app.shell().step(std::chrono::milliseconds(0));
	const auto press = [&tracks](ui::Point at)
	{ tracks.app.ui().press(std::nullopt, {"", 0, at}, ui::MouseButton::Left); };
	const auto move = [&tracks](ui::Point to) { tracks.app.ui().move(std::nullopt, {"", 0, to}); };
	const auto release = [&tracks](ui::Point at)
	{ tracks.app.ui().release(std::nullopt, {"", 0, at}, ui::MouseButton::Left); };

	// A press that moves only sideways, here on a track whose in-place editor makes it higher than its own height,
	// and Fold Tracks with both folded send nothing either, and a drag from the same point (which shows that it is on
	// the edge) sends the audio track's height, not the other track's, which only the list has (and another client
	// may have removed meanwhile).
	tracks.display.setInPlaceTracks({audio});
	tracks.app.shell().step(std::chrono::milliseconds(0));
	const ui::Point tall = edge();
	press(tall);
	move({tall.x + 30, tall.y});
	release({tall.x + 30, tall.y});
	CHECK(tracks.display.ownHeight(audio) == Approx(ui::g_trackHeight));
	tracks.display.setInPlaceTracks({});
	tracks.app.shell().step(std::chrono::milliseconds(0));
	const ui::Point from = edge();
	press(from);
	move({from.x + 30, from.y});
	release({from.x + 30, from.y});
	tracks.app.keys().run("track.foldAll", Json::object(), tracks.app.main(), nullptr);
	tracks.display.setTrackHeight(other, 100.0);
	tracks.app.ui().drag(std::nullopt, {"", 0, from}, {"", 0, ui::Point{from.x, from.y + 20}});
	const auto heights = [&tracks]() { return tracks.app.call("trackLayout.get").at("heights"); };
	const auto height = [](uint64_t track, double dp) { return Json{{"track", track}, {"height", dp}}; };
	REQUIRE(tracks.app.waitUntil([&] { return heights() == Json::array({height(audio, ui::g_trackHeight + 20.0)}); }));
	CHECK(tracks.app.call("project.revision").at("revision").get<uint64_t>() == revision + 2);
	{
		const std::scoped_lock lock(mutex);
		CHECK(changes == 2);
	}

	// Heights another client sets during a drag stay: the drag keeps its track's height when they come and sends
	// only that.
	tracks.app.shell().step(std::chrono::milliseconds(0));
	ui::Point lower = edge();
	press(lower);
	move({lower.x, lower.y + 20});
	tracks.app.call("trackLayout.set", {{"heights", {height(other, 120.0)}}});
	REQUIRE(tracks.app.waitUntil([&] { return tracks.display.ownHeight(other) == Approx(120.0); }));
	CHECK(tracks.display.ownHeight(audio) == Approx(ui::g_trackHeight + 40.0));
	tracks.app.call("trackLayout.set", {{"heights", {height(other, 150.0)}}});
	release({lower.x, lower.y + 20});
	REQUIRE(tracks.app.waitUntil(
		[&] { return heights() == Json{height(audio, ui::g_trackHeight + 40.0), height(other, 150.0)}; }));

	// With Ctrl it sends the heights of the tracks it changed: those shown.
	tracks.app.shell().step(std::chrono::milliseconds(0));
	lower = edge();
	tracks.app.ui().drag(std::nullopt, {"", 0, lower}, {"", 0, ui::Point{lower.x, lower.y + 10}}, {"ctrl"});
	const double all = ui::g_trackHeight + 50.0;
	REQUIRE(tracks.app.waitUntil(
		[&]
		{
			return heights() == Json{height(folder, all), height(second, all), height(audio, all), height(other, all)};
		}));

	// A drag that loading a project interrupts sends nothing to the loaded project, whose tracks have the same ids.
	tracks.app.shell().step(std::chrono::milliseconds(0));
	lower = edge();
	press(lower);
	move({lower.x, lower.y + 20});
	tracks.app.call("project.load", {{"path", (dir.path() / "layout.dawproject").u8string()}});
	REQUIRE(tracks.app.waitUntil(
		[&] { return !tracks.list.folded(folder) && tracks.display.ownHeight(audio) == Approx(ui::g_trackHeight); }));
	release({lower.x, lower.y + 20});
	tracks.app.keys().run("track.foldAll", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(
		tracks.app.waitUntil([&] { return tracks.app.call("trackLayout.get").at("folded") == Json{folder, second}; }));
	CHECK(heights() == Json::array());
}

TEST_CASE("Remove Empty Tracks keeps an instrument track without parts that a MIDI track plays", "[ui]")
{
	Tracks tracks;
	const uint64_t instrument = tracks.add({{"type", "instrument"}});
	const Json synth = tracks.track(instrument).at("channel");
	const uint64_t midi = tracks.add({{"type", "midi"}, {"output", synth}});
	tracks.app.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 1000}});
	tracks.add({{"type", "audio"}}); // empty, goes
	tracks.rows(3);

	tracks.app.keys().run("track.removeEmpty", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(tracks.app.waitUntil([&] { return tracks.order() == std::vector<uint64_t>{instrument, midi}; }));
	CHECK(tracks.channel(midi).at("output") == synth);
}

TEST_CASE("Remove Empty Tracks also removes what only the empty tracks played into or filled, in one undo step", "[ui]")
{
	Tracks tracks;
	const uint64_t instrument = tracks.add({{"type", "instrument"}});
	const Json synth = tracks.track(instrument).at("channel");
	const uint64_t midi = tracks.add({{"type", "midi"}, {"output", synth}});
	const uint64_t folder = tracks.add({{"type", "folder"}, {"groupChannel", true}});
	const Json group = tracks.track(folder).at("channel");
	const uint64_t audio = tracks.add({{"type", "audio"}, {"parent", folder}});
	REQUIRE(tracks.channel(audio).at("output") == group);
	tracks.rows(4);

	tracks.app.keys().run("track.removeEmpty", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(0);
	CHECK(tracks.order().empty());

	tracks.app.call("history.undo");
	tracks.rows(4);
	CHECK(tracks.order() == std::vector<uint64_t>{instrument, midi, folder, audio});
	CHECK(tracks.channel(midi).at("output") == synth);
	CHECK(tracks.channel(audio).at("output") == group);
}

TEST_CASE(
	"Remove Empty Tracks keeps a folder that a track outside it plays into and removes nested empty folders", "[ui]")
{
	Tracks tracks;
	const uint64_t folder = tracks.add({{"type", "folder"}, {"groupChannel", true}});
	tracks.add({{"type", "audio"}, {"parent", folder}}); // empty, goes
	const uint64_t full = tracks.add({{"type", "instrument"}, {"output", tracks.track(folder).at("channel")}});
	tracks.app.call("midiPart.add", {{"track", full}, {"position", 0}, {"length", 1000}});
	const uint64_t outer = tracks.add({{"type", "folder"}});
	const uint64_t inner = tracks.add({{"type", "folder"}, {"parent", outer}});
	tracks.add({{"type", "midi"}, {"parent", inner}});
	// A group track stays, even when only an empty track plays through it.
	const uint64_t group = tracks.add({{"type", "group"}});
	tracks.add({{"type", "audio"}, {"output", tracks.track(group).at("channel")}}); // empty, goes
	tracks.rows(8);

	tracks.app.keys().run("track.removeEmpty", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(tracks.app.waitUntil([&] { return tracks.order() == std::vector<uint64_t>{folder, full, group}; }));
	tracks.app.call("history.undo");
	tracks.rows(8);
	CHECK(tracks.track(inner).at("parent") == outer);
}

TEST_CASE("Remove Empty Tracks keeps a folder and a folder nested in it while a track inside holds a part", "[ui]")
{
	Tracks tracks;
	const uint64_t outer = tracks.add({{"type", "folder"}});
	const uint64_t outerFull = tracks.add({{"type", "instrument"}, {"parent", outer}});
	tracks.app.call("midiPart.add", {{"track", outerFull}, {"position", 0}, {"length", 1000}});
	tracks.add({{"type", "audio"}, {"parent", outer}}); // empty, goes
	const uint64_t inner = tracks.add({{"type", "folder"}, {"parent", outer}});
	const uint64_t innerFull = tracks.add({{"type", "audio"}, {"parent", inner}});
	tracks.app.call("audioPart.add", {{"track", innerFull}, {"position", 0}, {"length", 1000}});
	tracks.add({{"type", "midi"}, {"parent", inner}}); // empty, goes
	tracks.rows(6);

	tracks.app.keys().run("track.removeEmpty", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(4);
	CHECK(tracks.order() == std::vector<uint64_t>{outer, outerFull, inner, innerFull});
	CHECK(tracks.track(inner).at("parent") == outer);
	CHECK(tracks.track(innerFull).at("parent") == inner);
	tracks.app.call("history.undo");
	tracks.rows(6);
}

TEST_CASE(
	"the tempo and signature tracks stay through Remove Empty Tracks, and out of new folders and duplicates", "[ui]")
{
	Tracks tracks;
	const uint64_t tempo = tracks.add({{"type", "tempo"}});
	const uint64_t signature = tracks.add({{"type", "signature"}});
	const uint64_t audio = tracks.add({{"type", "audio"}});
	tracks.rows(3);

	// Remove Empty Tracks removes the empty audio track, not the tempo and signature tracks.
	tracks.app.keys().run("track.removeEmpty", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(tracks.app.waitUntil([&] { return tracks.order() == std::vector<uint64_t>{tempo, signature}; }));

	// Move Selected Tracks to New Folder moves the other tracks and leaves them at the top level.
	tracks.app.call("history.undo");
	tracks.rows(3);
	tracks.click(row(tempo) + " .track-name");
	tracks.click(row(signature) + " .track-name", {"ctrl"});
	tracks.click(row(audio) + " .track-name", {"ctrl"});
	tracks.app.keys().run("track.moveSelectedToNewFolder", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(4);
	const std::vector<uint64_t> order = tracks.order();
	REQUIRE(order.size() == 4);
	CHECK(order[0] == tempo);
	CHECK(order[1] == signature);
	CHECK(tracks.track(tempo).at("parent") == 0);
	CHECK(tracks.track(signature).at("parent") == 0);
	CHECK(tracks.track(order[2]).at("type") == "folder");
	CHECK(tracks.track(audio).at("parent") == order[2]);

	// Duplicate Tracks duplicates the other tracks only.
	tracks.click(row(tempo) + " .track-name");
	tracks.click(row(signature) + " .track-name", {"ctrl"});
	tracks.click(row(audio) + " .track-name", {"ctrl"});
	tracks.app.keys().run("track.duplicateSelected", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(5);
	const std::vector<uint64_t> after = tracks.order();
	for (const char* type : {"tempo", "signature"})
		CHECK(std::count_if(after.begin(), after.end(),
				  [&tracks, type](uint64_t track) { return tracks.track(track).at("type") == type; }) == 1);
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
	CHECK(elements(tracks.app, "#visible-controls > listitem").size() == 5);
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return !open(); }));

	REQUIRE(tracks.app.waitUntil(
		[&] { return controls(audio) == std::vector<std::string>{"mute", "solo", "record", "timeBase", "read"}; }));
	CHECK(controls(midi) == std::vector<std::string>{"mute", "solo", "record", "monitor", "read"});
	CHECK(tracks.app.call("settings.get", {{"key", "ui.trackControls"}}).at("value").at("audio") ==
		Json{"mute", "solo", "record", "timeBase", "read"});
	// Switch Time Base switches the track's time base.
	tracks.click(row(audio) + " button.timeBase");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(audio).at("timeBase") == "linear"; }));
}

TEST_CASE("group and effect tracks can show Switch Time Base in the track list", "[ui]")
{
	Tracks tracks;
	const uint64_t group = tracks.add({{"type", "group"}});
	const uint64_t effect = tracks.add({{"type", "effect"}});
	tracks.rows(2);

	// The track controls settings dialog offers it for both types (reference manual pp. 179-180).
	const auto open = [&tracks] { return !elements(tracks.app, "#track-controls-dialog").empty(); };
	tracks.click("#track-controls-settings");
	REQUIRE(tracks.app.waitUntil(open));
	for (const char* type : {"group", "effect"})
	{
		INFO(type);
		tracks.click(std::string("#controls-types > [value=") + type + "]");
		tracks.click("#hidden-controls > [value=timeBase]");
		tracks.click("#controls-add");
		CHECK(elements(tracks.app, "#visible-controls > [value=timeBase]").size() == 1);
	}
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return !open(); }));

	for (const uint64_t track : {group, effect})
	{
		INFO(tracks.track(track).at("type"));
		REQUIRE(
			tracks.app.waitUntil([&] { return elements(tracks.app, row(track) + " button.timeBase").size() == 1; }));
		tracks.click(row(track) + " button.timeBase");
		REQUIRE(tracks.app.waitUntil([&] { return tracks.track(track).at("timeBase") == "linear"; }));
		REQUIRE(tracks.app.waitUntil(
			[&] { return !elements(tracks.app, row(track) + " button.timeBase.checked").empty(); }));
	}
}

TEST_CASE("the track controls settings dialog keeps the tempo track's controls and chooses them", "[ui]")
{
	Tracks tracks;
	const uint64_t tempo = tracks.add({{"type", "tempo"}});
	tracks.rows(1);
	const auto controls = [&tracks](uint64_t track)
	{
		std::vector<std::string> names;
		for (const Json& control : elements(tracks.app, row(track) + " [control]"))
			names.push_back(control.at("attributes").at("control").get<std::string>());
		return names;
	};
	const auto setting = [&tracks]
	{ return tracks.app.call("settings.get", {{"key", "ui.trackControls"}}).at("value").value("tempo", Json()); };
	const std::vector<std::string> all{"tempoTrack", "currentTempo", "tempoType"};
	REQUIRE(tracks.app.waitUntil([&] { return controls(tempo) == all; }));
	// By default the three fit on one line of the tempo track's controls area (reference manual p. 162).
	const Json button = tracks.box(row(tempo) + " [control=tempoTrack]");
	const Json type = tracks.box(row(tempo) + " select.tempo-type");
	CHECK(type.at("y").get<int>() < button.at("y").get<int>() + button.at("height").get<int>());

	const auto open = [&tracks] { return !elements(tracks.app, "#track-controls-dialog").empty(); };
	// OK without touching the tempo track keeps its controls.
	tracks.click("#track-controls-settings");
	REQUIRE(tracks.app.waitUntil(open));
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return !open(); }));
	CHECK(setting() == Json(all));
	tracks.app.shell().step(std::chrono::milliseconds(0));
	CHECK(controls(tempo) == all);

	// Its type lists its own controls, without Mute or Solo; hiding Type of New Tempo Points takes it off the row.
	tracks.click("#track-controls-settings");
	REQUIRE(tracks.app.waitUntil(open));
	tracks.click("#controls-types > [value=tempo]");
	std::vector<std::string> visible;
	for (const Json& item : elements(tracks.app, "#visible-controls > listitem"))
		visible.push_back(item.at("attributes").at("value").get<std::string>());
	CHECK(visible == all);
	tracks.click("#visible-controls > [value=tempoType]");
	tracks.click("#controls-remove");
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return !open(); }));
	CHECK(setting() == Json{"tempoTrack", "currentTempo"});
	REQUIRE(tracks.app.waitUntil([&] { return elements(tracks.app, row(tempo) + " select.tempo-type").empty(); }));
	CHECK(controls(tempo) == std::vector<std::string>{"tempoTrack", "currentTempo"});
}

TEST_CASE("the divider beside the track list sizes it, and the width comes back after a restart", "[ui]")
{
	test::UiApp app({1000, 500});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	const uint64_t audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return elements(app, row(audio)).size() == 1; }));
	app.shell().step(std::chrono::milliseconds(0));
	const auto box = [&app](const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].at("box");
	};
	const auto x = [&box](const std::string& selector) { return box(selector).at("x").get<int>(); };
	const auto width = [&box](const std::string& selector) { return box(selector).at("width").get<int>(); };
	const int lanes = x("event-lanes.events") - x("#event-display");
	// The track list, its rows, the divider and the event display with its lanes, which takes the rest of the main
	// zone.
	const auto follows = [&](int expected)
	{
		CHECK(width("#track-list") == expected);
		CHECK(width(row(audio)) == expected);
		CHECK(x("splitter[for=trackList]") == x("#track-list") + expected);
		CHECK(x("#event-display") == x("splitter[for=trackList]") + width("splitter[for=trackList]"));
		CHECK(x("#event-display") + width("#event-display") == x("#main-zone") + width("#main-zone"));
		CHECK(x("event-lanes.events") - x("#event-display") == lanes);
	};
	const auto project = [&app]
	{
		const Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
		return layout.value("windows", Json::object()).value("project", Json::object());
	};
	const auto saved = [&project] { return project().value("sizes", Json::object()).value("trackList", 0.0); };
	const auto layout = [&app](const Json& sizes)
	{ app.call("settings.set", {{"key", "ui.layout"}, {"value", {{"windows", {{"project", {{"sizes", sizes}}}}}}}}); };
	const auto drag = [&](int dx)
	{
		const Json splitter = box("splitter[for=trackList]");
		const ui::Point from{splitter.at("x").get<int>() + (splitter.at("width").get<int>() / 2),
			splitter.at("y").get<int>() + (splitter.at("height").get<int>() / 2)};
		app.ui().drag(std::nullopt, {"", 0, from}, {"", 0, ui::Point{from.x + dx, from.y}});
		app.shell().step(std::chrono::milliseconds(0));
	};
	follows(280);

	// Dragging the divider sizes the track list; the event display follows, and ui.layout keeps the width.
	drag(60);
	follows(340);
	REQUIRE(app.waitUntil([&] { return saved() == 340.0; }));
	drag(-25);
	follows(315);
	REQUIRE(app.waitUntil([&] { return saved() == 315.0; }));

	// Not below the track list's minimum, nor so far that the event display goes below its own.
	const int most = width("#main-zone") - width("splitter[for=trackList]") - 72;
	drag(-1000);
	follows(120);
	drag(2000);
	follows(most);

	// A double click doesn't hide the track list, as it hides a zone.
	const Json splitter = box("splitter[for=trackList]");
	app.ui().click(std::nullopt,
		{"", 0,
			ui::Point{splitter.at("x").get<int>() + (splitter.at("width").get<int>() / 2),
				splitter.at("y").get<int>() + (splitter.at("height").get<int>() / 2)}},
		ui::MouseButton::Left, 2, {});
	app.shell().step(std::chrono::milliseconds(0));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#track-list").at(0).at("visible").get<bool>());
	follows(most);
	CHECK(!project().value("strips", Json::object()).contains("trackList"));

	// ui.layout from another client sizes it too, as Docking::setSize does; a size too large for the window is held
	// where the event display keeps its minimum.
	layout({{"trackList", 300}});
	REQUIRE(app.waitUntil([&] { return width("#track-list") == 300; }));
	app.shell().step(std::chrono::milliseconds(0));
	follows(300);
	app.appUi().docking().setSize("project", "trackList", 333);
	app.shell().step(std::chrono::milliseconds(0));
	follows(333);
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("sizes").at("trackList") == 333.0);
	layout({{"trackList", 1e300}});
	REQUIRE(app.waitUntil([&] { return width("#track-list") == most; }));
	app.shell().step(std::chrono::milliseconds(0));
	follows(most);

	// A window too narrow for both takes the room from the track list once the event display is at its minimum.
	layout({{"trackList", 600}});
	REQUIRE(app.waitUntil([&] { return width("#track-list") == 600; }));
	const ui::Rect bounds = app.shell().bounds(app.main().id());
	app.shell().setBounds(app.main().id(), {bounds.x, bounds.y, 500, bounds.height});
	REQUIRE(app.waitUntil([&] { return width("#main-zone") == 500; }));
	app.shell().step(std::chrono::milliseconds(0));
	follows(500 - width("splitter[for=trackList]") - 72);
	app.shell().setBounds(app.main().id(), bounds);
	REQUIRE(app.waitUntil([&] { return width("#main-zone") == 1000; }));
	app.shell().step(std::chrono::milliseconds(0));
	follows(600);

	// Squeezed by a side zone, the main zone keeps the column's 200 dp, which the track list at its minimum, the
	// divider and the event display at its minimum fill.
	app.appUi().docking().setVisible("project", "left", true);
	app.appUi().docking().setSize("project", "left", 900);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(width("#main-zone") == 200);
	follows(120);

	// It is in dp, so a larger UI scale makes it wider in pixels.
	app.appUi().docking().setVisible("project", "left", false);
	layout({{"trackList", 300}});
	REQUIRE(app.waitUntil([&] { return width("#track-list") == 300; }));
	app.setDpRatio(2.0F);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(width("#track-list") == 600);
	app.setDpRatio(1.0F);
	app.shell().step(std::chrono::milliseconds(0));

	// The width dragged last comes back after a restart, with a new project.
	drag(20);
	follows(320);
	REQUIRE(app.waitUntil([&] { return saved() == 320.0; }));
	app.restart();
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	const uint64_t again = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return elements(app, row(again)).size() == 1; }));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(width("#track-list") == 320);
	CHECK(width(row(again)) == 320);
	CHECK(x("#event-display") == x("splitter[for=trackList]") + width("splitter[for=trackList]"));
}

TEST_CASE("the track name width and each track type's controls area width come from the settings dialog", "[ui]")
{
	Tracks tracks;
	const uint64_t audio = tracks.add({{"type", "audio"}});
	const uint64_t midi = tracks.add({{"type", "midi"}});
	// An audio track two folders deep.
	const uint64_t outer = tracks.add({{"type", "folder"}});
	const uint64_t inner = tracks.add({{"type", "folder"}, {"parent", outer}});
	const uint64_t deep = tracks.add({{"type", "audio"}, {"parent", inner}});
	tracks.rows(5);
	const auto width = [&tracks](const std::string& selector) { return tracks.box(selector).at("width").get<int>(); };
	const auto right = [&tracks](const std::string& selector)
	{
		const Json box = tracks.box(selector);
		return box.at("x").get<int>() + box.at("width").get<int>();
	};
	// The lines a row's controls take, by their buttons' tops.
	const auto lines = [&tracks](uint64_t track)
	{
		std::set<int> tops;
		for (const Json& button : elements(tracks.app, row(track) + " button[control]"))
			tops.insert(button.at("box").at("y").get<int>());
		return tops.size();
	};
	const auto trackList = [&tracks](int dp)
	{
		tracks.app.call("settings.set",
			{{"key", "ui.layout"}, {"value", {{"windows", {{"project", {{"sizes", {{"trackList", dp}}}}}}}}}});
		REQUIRE(tracks.app.waitUntil([&] { return tracks.box("#track-list").at("width") == dp; }));
		tracks.app.shell().step(std::chrono::milliseconds(0));
	};
	// By default names and controls areas are 120 dp wide, with audio's five controls on one line. Two folders deep,
	// the name gives way, so that the controls still show whole.
	CHECK(width(row(audio) + " .track-name") == 120);
	CHECK(width(row(midi) + " .track-name") == 120);
	CHECK(width(row(audio) + " .track-controls") == 120);
	CHECK(lines(audio) == 1);
	CHECK(width(row(deep) + " .track-name") < 120);
	CHECK(width(row(deep) + " .track-controls") == 120);
	CHECK(right(row(deep) + " .track-controls") <= right(row(deep)));
	CHECK(lines(deep) == 1);

	// The dialog keeps what it doesn't know of the setting, e.g. from a later GNUbase.
	tracks.app.call("settings.set",
		{{"key", "ui.trackWidths"},
			{"value", {{"name", 120}, {"controls", {{"audio", 120}, {"chord", 200}}}, {"channel", 60}}}});

	const auto open = [&tracks] { return !elements(tracks.app, "#track-controls-dialog").empty(); };
	const auto type = [&tracks](const std::string& selector, const std::string& text)
	{
		tracks.click(selector, {}, 2);
		tracks.app.ui().key(std::nullopt, "Mod+A");
		tracks.app.ui().text(std::nullopt, text);
		tracks.app.ui().key(std::nullopt, "Enter");
		tracks.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto value = [&tracks](const std::string& selector)
	{ return elements(tracks.app, selector).at(0).at("attributes").value("value", ""); };
	tracks.click("#track-controls-settings");
	REQUIRE(tracks.app.waitUntil(open));
	CHECK(value("#track-name-width") == "120");
	CHECK(value("#controls-area-width") == "120");
	// Track Name Width is global; Controls Area Width is the selected type's, here audio's.
	type("#track-name-width", "130");
	type("#controls-area-width", "48");
	// MIDI's own width, which Reset sets back to its default.
	tracks.click("#controls-types > [value=midi]");
	CHECK(value("#controls-area-width") == "120");
	type("#controls-area-width", "60");
	tracks.click("#controls-reset");
	CHECK(value("#controls-area-width") == "120");
	tracks.click("#controls-types > [value=audio]");
	CHECK(value("#controls-area-width") == "48");
	// Out of range, it is held at the field's limits; Ctrl-click sets the default.
	type("#track-name-width", "5000");
	CHECK(value("#track-name-width") == "400");
	tracks.click("#track-name-width", {"ctrl"});
	CHECK(value("#track-name-width") == "120");
	type("#track-name-width", "130");
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return !open(); }));

	const Json saved = tracks.app.call("settings.get", {{"key", "ui.trackWidths"}}).at("value");
	CHECK(saved.at("name") == 130);
	CHECK(saved.at("controls").at("audio") == 48);
	CHECK(saved.at("controls").at("midi") == 120);
	CHECK(saved.at("controls").at("chord") == 200);
	CHECK(saved.at("channel") == 60);
	REQUIRE(tracks.app.waitUntil([&] { return width(row(audio) + " .track-name") == 130; }));
	CHECK(width(row(midi) + " .track-name") == 130);

	// Each controls area is as wide as its type's width, whatever the track list's: audio's five controls wrap onto
	// three lines of two, MIDI's three fit on one. In a narrow track list the names give way.
	CHECK(width(row(audio) + " .track-controls") == 48);
	CHECK(lines(audio) == 3);
	CHECK(lines(midi) == 1);
	trackList(500);
	CHECK(width(row(audio) + " .track-controls") == 48);
	CHECK(lines(audio) == 3);
	trackList(200);
	CHECK(width(row(audio) + " .track-controls") == 48);
	CHECK(width(row(midi) + " .track-controls") == 120);
	CHECK(width(row(midi) + " .track-name") < 130);
	CHECK(right(row(midi) + " .track-controls") <= right(row(midi)));

	// From another client: a type left out has the default controls area width.
	tracks.app.call("settings.set", {{"key", "ui.trackWidths"}, {"value", {{"name", 90}}}});
	trackList(300);
	REQUIRE(tracks.app.waitUntil([&] { return width(row(audio) + " .track-name") == 90; }));
	CHECK(width(row(audio) + " .track-controls") == 120);
	CHECK(lines(audio) == 1);
}

TEST_CASE("missing track widths use each type's default and keep tempo controls on one line", "[ui]")
{
	Tracks tracks;
	tracks.app.call("settings.set", {{"key", "ui.trackWidths"}, {"value", {{"name", 90}}}});
	const uint64_t tempo = tracks.add({{"type", "tempo"}});
	const uint64_t marker = tracks.add({{"type", "marker"}});
	const uint64_t chord = tracks.add({{"type", "chord"}});
	tracks.rows(3);
	CHECK(tracks.box(row(tempo) + " .track-controls").at("width") == 144);
	CHECK(tracks.box(row(marker) + " .track-controls").at("width") == 140);
	CHECK(tracks.box(row(chord) + " .track-controls").at("width") == 160);
	const Json controls = elements(tracks.app, row(tempo) + " .track-controls > [control]");
	REQUIRE(controls.size() == 3);
	std::set<int> tops;
	for (const Json& control : controls)
		tops.insert(control.at("box").at("y").get<int>());
	CHECK(tops.size() == 1);
	tracks.click("#track-controls-settings");
	REQUIRE(tracks.app.waitUntil([&] { return !elements(tracks.app, "#track-controls-dialog").empty(); }));
	tracks.click("#controls-types > [value=tempo]");
	CHECK(elements(tracks.app, "#controls-area-width").at(0).at("attributes").at("value") == "144");
	tracks.click("#controls-reset");
	CHECK(elements(tracks.app, "#controls-area-width").at(0).at("attributes").at("value") == "144");
	tracks.click("#controls-ok");
	REQUIRE(tracks.app.waitUntil([&] { return elements(tracks.app, "#track-controls-dialog").empty(); }));
	CHECK(tracks.box(row(tempo) + " .track-controls").at("width") == 144);
	// An explicit width still wins over the default, including zero.
	tracks.app.call(
		"settings.set", {{"key", "ui.trackWidths"}, {"value", {{"name", 90}, {"controls", {{"tempo", 0}}}}}});
	REQUIRE(tracks.app.waitUntil([&] { return tracks.box(row(tempo) + " .track-controls").at("width") == 0; }));
}

TEST_CASE("folder tracks: new folders for the selected tracks, group channels and data blocks", "[ui]")
{
	Tracks tracks;
	const uint64_t audio = tracks.add({{"type", "audio"}});
	const uint64_t other = tracks.add({{"type", "audio"}});
	const uint64_t marker = tracks.add({{"type", "marker"}});
	const uint64_t media =
		tracks.app
			.call(
				"media.add", {{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 96000}})
			.at("media")
			.at("id")
			.get<uint64_t>();
	tracks.app.call("audioEvent.add",
		{{"track", other}, {"clip", tracks.app.call("clip.add", {{"media", media}}).at("clip").at("id")},
			{"position", 0}});
	tracks.rows(3);
	const Json main = tracks.app.call("project.get").at("busses")[1].at("id");

	// Move Selected Tracks to New Folder with Group Channel (reference manual p. 223): the folder takes the first
	// one's place, the marker track stays at the top level, and the tracks play through the group channel.
	tracks.click(row(other) + " .track-name");
	tracks.click(row(audio) + " .track-name", {"ctrl"});
	tracks.click(row(marker) + " .track-name", {"ctrl"});
	tracks.app.keys().run("track.moveSelectedToNewFolderWithGroupChannel", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(4);
	const std::vector<uint64_t> order = tracks.order();
	const uint64_t folder = order[0];
	CHECK(order == std::vector<uint64_t>{folder, audio, other, marker});
	CHECK(tracks.track(folder).at("type") == "folder");
	const Json group = tracks.track(folder).at("channel");
	REQUIRE(group != 0);
	CHECK(tracks.channel(audio).at("output") == group);
	CHECK(tracks.channel(other).at("output") == group);
	// A folder with a group channel shows mute and solo.
	REQUIRE(tracks.app.waitUntil([&] { return elements(tracks.app, row(folder) + " button.mute").size() == 1; }));

	// The folder's lane shows the event inside it as a data block (p. 226), by ui.folderData, which the context menu
	// sets with settings.set.
	const auto blocks = [&tracks] { return tracks.display.folderData().at(0).events.size(); };
	REQUIRE(tracks.app.waitUntil([&] { return tracks.display.folderData().size() == 4 && blocks() == 1; }));
	CHECK(tracks.display.folderData().at(2).events.empty());
	CHECK(elements(tracks.app, "#folder-data-submenu [command=\"settings.set\"]").size() == 3);
	tracks.app.keys().run(
		"settings.set", {{"key", "ui.folderData"}, {"value", "hideWhenExpanded"}}, tracks.app.main(), nullptr);
	REQUIRE(tracks.app.waitUntil([&] { return blocks() == 0; }));
	tracks.list.setFolded(folder, true);
	REQUIRE(tracks.app.waitUntil([&] { return tracks.display.laneTracks().size() == 2 && blocks() == 1; }));
	tracks.list.setFolded(folder, false);

	// The context menu turns the selected folders' group channels off and on (p. 224); the one that has nothing to do
	// is disabled.
	const auto menu = [&tracks](uint64_t track, const char* command, const char* greyed)
	{
		tracks.rightClick(track);
		tracks.menuOpen();
		REQUIRE(tracks.app.waitUntil([&] { return tracks.itemDisabled(greyed) && !tracks.itemDisabled(command); }));
		tracks.choose(command);
	};
	menu(folder, "track.disableSelectedGroupChannels", "track.enableSelectedGroupChannels");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(folder).at("channel") == 0; }));
	CHECK(tracks.channel(audio).at("output") == main);
	REQUIRE(tracks.app.waitUntil([&] { return elements(tracks.app, row(folder) + " button.mute").empty(); }));
	menu(folder, "track.enableSelectedGroupChannels", "track.disableSelectedGroupChannels");
	REQUIRE(tracks.app.waitUntil([&] { return tracks.track(folder).at("channel") != 0; }));
	CHECK(tracks.channel(other).at("output") == tracks.track(folder).at("channel"));
	// The list shows the group channel too: its rows are built again for it, before the next click on one.
	REQUIRE(tracks.app.waitUntil([&] { return elements(tracks.app, row(folder) + " button.mute").size() == 1; }));

	// Move Selected Tracks to New Folder puts one into a folder inside it, without a group channel.
	tracks.click(row(other) + " .track-name");
	tracks.app.keys().run("track.moveSelectedToNewFolder", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(5);
	const auto inner = tracks.track(other).at("parent").get<uint64_t>();
	CHECK(tracks.track(inner).at("parent") == folder);
	CHECK(tracks.track(inner).at("channel") == 0);
	CHECK(tracks.channel(other).at("output") == tracks.track(folder).at("channel"));
}

TEST_CASE("Freeze/Unfreeze Selected Tracks freezes and unfreezes them, Render in Place renders them", "[ui]")
{
	Tracks tracks;
	tracks.app.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	const uint64_t synth =
		tracks.add({{"type", "instrument"}, {"instrument", "testSynth"}, {"timeBase", "linear"}, {"name", "Synth"}});
	const Json part = tracks.app.call("midiPart.add", {{"track", synth}, {"position", 0}, {"length", 4800}});
	tracks.app.call("note.add",
		{{"part", part.at("part").at("id")},
			{"notes", {{{"position", 0}, {"length", 100000}, {"pitch", 60}, {"velocity", 0.8}}}}});
	tracks.rows(1);
	tracks.click(row(synth) + " .track-name");

	// The engine answers nothing while it renders, on a busy machine for longer than a call to it waits: the list says
	// when it's done.
	const std::chrono::seconds rendering(60);
	const auto frozen = [&tracks, synth] { return hasClass(elements(tracks.app, row(synth)).at(0), "frozen"); };
	tracks.app.keys().run("track.freezeSelected", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(tracks.app.waitUntil(frozen, rendering));
	CHECK(!tracks.track(synth).at("frozen").is_null());
	// Again, with every selected track frozen, it unfreezes them.
	tracks.app.keys().run("track.freezeSelected", Json::object(), tracks.app.main(), nullptr);
	REQUIRE(tracks.app.waitUntil([&frozen] { return !frozen(); }, rendering));
	CHECK(tracks.track(synth).at("frozen").is_null());

	tracks.app.keys().run("track.renderSelected", Json::object(), tracks.app.main(), nullptr);
	tracks.rows(2, rendering);
	const std::vector<uint64_t> order = tracks.order();
	REQUIRE(order.size() == 2);
	CHECK(tracks.track(order[1]).at("name") == "Synth (Rendered)");
	CHECK(tracks.track(order[1]).at("type") == "audio");
}
