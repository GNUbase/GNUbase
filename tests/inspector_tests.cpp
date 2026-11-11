// SPDX-License-Identifier: GPL-3.0-or-later
// The info line and the inspector (#89, docs/features/inspector.md) in the app's project window, driven with
// ui::Automation as MCP's UI tools drive them: the info line shows the first selected event or part and its edits
// change every selected one, as one undo step; the inspector shows the selected track's basics, routing, inserts and
// notepad, and its controls change the track's channel.
#include "testutils/testutils.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/eventdisplay.h"
#include "ui/markers.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json classes = element(app, selector).value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
	}

	std::string attribute(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json attributes = element(app, selector).value("attributes", Json::object());
		return attributes.contains(name) ? attributes.at(name).get<std::string>() : std::string();
	}

	// The project window at a dp ratio of 1 with the info line and the inspector shown.
	struct Window
	{
		Window() : display(app.appUi().showProject()), list(*app.appUi().trackList())
		{
			app.setDpRatio(1.0F);
			app.appUi().docking().setVisible("project", "infoLine", true);
			app.appUi().docking().open("inspector");
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		uint64_t addTrack(const Json& parameters)
		{
			return call("track.add", parameters).at("track").at("id").get<uint64_t>();
		}

		uint64_t addPart(uint64_t track, int64_t position, int64_t length, const std::string& name)
		{
			return call("midiPart.add", {{"track", track}, {"position", position}, {"length", length}, {"name", name}})
				.at("part")
				.at("id")
				.get<uint64_t>();
		}

		// The object with that id in a list of the project.
		Json find(const char* list, uint64_t id)
		{
			const Json objects = call("project.get").at(list);
			const auto found = std::find_if(
				objects.begin(), objects.end(), [id](const Json& object) { return object.at("id") == id; });
			REQUIRE(found != objects.end());
			return *found;
		}

		Json part(uint64_t id) { return find("midiParts", id); }
		Json channel(uint64_t track) { return find("channels", find("tracks", track).at("channel").get<uint64_t>()); }

		void click(const std::string& selector, ui::MouseButton button = ui::MouseButton::Left, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, button, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Double-clicks a display or value field, types into it and presses Enter, or keys such as Ctrl+Enter.
		void type(const std::string& selector, const std::string& text, const std::string& enter = "Enter")
		{
			click(selector, ui::MouseButton::Left, 2);
			app.ui().key(std::nullopt, "Mod+A");
			app.ui().text(std::nullopt, text);
			app.ui().key(std::nullopt, enter);
			app.shell().step(std::chrono::milliseconds(0));
		}

		bool shows(const std::string& selector, const std::string& value)
		{
			return app.waitUntil([&] { return attribute(app, selector, "value") == value; });
		}

		int64_t historyPosition() { return call("history.get").at("position").get<int64_t>(); }

		test::UiApp app{{1200, 600}};
		ui::EventDisplay& display;
		ui::TrackList& list;
	};
}

TEST_CASE("the info line shows the first selected part, and its edits change every selected one, undoably", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t first = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	const uint64_t second = window.addPart(midi, 12 * g_quarter, 8 * g_quarter, "Verse");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));

	// Nothing selected: the info line says so.
	CHECK(hasClass(window.app, "#info-line", "empty"));
	window.display.select({first});
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "2.1.1.0"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "empty"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "multiple"));
	CHECK(attribute(window.app, "#info-line textdisplay[field=name]", "value") == "Intro");
	CHECK(attribute(window.app, "#info-line textdisplay[field=end]", "value") == "3.1.1.0");
	CHECK(attribute(window.app, "#info-line textdisplay[field=length]", "value") == "1.0.0.0");
	// Parts have no volume.
	CHECK(hasClass(window.app, "#info-line [info-field=volume]", "unavailable"));

	// Two selected: the first one shows, and a new start moves both by as much, as one undo step.
	window.display.select({second, first});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	CHECK(attribute(window.app, "#info-line textdisplay[field=name]", "value") == "Intro");
	window.type("#info-line textdisplay[field=start]", "3.1.1.0");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("position") == 8 * g_quarter; }));
	CHECK(window.part(second).at("position") == 16 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "3.1.1.0"));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("position") == 4 * g_quarter; }));
	CHECK(window.part(second).at("position") == 12 * g_quarter);

	// A length relative to the current ones, and with Ctrl the same length for all.
	REQUIRE(window.shows("#info-line textdisplay[field=length]", "1.0.0.0"));
	window.type("#info-line textdisplay[field=length]", "2.0.0.0");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("length") == 8 * g_quarter; }));
	CHECK(window.part(second).at("length") == 12 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=length]", "2.0.0.0"));
	window.type("#info-line textdisplay[field=length]", "1.0.0.0", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("length") == 4 * g_quarter; }));
	CHECK(window.part(first).at("length") == 4 * g_quarter);
	// The end, relative too.
	REQUIRE(window.shows("#info-line textdisplay[field=end]", "3.1.1.0"));
	window.type("#info-line textdisplay[field=end]", "3.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("length") == 6 * g_quarter; }));
	CHECK(window.part(second).at("length") == 6 * g_quarter);

	// The name, mute and lock go to every selected part.
	window.type("#info-line textdisplay[field=name]", "Chorus");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("name") == "Chorus"; }));
	CHECK(window.part(first).at("name") == "Chorus");
	window.click("#info-line button[field=mute]");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("mute") == true; }));
	CHECK(window.part(first).at("mute") == true);
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line button[field=mute]", "checked"); }));
	window.click("#info-line button[field=lock]");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("lock") == true; }));
	CHECK(window.part(second).at("lock") == true);

	// Changes from another client show.
	window.call("midiPart.set", {{"part", first}, {"name", "Bridge"}});
	CHECK(window.shows("#info-line textdisplay[field=name]", "Bridge"));
	window.display.select({});
	CHECK(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "empty"); }));
}

TEST_CASE("Ctrl+Enter in the info line takes the first part's own value for all, and only that take", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t first = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	const uint64_t second = window.addPart(midi, 12 * g_quarter, 8 * g_quarter, "Verse");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));
	window.display.select({second, first});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	REQUIRE(window.shows("#info-line textdisplay[field=length]", "1.0.0.0"));

	// The first part's length, untouched, with Ctrl+Enter: the 2-bar part becomes 1 bar long too.
	window.click("#info-line textdisplay[field=length]", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("length") == 4 * g_quarter; }));
	CHECK(window.part(first).at("length") == 4 * g_quarter);

	// The next edit, taken by leaving the field, is relative again: both move by as much.
	window.click("#info-line textdisplay[field=start]", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "3.1.1.0");
	window.click("#info-line [info-field=name] .info-label");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("position") == 8 * g_quarter; }));
	CHECK(window.part(second).at("position") == 16 * g_quarter);
}

TEST_CASE("the info line edits an audio event's volume", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const uint64_t event =
		window
			.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", 96000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "take"));
	CHECK(element(window.app, "#info-line textdisplay[field=name]").at("attributes").contains("disabled"));
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=volume]", "unavailable"));
	window.type("#info-line valuefield[field=volume]", "-6");
	REQUIRE(window.app.waitUntil([&] { return window.find("audioEvents", event).at("volume") == -6.0; }));
	window.call("history.undo");
	CHECK(window.app.waitUntil([&] { return window.find("audioEvents", event).at("volume") == 0.0; }));
}

TEST_CASE("an untouched Ctrl+Enter on the volume gives every selected audio event the first one's volume", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto add = [&](int64_t position, double volume)
	{
		return window
			.call("audioEvent.add",
				{{"track", audio}, {"clip", clip.at("id")}, {"position", position}, {"length", 24000},
					{"volume", volume}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = add(96000, -4.5);
	const uint64_t second = add(192000, 0.0);
	const uint64_t third = add(288000, -12.0);
	const auto volume = [&window](uint64_t id) { return window.find("audioEvents", id).at("volume").get<double>(); };
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 3; }));
	// The first one is the one that starts first, whatever the order selected in.
	window.display.select({third, second, first});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	REQUIRE(window.app.waitUntil(
		[&]
		{
			const std::string shown = attribute(window.app, "#info-line valuefield[field=volume]", "value");
			return !shown.empty() && std::stod(shown) == -4.5;
		}));
	const int64_t position = window.historyPosition();

	// Double-clicked, nothing typed, Ctrl+Enter: the field sends no change, and the first one's volume goes to all.
	window.click("#info-line valuefield[field=volume]", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Ctrl+Enter");
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return volume(second) == -4.5 && volume(third) == -4.5; }));
	CHECK(volume(first) == -4.5);
	// As one undo step.
	CHECK(window.historyPosition() == position + 1);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return volume(third) == -12.0; }));
	CHECK(volume(second) == 0.0);
	CHECK(volume(first) == -4.5);
}

TEST_CASE(
	"the info line shows the marker selected on the marker track, and its edits change every selected one", "[ui]")
{
	Window window;
	window.addTrack({{"type", "marker"}});
	const auto add = [&window](const Json& parameters)
	{ return window.call("marker.add", parameters).at("marker").at("id").get<uint64_t>(); };
	const uint64_t intro = add({{"position", g_quarter}, {"name", "Intro"}});
	const uint64_t verse =
		add({{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});
	const auto marker = [&window](uint64_t id) { return window.find("markers", id); };
	REQUIRE(window.app.waitUntil([&] { return elements(window.app, ".marker-label").size() == 2; }));
	const size_t steps = window.call("history.get").at("steps").size();

	// A click on a marker on the marker track shows it: its description, position, end and length.
	window.app.ui().click(std::nullopt, {".marker-label", 1, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Verse"));
	CHECK(hasClass(window.app, "#info-line", "markers"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "empty"));
	CHECK(attribute(window.app, "#info-line textdisplay[field=start]", "value") == "2.1.1.0");
	CHECK(attribute(window.app, "#info-line textdisplay[field=end]", "value") == "3.1.1.0");
	CHECK(attribute(window.app, "#info-line textdisplay[field=length]", "value") == "1.0.0.0");
	for (const char* field : {"volume", "mute", "lock"})
		CHECK(hasClass(window.app, std::string("#info-line [info-field=") + field + "]", "unavailable"));

	// Each field edits it with marker.set; a new position keeps a cycle marker's length.
	window.type("#info-line textdisplay[field=name]", "Chorus");
	REQUIRE(window.app.waitUntil([&] { return marker(verse).at("name") == "Chorus"; }));
	window.type("#info-line textdisplay[field=start]", "3.1.1.0");
	REQUIRE(window.app.waitUntil([&] { return marker(verse).at("position") == 8 * g_quarter; }));
	CHECK(marker(verse).at("end") == 12 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=end]", "4.1.1.0"));
	window.type("#info-line textdisplay[field=end]", "5.1.1.0");
	REQUIRE(window.app.waitUntil([&] { return marker(verse).at("end") == 16 * g_quarter; }));
	REQUIRE(window.shows("#info-line textdisplay[field=length]", "2.0.0.0"));
	window.type("#info-line textdisplay[field=length]", "1.0.0.0");
	REQUIRE(window.app.waitUntil([&] { return marker(verse).at("end") == 12 * g_quarter; }));
	CHECK(window.call("history.get").at("steps").size() == steps + 4);
	// The info line edits from the markers it shows, which follow the engine's a step or more later: the next edit
	// must start from this end (#929).
	REQUIRE(window.shows("#info-line textdisplay[field=end]", "4.1.1.0"));

	// With several selected the first one shows, a position marker without end and length, and an edit changes all
	// of them, relative to the first one's value, in one undo step.
	window.app.ui().click(std::nullopt, {".marker-label", 0, std::nullopt}, ui::MouseButton::Left, 1, {"ctrl"});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	CHECK(hasClass(window.app, "#info-line", "multiple"));
	CHECK(hasClass(window.app, "#info-line [info-field=end]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=length]", "unavailable"));
	window.type("#info-line textdisplay[field=start]", "1.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return marker(intro).at("position") == 2 * g_quarter; }));
	CHECK(marker(verse).at("position") == 9 * g_quarter);
	CHECK(marker(verse).at("end") == 13 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "1.3.1.0"));
	window.type("#info-line textdisplay[field=name]", "Song");
	REQUIRE(window.app.waitUntil([&] { return marker(intro).at("name") == "Song"; }));
	CHECK(marker(verse).at("name") == "Song");
	CHECK(window.call("history.get").at("steps").size() == steps + 6);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return marker(intro).at("name") == "Intro"; }));
	CHECK(marker(verse).at("name") == "Chorus");
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));

	// With Ctrl+Enter every one gets the value typed.
	window.type("#info-line textdisplay[field=start]", "4.1.1.0", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return marker(intro).at("position") == 12 * g_quarter; }));
	CHECK(marker(verse).at("position") == 12 * g_quarter);

	// Selecting an event shows the event again.
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Part");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Part"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "markers"));
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=mute]", "unavailable"));
}

TEST_CASE("the project window selects events or markers, and the info line shows the ones selected", "[ui]")
{
	Window window;
	window.addTrack({{"type", "marker"}});
	const auto add = [&window](const Json& parameters)
	{ return window.call("marker.add", parameters).at("marker").at("id").get<uint64_t>(); };
	const uint64_t intro = add({{"position", g_quarter}, {"name", "Intro"}});
	const uint64_t verse =
		add({{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Part");
	const auto marker = [&window](uint64_t id) { return window.find("markers", id); };
	REQUIRE(window.app.waitUntil([&] { return elements(window.app, ".marker-label").size() == 2; }));
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	const ui::MarkerLines& lines = *window.app.appUi().markerLines();
	const auto clickMarker = [&window](int index, const std::vector<std::string>& modifiers = {})
	{
		window.app.ui().click(
			std::nullopt, {".marker-label", index, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// A marker clicked again after an event was selected shows again: selecting an event deselects the markers.
	clickMarker(1);
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Verse"));
	// The info line's setup pane names the fields as the info line does (p. 430).
	const auto paneLabels = [&window]
	{
		return element(window.app, "#info-line-setup [setup-section=name]").at("text").get<std::string>() + "/" +
			element(window.app, "#info-line-setup [setup-section=start]").at("text").get<std::string>();
	};
	REQUIRE(window.app.waitUntil([&] { return paneLabels() == "Description/Position"; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Part"));
	REQUIRE(window.app.waitUntil([&] { return paneLabels() == "Name/Start"; }));
	REQUIRE(window.app.waitUntil([&] { return lines.selection().empty(); }));
	CHECK(!hasClass(window.app, ".marker-label[marker=" + std::to_string(verse) + "]", "selected"));
	// On the cycle marker beside its label: a click in the same place this soon would be a double click.
	Rml::ElementList labels;
	Rml::Context& context = window.app.main().context();
	for (int i = 0; i < context.GetNumDocuments() && labels.empty(); ++i)
		context.GetDocument(i)->QuerySelectorAll(labels, ".marker-label");
	REQUIRE(labels.size() == 2);
	Rml::Element& label = *labels[1];
	window.app.ui().click(std::nullopt,
		{"", 0,
			ui::Point{static_cast<int>(label.GetAbsoluteLeft() + label.GetOffsetWidth() + 20.0F),
				static_cast<int>(label.GetAbsoluteTop() + (label.GetOffsetHeight() / 2.0F))}},
		ui::MouseButton::Left, 1, {});
	REQUIRE(window.app.waitUntil([&] { return lines.selection() == std::vector<uint64_t>{verse}; }));
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Verse"));
	CHECK(hasClass(window.app, "#info-line", "markers"));
	CHECK(window.display.selection().empty());

	// Ctrl+Enter on the first marker's own value, typed again, gives every selected marker that value, and leaves the
	// part.
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Part"));
	clickMarker(1);
	clickMarker(0, {"ctrl"});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	REQUIRE(hasClass(window.app, "#info-line", "multiple"));
	const size_t steps = window.call("history.get").at("steps").size();
	window.type("#info-line textdisplay[field=start]", "1.2.1.0", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return marker(verse).at("position") == g_quarter; }));
	CHECK(marker(verse).at("end") == 5 * g_quarter);
	CHECK(marker(intro).at("position") == g_quarter);
	CHECK(window.part(part).at("position") == 0);
	CHECK(window.part(part).at("name") == "Part");
	CHECK(window.call("history.get").at("steps").size() == steps + 1);

	// A selection naming a marker the project no longer has shows nothing.
	window.call("marker.remove", {{"markers", {verse, intro}}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("markers").empty(); }));
	// No reload left that would drop the next selection before the info line sees it.
	REQUIRE(window.app.waitUntil([&] { return !lines.loading() && lines.selection().empty(); }));
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, "#info-line", "markers"); }));
	window.app.appUi().markerLines()->setSelection({verse});
	// Only showMarkers' branch for markers the project doesn't have sets both.
	CHECK(window.app.waitUntil(
		[&] { return hasClass(window.app, "#info-line", "markers") && hasClass(window.app, "#info-line", "empty"); }));
}

TEST_CASE("an untouched Ctrl+Enter on a marker's start gives every selected marker the first one's position", "[ui]")
{
	Window window;
	window.addTrack({{"type", "marker"}});
	const auto add = [&window](const Json& parameters)
	{ return window.call("marker.add", parameters).at("marker").at("id").get<uint64_t>(); };
	const uint64_t intro = add({{"position", 2 * g_quarter}, {"name", "Intro"}});
	const uint64_t verse =
		add({{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});
	// Every label in view: CI's screens make the window 1024 pixels wide, where the timeline beside the inspector shows
	// less than 10 quarters.
	const uint64_t outro = add({{"position", 6 * g_quarter}, {"name", "Outro"}});
	const auto marker = [&window](uint64_t id) { return window.find("markers", id); };
	REQUIRE(window.app.waitUntil([&] { return elements(window.app, ".marker-label").size() == 3; }));
	const auto clickMarker = [&window](int index, const std::vector<std::string>& modifiers = {})
	{
		window.app.ui().click(
			std::nullopt, {".marker-label", index, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// All three selected, the last one first: the one that starts first shows.
	clickMarker(2);
	clickMarker(1, {"ctrl"});
	clickMarker(0, {"ctrl"});
	const ui::MarkerLines& lines = *window.app.appUi().markerLines();
	REQUIRE(window.app.waitUntil([&] { return lines.selection().size() == 3; }));
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	REQUIRE(hasClass(window.app, "#info-line", "multiple"));
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "1.3.1.0"));
	const int64_t position = window.historyPosition();

	// Double-clicked, nothing typed, Ctrl+Enter: the display sends no change, and the first one's position goes to
	// all, a cycle marker keeping its length.
	window.click("#info-line textdisplay[field=start]", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Ctrl+Enter");
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.app.waitUntil([&] { return marker(outro).at("position") == 2 * g_quarter; }));
	CHECK(marker(verse).at("position") == 2 * g_quarter);
	CHECK(marker(verse).at("end") == 6 * g_quarter);
	CHECK(marker(intro).at("position") == 2 * g_quarter);
	// As one undo step.
	CHECK(window.historyPosition() == position + 1);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return marker(outro).at("position") == 6 * g_quarter; }));
	CHECK(marker(verse).at("position") == 4 * g_quarter);
	CHECK(marker(verse).at("end") == 8 * g_quarter);
	CHECK(marker(intro).at("position") == 2 * g_quarter);
}

TEST_CASE("the inspector shows the selected track, and its controls change the track's channel", "[ui]")
{
	Window window;
	CHECK(hasClass(window.app, ".inspector", "empty"));
	const uint64_t group = window.addTrack({{"type", "group"}, {"name", "Drums"}});
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t marker = window.addTrack({{"type", "marker"}});
	const uint64_t vca = window.addTrack({{"type", "vca"}, {"name", "Strings"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 4; }));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	CHECK_FALSE(hasClass(window.app, ".inspector", "empty"));
	for (const char* section : {"basics", "routing", "inserts", "notepad"})
		CHECK(elements(window.app, ".inspector [sections=inspector] > [section=" + std::string(section) + "]").size() ==
			1);

	// Basics: the switches, volume and pan.
	for (const char* control : {"mute", "solo", "record", "monitor"})
	{
		INFO(control);
		const std::string key = std::string(control) == "record" ? "recordEnable" : control;
		const std::string button = ".inspector button[control=" + std::string(control) + "]";
		window.click(button);
		REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at(key) == true; }));
		REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, button, "checked"); }));
		window.call("channel.set", {{"channel", window.channel(audio).at("id")}, {key, false}});
		REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, button, "checked"); }));
	}
	// Switch Time Base (p. 229): checked when the track is linear.
	const std::string timeBase = ".inspector button[control=timeBase]";
	CHECK_FALSE(hasClass(window.app, timeBase, "unavailable"));
	window.click(timeBase);
	REQUIRE(window.app.waitUntil([&] { return window.find("tracks", audio).at("timeBase") == "linear"; }));
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, timeBase, "checked"); }));
	window.call("track.set", {{"track", audio}, {"timeBase", "musical"}});
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, timeBase, "checked"); }));
	window.type(".inspector valuefield[control=volume]", "-6");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == -6.0; }));
	CHECK(window.shows(".inspector valuefield[control=volume]", "-6")); // whatever the C locale is
	window.type(".inspector valuefield[control=pan]", "-50");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("pan") == -0.5; }));

	// Routing: the output from another client shows; one chosen in the drop-down list goes to the engine.
	const uint64_t groupChannel = window.channel(group).at("id").get<uint64_t>();
	window.call("channel.set", {{"channel", window.channel(audio).at("id")}, {"output", groupChannel}});
	REQUIRE(window.shows(".inspector select[control=output]", std::to_string(groupChannel)));
	window.click(".inspector select[control=output]");
	window.app.ui().key(std::nullopt, "Up");
	window.click(".inspector select[control=output]");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("output") != groupChannel; }));
	CHECK(attribute(window.app, ".inspector select[control=input]", "value") ==
		std::to_string(window.channel(audio).at("input").get<uint64_t>()));

	// Inserts: an overview of the slots, bypassed ones marked.
	const uint64_t slot =
		window.call("plugin.insert", {{"channel", window.channel(audio).at("id")}, {"plugin", "builtin:eq"}})
			.at("slot")
			.at("id")
			.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return elements(window.app, ".inspector .insert").size() == 1; }));
	window.call("plugin.set", {{"slot", slot}, {"bypass", true}});
	CHECK(window.app.waitUntil([&] { return hasClass(window.app, ".inspector .insert", "bypassed"); }));

	// The notepad: what is typed is the channel's notes once the field loses the focus.
	window.click(".inspector textarea.notepad");
	window.app.ui().text(std::nullopt, "Double the chorus");
	window.app.ui().key(std::nullopt, "Escape");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("notes") == "Double the chorus"; }));

	// The name.
	window.type(".inspector textdisplay.inspector-name", "Lead");
	REQUIRE(window.app.waitUntil([&] { return window.find("tracks", audio).at("name") == "Lead"; }));

	// A section header opens and closes its section.
	window.click(".inspector [section=basics] .section-header");
	CHECK(hasClass(window.app, ".inspector [section=basics]", "collapsed"));
	window.click(".inspector [section=basics] .section-header");
	CHECK_FALSE(hasClass(window.app, ".inspector [section=basics]", "collapsed"));

	// Set up Sections, from the context menu: hiding a section hides it and keeps it in the user's layout.
	// From an empty area, not from a control or the notepad (p. 167).
	for (const char* control : {".inspector valuefield[control=volume]", ".inspector textarea.notepad"})
	{
		window.click(control, ui::MouseButton::Right);
		CHECK_FALSE(hasClass(window.app, "#inspector-setup", "open"));
	}
	window.click(".inspector [row=volume] .inspector-label", ui::MouseButton::Right);
	REQUIRE(hasClass(window.app, "#inspector-setup", "open"));
	window.click("#inspector-setup [setup-section=notepad]");
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, ".inspector [section=notepad]", "hidden"); }));
	CHECK(window.call("settings.get", {{"key", "ui.layout"}}).dump().find("notepad") != std::string::npos);
	// Show All shows it again (p. 167).
	if (!hasClass(window.app, "#inspector-setup", "open"))
		window.click(".inspector [row=volume] .inspector-label", ui::MouseButton::Right);
	window.click("#inspector-setup [setup-all]");
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, ".inspector [section=notepad]", "hidden"); }));

	// Another track: the group has no input or record buttons, but switches its time base (pp. 179-180); a marker
	// track has no channel to show.
	window.list.select({group});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Drums"));
	CHECK(hasClass(window.app, ".inspector [row=input]", "unavailable"));
	CHECK(hasClass(window.app, ".inspector button[control=record]", "unavailable"));
	CHECK_FALSE(hasClass(window.app, timeBase, "unavailable"));
	window.click(timeBase);
	REQUIRE(window.app.waitUntil([&] { return window.find("tracks", group).at("timeBase") == "linear"; }));
	// A VCA fader track switches its time base too (p. 181).
	window.list.select({vca});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Strings"));
	CHECK_FALSE(hasClass(window.app, timeBase, "unavailable"));
	window.click(timeBase);
	REQUIRE(window.app.waitUntil([&] { return window.find("tracks", vca).at("timeBase") == "linear"; }));
	window.list.select({marker});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, ".inspector [section=basics]", "unavailable"); }));
	CHECK(hasClass(window.app, ".inspector [section=notepad]", "unavailable"));
}

TEST_CASE("the inspector's setup pane saves and recalls presets, and Reset All brings the default order back", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 1; }));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	ui::Docking& docking = window.app.appUi().docking();
	const std::vector<std::string> defaults = docking.sections("project", "inspector");
	REQUIRE(defaults.size() > 1);
	const auto openPane = [&window]
	{
		if (!hasClass(window.app, "#inspector-setup", "open"))
			window.click(".inspector [row=volume] .inspector-label", ui::MouseButton::Right);
		REQUIRE(hasClass(window.app, "#inspector-setup", "open"));
	};
	const auto notepadHidden = [&window] { return hasClass(window.app, ".inspector [section=notepad]", "hidden"); };

	// The notepad hidden and the last section first.
	openPane();
	window.click("#inspector-setup [setup-section=notepad]");
	REQUIRE(window.app.waitUntil(notepadHidden));
	docking.moveSection("project", "inspector", defaults.back(), defaults.front());
	const std::vector<std::string> moved = docking.sections("project", "inspector");
	REQUIRE(moved.front() == defaults.back());

	// Save Preset, whose OK waits for a name.
	openPane();
	window.click("#inspector-setup [setup-preset-save]");
	REQUIRE(window.app.waitUntil([&window] { return !elements(window.app, "#setup-preset-name").empty(); }));
	CHECK(elements(window.app, "#setup-preset-ok").at(0).at("attributes").contains("disabled"));
	window.click("#setup-preset-name");
	window.app.ui().text(std::nullopt, "Lean");
	REQUIRE(window.app.waitUntil(
		[&window] { return !elements(window.app, "#setup-preset-ok").at(0).at("attributes").contains("disabled"); }));
	window.click("#setup-preset-ok");
	REQUIRE(window.app.waitUntil([&window]
		{ return window.call("settings.get", {{"key", "ui.setupPresets"}}).at("value").contains("inspector"); }));

	// Reset All: the default order and sections.
	openPane();
	window.click("#inspector-setup [setup-reset]");
	REQUIRE(window.app.waitUntil([&] { return !notepadHidden(); }));
	CHECK(docking.sections("project", "inspector") == defaults);

	// The preset brings its order and hidden sections back.
	openPane();
	window.click("#inspector-setup [setup-preset=Lean]");
	REQUIRE(window.app.waitUntil(notepadHidden));
	CHECK(docking.sections("project", "inspector") == moved);
}

TEST_CASE("the info line's setup pane shows, hides and orders its fields, with presets, Show All and Reset All", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	ui::Docking& docking = window.app.appUi().docking();
	const std::vector<std::string> defaults = docking.sections("project", "infoLine");
	CHECK(defaults ==
		std::vector<std::string>{"name", "start", "end", "length", "volume", "mute", "lock", "transpose", "velocity",
			"rootKey", "globalTranspose"});
	const auto openPane = [&window]
	{
		if (!hasClass(window.app, "#info-line-setup", "open"))
			window.click("#info-line [info-field=name] .info-label", ui::MouseButton::Right);
		REQUIRE(hasClass(window.app, "#info-line-setup", "open"));
	};
	const auto muteHidden = [&window] { return hasClass(window.app, "#info-line [section=mute]", "hidden"); };

	// Hidden, a field stays hidden when the info line shows the selection again, which makes it available.
	openPane();
	CHECK(elements(window.app, "#info-line-setup [setup-section]").size() == defaults.size());
	window.click("#info-line-setup [setup-section=mute]");
	REQUIRE(window.app.waitUntil(muteHidden));
	CHECK_FALSE(element(window.app, "#info-line button[field=mute]").at("visible").get<bool>());
	window.call("midiPart.set", {{"part", part}, {"name", "Verse"}});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Verse"));
	CHECK(muteHidden());
	CHECK_FALSE(hasClass(window.app, "#info-line [section=mute]", "unavailable"));
	// A field the selection doesn't have stays unavailable when Show All shows every section.
	openPane();
	window.click("#info-line-setup [setup-all]");
	REQUIRE(window.app.waitUntil([&] { return !muteHidden(); }));
	CHECK(element(window.app, "#info-line button[field=mute]").at("visible").get<bool>());
	CHECK_FALSE(element(window.app, "#info-line valuefield[field=volume]").at("visible").get<bool>());

	// A preset with the mute hidden and the last field first.
	openPane();
	window.click("#info-line-setup [setup-section=mute]");
	REQUIRE(window.app.waitUntil(muteHidden));
	docking.moveSection("project", "infoLine", defaults.back(), defaults.front());
	const std::vector<std::string> moved = docking.sections("project", "infoLine");
	REQUIRE(moved.front() == defaults.back());
	openPane();
	window.click("#info-line-setup [setup-preset-save]");
	REQUIRE(window.app.waitUntil([&window] { return !elements(window.app, "#setup-preset-name").empty(); }));
	window.click("#setup-preset-name");
	window.app.ui().text(std::nullopt, "Short");
	REQUIRE(window.app.waitUntil(
		[&window] { return !elements(window.app, "#setup-preset-ok").at(0).at("attributes").contains("disabled"); }));
	window.click("#setup-preset-ok");
	REQUIRE(window.app.waitUntil([&window]
		{ return window.call("settings.get", {{"key", "ui.setupPresets"}}).at("value").contains("infoLine"); }));

	// Reset All, then the preset.
	openPane();
	window.click("#info-line-setup [setup-reset]");
	REQUIRE(window.app.waitUntil([&] { return !muteHidden(); }));
	CHECK(docking.sections("project", "infoLine") == defaults);
	openPane();
	window.click("#info-line-setup [setup-preset=Short]");
	REQUIRE(window.app.waitUntil(muteHidden));
	CHECK(docking.sections("project", "infoLine") == moved);
}

TEST_CASE("the toolbar's setup pane shows, hides and orders its sections, with presets, Show All and Reset All", "[ui]")
{
	Window window;
	ui::Docking& docking = window.app.appUi().docking();
	const std::vector<std::string> defaults = docking.sections("project", "toolbar");
	CHECK(defaults ==
		std::vector<std::string>{"tools", "autoScroll", "zoomPresets", "snap", "gridType", "quantize", "color",
			"trackVisibility", "rootKey", "zones"});
	const auto openPane = [&window]
	{
		if (!hasClass(window.app, "#toolbar-setup", "open"))
			window.click("#toolbar", ui::MouseButton::Right);
		REQUIRE(hasClass(window.app, "#toolbar-setup", "open"));
	};
	const auto snapHidden = [&window] { return hasClass(window.app, "#toolbar [section=snap]", "hidden"); };
	// How far the first section of an order starts from the toolbar's left and the last ends from its right.
	const auto insets = [&window](const std::vector<std::string>& order)
	{
		window.app.shell().step(std::chrono::milliseconds(0));
		const Json bar = element(window.app, "#toolbar").at("box");
		const Json first = element(window.app, "#toolbar [section=" + order.front() + "]").at("box");
		const Json last = element(window.app, "#toolbar [section=" + order.back() + "]").at("box");
		return std::pair{first.at("x").get<double>() - bar.at("x").get<double>(),
			bar.at("x").get<double>() + bar.at("width").get<double>() - last.at("x").get<double>() -
				last.at("width").get<double>()};
	};
	const std::pair<double, double> edges = insets(defaults);
	const auto atEdges = [&](const std::vector<std::string>& order)
	{
		const std::pair<double, double> now = insets(order);
		CHECK(std::abs(now.first - edges.first) <= 1.0);
		CHECK(std::abs(now.second - edges.second) <= 1.0);
		// The spacer stays before the last place, also where a wide toolbar leaves it no room.
		CHECK(element(window.app, "#toolbar .spacer + div").at("attributes").at("section") == order.back());
	};

	// Hiding a section hides its controls and keeps it in the user's layout; Show All shows it again.
	openPane();
	CHECK(elements(window.app, "#toolbar-setup [setup-section]").size() == defaults.size());
	window.click("#toolbar-setup [setup-section=snap]");
	REQUIRE(window.app.waitUntil(snapHidden));
	CHECK_FALSE(element(window.app, "#snap").at("visible").get<bool>());
	CHECK(window.call("settings.get", {{"key", "ui.layout"}})
			.at("value")
			.at("windows")
			.at("project")
			.contains("sections"));
	openPane();
	window.click("#toolbar-setup [setup-all]");
	REQUIRE(window.app.waitUntil([&] { return !snapHidden(); }));
	CHECK(element(window.app, "#snap").at("visible").get<bool>());

	// A preset with the snap hidden and the tools last.
	openPane();
	window.click("#toolbar-setup [setup-section=snap]");
	REQUIRE(window.app.waitUntil(snapHidden));
	docking.moveSection("project", "toolbar", defaults.front(), defaults.back());
	const std::vector<std::string> moved = docking.sections("project", "toolbar");
	REQUIRE(moved.back() == defaults.front());
	atEdges(moved); // the spacer stays in its place, so the sections still fill the toolbar from end to end
	openPane();
	window.click("#toolbar-setup [setup-preset-save]");
	REQUIRE(window.app.waitUntil([&window] { return !elements(window.app, "#setup-preset-name").empty(); }));
	window.click("#setup-preset-name");
	window.app.ui().text(std::nullopt, "Editing");
	REQUIRE(window.app.waitUntil(
		[&window] { return !elements(window.app, "#setup-preset-ok").at(0).at("attributes").contains("disabled"); }));
	window.click("#setup-preset-ok");
	REQUIRE(window.app.waitUntil([&window]
		{ return window.call("settings.get", {{"key", "ui.setupPresets"}}).at("value").contains("toolbar"); }));

	// Reset All, then the preset.
	openPane();
	window.click("#toolbar-setup [setup-reset]");
	REQUIRE(window.app.waitUntil([&] { return !snapHidden(); }));
	CHECK(docking.sections("project", "toolbar") == defaults);
	atEdges(defaults);
	openPane();
	window.click("#toolbar-setup [setup-preset=Editing]");
	REQUIRE(window.app.waitUntil(snapHidden));
	CHECK(docking.sections("project", "toolbar") == moved);
	atEdges(moved);
}

TEST_CASE("an edit in the inspector goes to the track it started on when another track shows", "[ui]")
{
	Window window;
	const uint64_t vox = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t bass = window.addTrack({{"type", "audio"}, {"name", "Bass"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	window.list.select({vox});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));

	// Notes typed for one track, then another track selected: the notes are the first one's, not the other's.
	window.click(".inspector textarea.notepad");
	window.app.ui().text(std::nullopt, "Double the chorus");
	window.list.select({bass});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bass"));
	REQUIRE(window.app.waitUntil([&] { return window.channel(vox).value("notes", "") == "Double the chorus"; }));
	window.click(".inspector textarea.notepad");
	window.app.ui().key(std::nullopt, "Escape");
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.channel(bass).value("notes", "").empty());

	// The same for a name being typed.
	window.click(".inspector textdisplay.inspector-name", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "Low End");
	window.list.select({vox});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	REQUIRE(window.app.waitUntil([&] { return window.find("tracks", bass).at("name") == "Low End"; }));
	CHECK(window.find("tracks", vox).at("name") == "Vox");
}

TEST_CASE("a drag or wheel turns on a volume or pan field is one undo step, up to the project's maximum", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}, {"timeBase", "linear"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 1; }));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	window.call("project.set", {{"maxVolume", 12.0}});
	REQUIRE(window.app.waitUntil(
		[&] { return attribute(window.app, ".inspector valuefield[control=volume]", "max") == "12"; }));

	// A drag up to the name, far past the maximum: one step, which goes back to 0 dB.
	int64_t position = window.historyPosition();
	window.app.ui().drag(std::nullopt, {".inspector valuefield[control=volume]", 0, std::nullopt},
		{".inspector textdisplay.inspector-name", 0, std::nullopt});
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == 12.0; }));
	CHECK(window.historyPosition() == position + 1);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == 0.0; }));

	// Wheel turns on the pan, until the mouse leaves: one step.
	position = window.historyPosition();
	for (int turn = 0; turn < 3; ++turn)
		window.app.ui().wheel(std::nullopt, {".inspector valuefield[control=pan]", 0, std::nullopt}, 0.0F, -1.0F);
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("pan") == 0.03; }));
	CHECK(window.historyPosition() == position + 1);
	window.app.ui().move(std::nullopt, {".inspector [row=pan] .inspector-label", 0, std::nullopt});
	window.app.ui().wheel(std::nullopt, {".inspector valuefield[control=pan]", 0, std::nullopt}, 0.0F, -1.0F);
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("pan") == 0.04; }));
	CHECK(window.historyPosition() == position + 2);

	// The info line's volume of an audio event, dragged down.
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const uint64_t event =
		window
			.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", 96000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "take"));
	position = window.historyPosition();
	window.app.ui().drag(std::nullopt, {"#info-line valuefield[field=volume]", 0, std::nullopt},
		{".inspector [section=notepad]", 0, std::nullopt});
	REQUIRE(window.app.waitUntil([&] { return window.find("audioEvents", event).at("volume") < -1.0; }));
	CHECK(window.historyPosition() == position + 1);
	window.call("history.undo");
	CHECK(window.app.waitUntil([&] { return window.find("audioEvents", event).at("volume") == 0.0; }));
}

TEST_CASE("the inspector's volume and pan fields dragged while the engine answers late follow the mouse", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 1; }));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));

	for (const auto& [control, key, scale] : {std::tuple{"volume", "volume", 1.0}, std::tuple{"pan", "pan", 100.0}})
	{
		INFO(control);
		const std::string field = ".inspector valuefield[control=" + std::string(control) + "]";
		const int64_t position = window.historyPosition();
		test::LateDrag drag(window.app, std::nullopt, field);
		const double dragged = drag.move(15, 1);
		CHECK(dragged < -5.0);
		drag.release();
		// The engine and the field agree on where the mouse left it, in one undo step.
		const std::string name = key;
		const double wanted = dragged / scale;
		REQUIRE(window.app.waitUntil(
			[&] { return window.channel(audio).at(name).get<double>() == Catch::Approx(wanted).margin(1e-5); }));
		window.app.shell().step(std::chrono::milliseconds(0));
		CHECK(drag.value() == Catch::Approx(dragged).margin(1e-5));
		CHECK(window.historyPosition() == position + 1);
	}

	// Wheel turns too.
	const std::string pan = ".inspector valuefield[control=pan]";
	const int64_t position = window.historyPosition();
	const double turned = test::lateSteps(window.app, std::nullopt, pan, 10, -1,
		[&] { window.app.ui().wheel(std::nullopt, {pan, 0, std::nullopt}, 0.0F, -1.0F); });
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("pan") == Catch::Approx(turned / 100.0); }));
	CHECK(window.historyPosition() == position + 1);

	// Without a gesture, a value set through MCP shows at once.
	window.call("channel.set", {{"channel", window.channel(audio).at("id")}, {"volume", -12.0}, {"pan", 0.25}});
	CHECK(window.shows(".inspector valuefield[control=volume]", "-12"));
	CHECK(window.shows(".inspector valuefield[control=pan]", "25"));
}

TEST_CASE("the info line's volume turned or dragged while the engine answers late follows the mouse", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const uint64_t event =
		window
			.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", 96000}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "take"));
	const std::string field = "#info-line valuefield[field=volume]";
	const auto volume = [&] { return window.find("audioEvents", event).at("volume").get<double>(); };

	// Wheel turns, which the field doesn't hold as it holds a drag: the project read after each step's history.changed
	// is older than the step that came after it (#716).
	int64_t position = window.historyPosition();
	const double turned = test::lateSteps(window.app, std::nullopt, field, 10, 1,
		[&] { window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F); });
	REQUIRE(window.app.waitUntil([&] { return volume() == Catch::Approx(turned).margin(1e-5); }));
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(test::controlValue(window.app, std::nullopt, field) == Catch::Approx(turned).margin(1e-5));
	CHECK(window.historyPosition() == position + 1);

	// A drag.
	window.app.ui().move(
		std::nullopt, {"#info-line textdisplay[field=name]", 0, std::nullopt}); // the wheel's gesture ends
	position = window.historyPosition();
	test::LateDrag drag(window.app, std::nullopt, field);
	const double dragged = drag.move(15, 1);
	CHECK(dragged < turned);
	drag.release();
	REQUIRE(window.app.waitUntil([&] { return volume() == Catch::Approx(dragged).margin(1e-5); }));
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(drag.value() == Catch::Approx(dragged).margin(1e-5));
	CHECK(window.historyPosition() == position + 1);
}

TEST_CASE("the inspector shows the channel as it is when a change is refused", "[ui]")
{
	Window window;
	const uint64_t drums = window.addTrack({{"type", "group"}, {"name", "Drums"}});
	const uint64_t bus = window.addTrack({{"type", "group"}, {"name", "Bus"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	// Drums play into Bus, so Bus into Drums would be a loop.
	const uint64_t busChannel = window.channel(bus).at("id").get<uint64_t>();
	window.call("channel.set", {{"channel", window.channel(drums).at("id")}, {"output", busChannel}});
	const std::string output = std::to_string(window.channel(bus).at("output").get<uint64_t>());
	window.list.select({bus});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bus"));
	REQUIRE(window.shows(".inspector select[control=output]", output));

	// Drums is the choice after the output bus.
	window.click(".inspector select[control=output]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=output]");
	// Anywhere in the log, not just last: the project window's editor.inPlace after each project answer can follow it.
	REQUIRE(window.app.waitUntil(
		[&]
		{
			const Json commands = window.call("engine.commandLog").at("commands");
			return std::any_of(commands.begin(), commands.end(), [](const Json& command)
				{ return command.at("command") == "channel.set" && command.contains("error"); });
		}));
	CHECK(window.shows(".inspector select[control=output]", output));
	CHECK(std::to_string(window.channel(bus).at("output").get<uint64_t>()) == output);
}

TEST_CASE("the info line asks for the selection only while it shows", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Intro");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));

	window.app.appUi().docking().setVisible("project", "infoLine", false);
	window.app.shell().step(std::chrono::milliseconds(0));
	window.call("midiPart.set", {{"part", part}, {"name", "Verse"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts")[0].at("name") == "Verse"; }));
	CHECK_FALSE(window.app.waitUntil([&]
		{ return attribute(window.app, "#info-line textdisplay[field=name]", "value") == "Verse"; },
		std::chrono::milliseconds(300)));
	window.app.appUi().docking().setVisible("project", "infoLine", true);
	CHECK(window.shows("#info-line textdisplay[field=name]", "Verse"));
}

TEST_CASE("another project drops the inspector's edits in progress, and text fields keep their arrow keys", "[ui]")
{
	Window window;
	const test::TempDir folder;
	const std::string file = (folder.path() / "song.dawproject").string();
	const uint64_t vox = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t bass = window.addTrack({{"type", "audio"}, {"name", "Bass"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	window.call("project.save", {{"path", file}});
	window.list.select({bass});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bass"));

	// Up and Down in the notepad stay there: the selected track doesn't change.
	window.click(".inspector textarea.notepad");
	window.app.ui().text(std::nullopt, "Double the chorus");
	for (const char* key : {"Up", "Down", "Down"})
		window.app.ui().key(std::nullopt, key);
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.list.selection() == std::vector<uint64_t>{bass});

	// Another client loads the project, whose tracks have the same ids: the notes typed are dropped, not saved to the
	// loaded project's track.
	const uint64_t loads = window.display.loads();
	window.call("project.load", {{"path", file}});
	REQUIRE(window.app.waitUntil([&] { return window.display.loads() > loads; }));
	CHECK(window.list.selection().empty()); // another project starts without a selection (#491)
	window.list.select({bass});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bass"));
	window.click(".inspector [row=volume] .inspector-label");
	CHECK_FALSE(window.app.waitUntil(
		[&] { return !window.channel(bass).value("notes", "").empty(); }, std::chrono::milliseconds(300)));

	// The same for a name being typed.
	window.click(".inspector textdisplay.inspector-name", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "Low End");
	window.app.ui().key(std::nullopt, "Up");
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.list.selection() == std::vector<uint64_t>{bass});
	const uint64_t reloads = window.display.loads();
	window.call("project.load", {{"path", file}});
	REQUIRE(window.app.waitUntil([&] { return window.display.loads() > reloads; }));
	window.list.select({bass});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bass"));
	window.click(".inspector [row=volume] .inspector-label");
	CHECK_FALSE(window.app.waitUntil(
		[&] { return window.find("tracks", bass).at("name") != "Bass"; }, std::chrono::milliseconds(300)));
	CHECK(window.find("tracks", vox).at("name") == "Vox");
}

TEST_CASE("the info line bounds typed start, end and length to the largest project position", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}, {"timeBase", "linear"}});
	const uint64_t part = window.addPart(midi, 48000, 48000, "Part");
	window.call("project.set", {{"timeFormat", "seconds"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "0:00:01.000"));

	window.type("#info-line textdisplay[field=end]", "999999999999999");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == time::g_maxPosition - 48000; }));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == 48000; }));
	window.type("#info-line textdisplay[field=length]", "999999999999999");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == time::g_maxPosition - 48000; }));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == 48000; }));
	window.type("#info-line textdisplay[field=start]", "999999999999999");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("position") == time::g_maxPosition; }));
}

TEST_CASE("the info line edits the selected MIDI parts' transpose and velocity, relatively or for all", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t first = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	const uint64_t second = window.addPart(midi, 12 * g_quarter, 8 * g_quarter, "Verse");
	window.call("midiPart.set", {{"part", second}, {"transpose", 2}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));
	window.display.select({second, first});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	for (const char* field : {"transpose", "velocity"})
		CHECK_FALSE(hasClass(window.app, "#info-line [info-field=" + std::string(field) + "]", "unavailable"));
	REQUIRE(window.shows("#info-line valuefield[field=transpose]", "0"));

	// Relative to the first part's value: both by as much, as one undo step.
	const int64_t before = window.historyPosition();
	window.type("#info-line valuefield[field=transpose]", "3");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("transpose") == 5; }));
	CHECK(window.part(first).at("transpose") == 3);
	CHECK(window.historyPosition() == before + 1);
	// With Ctrl the same for all, kept from -127 to 127 when relative.
	window.type("#info-line valuefield[field=velocity]", "-20", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("velocity") == -20; }));
	CHECK(window.part(first).at("velocity") == -20);
	window.call("midiPart.set", {{"part", second}, {"velocity", -120}});
	// The display has the other client's change before the edit, which is relative to it.
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return window.display.project().at("midiParts")[1].at("velocity") == -120 ||
				window.display.project().at("midiParts")[0].at("velocity") == -120;
		}));
	REQUIRE(window.shows("#info-line valuefield[field=velocity]", "-20"));
	window.type("#info-line valuefield[field=velocity]", "-30");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("velocity") == -30; }));
	CHECK(window.part(second).at("velocity") == -127);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("velocity") == -20; }));
	CHECK(window.part(second).at("velocity") == -120);

	// Changes from another client show; markers and audio events have no transpose.
	window.call("midiPart.set", {{"part", first}, {"transpose", -7}});
	CHECK(window.shows("#info-line valuefield[field=transpose]", "-7"));
	const uint64_t audio = window.addTrack({{"type", "audio"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const uint64_t event =
		window.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", 0}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	// The display drops ids it doesn't show yet from the selection, so it waits for the event first.
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 1; }));
	window.display.select({event});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "take"));
	CHECK(hasClass(window.app, "#info-line [info-field=transpose]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=velocity]", "unavailable"));
}

TEST_CASE("the inspector's MIDI Modifiers section shows and changes the track's modifiers", "[ui]")
{
	Window window;
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	const uint64_t vox = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK_FALSE(hasClass(window.app, ".inspector [section=modifiers]", "unavailable"));
	// The sections above it closed, so that its controls are in view.
	for (const char* section : {"basics", "routing"})
		window.click(".inspector [section=" + std::string(section) + "] .section-header");
	const auto modifiers = [&] { return window.channel(keys).at("modifiers"); };

	window.click(".inspector button[control=modifiers-bypass]");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("bypass") == true; }));
	REQUIRE(window.app.waitUntil(
		[&] { return hasClass(window.app, ".inspector button[control=modifiers-bypass]", "checked"); }));
	window.type(".inspector valuefield[control=modifiers-transpose]", "5");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("transpose") == 5; }));
	window.type(".inspector valuefield[control=modifiers-velocityShift]", "-12");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("velocityShift") == -12; }));
	// A fraction's numerator keeps its denominator.
	window.type(".inspector valuefield[control=modifiers-lengthCompression-1]", "4");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("lengthCompression") == Json{1, 4}; }));
	window.type(".inspector valuefield[control=modifiers-velocityCompression-0]", "3");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("velocityCompression") == Json{3, 1}; }));

	// The first generator's target is the choice after Off; positions show in ticks of 480 to a quarter note.
	window.click(".inspector select[control=modifiers-random-0-target]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=modifiers-random-0-target]");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("random")[0].at("target") == "position"; }));
	REQUIRE(window.app.waitUntil(
		[&] { return attribute(window.app, ".inspector [control=modifiers-random-0-minimum]", "min") == "-480"; }));
	window.type(".inspector valuefield[control=modifiers-random-0-minimum]", "-120");
	REQUIRE(window.app.waitUntil(
		[&] { return modifiers().at("random")[0].at("minimum") == -120 * (time::g_ticksPerQuarter / 480); }));
	// A range: what channel.set refuses, a minimum above the maximum, shows as it is again.
	window.click(".inspector select[control=modifiers-range-1-target]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=modifiers-range-1-target]");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("range")[1].at("target") == "velocityLimit"; }));
	window.type(".inspector valuefield[control=modifiers-range-1-maximum]", "100");
	REQUIRE(window.app.waitUntil([&] { return modifiers().at("range")[1].at("maximum") == 100; }));
	window.type(".inspector valuefield[control=modifiers-range-1-minimum]", "110");
	CHECK(window.shows(".inspector valuefield[control=modifiers-range-1-minimum]", "0"));
	CHECK(modifiers().at("range")[1].at("minimum") == 0);

	// Changes from another client show, and undo restores each change.
	window.call("channel.set", {{"channel", window.channel(keys).at("id")}, {"modifiers", {{"transpose", -3}}}});
	CHECK(window.shows(".inspector valuefield[control=modifiers-transpose]", "-3"));
	window.call("history.undo");
	REQUIRE(window.shows(".inspector valuefield[control=modifiers-transpose]", "5"));
	window.call("history.undo");
	CHECK(window.app.waitUntil([&] { return modifiers().at("range")[1].at("maximum") == 127; }));

	// Tracks without notes don't have it.
	window.list.select({vox});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	CHECK(hasClass(window.app, ".inspector [section=modifiers]", "unavailable"));
}

TEST_CASE("a MIDI modifier or a part's transpose dragged back to where it started while the engine answers late ends "
		  "there",
	"[ui]")
{
	Window window;
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	const uint64_t part = window.addPart(keys, 0, 4 * g_quarter, "Intro");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.list.select({keys});
	window.display.select({part});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	REQUIRE(window.shows("#info-line valuefield[field=transpose]", "0"));
	for (const char* section : {"basics", "routing"})
		window.click(".inspector [section=" + std::string(section) + "] .section-header");

	const auto dragBack = [&](const std::string& field, const std::function<int64_t()>& engineValue)
	{
		INFO(field);
		const int64_t position = window.historyPosition();
		// Down and back up to where it started before the UI hands on any answer: the project it shows still has the
		// value the drag ends at, while a step of the way is in flight and another waits.
		Rml::Context& context = window.app.main().context();
		const Json box = element(window.app, field).at("box");
		const int x = box.at("x").get<int>() + (box.at("width").get<int>() / 2);
		const int y = box.at("y").get<int>() + (box.at("height").get<int>() / 2);
		context.ProcessMouseMove(x, y, 0);
		context.ProcessMouseButtonDown(0, 0);
		for (int moved = 2; moved <= 12; moved += 2)
			context.ProcessMouseMove(x, y + moved, 0);
		CHECK(test::controlValue(window.app, std::nullopt, field) < 0.0);
		context.ProcessMouseMove(x, y, 0);
		context.ProcessMouseButtonUp(0, 0);
		CHECK(test::controlValue(window.app, std::nullopt, field) == 0.0);
		// The engine ends where the field does, not at a step of the way, and stays there: the gesture changed nothing
		// in the end, so it leaves at most one undo step.
		REQUIRE(window.app.waitUntil([&] { return engineValue() == 0; }));
		CHECK_FALSE(window.app.waitUntil([&] { return engineValue() != 0; }, std::chrono::milliseconds(300)));
		CHECK(window.historyPosition() - position <= 1);
		window.app.shell().step(std::chrono::milliseconds(0));
		CHECK(test::controlValue(window.app, std::nullopt, field) == 0.0);
	};
	dragBack(".inspector valuefield[control=modifiers-transpose]",
		[&] { return window.channel(keys).at("modifiers").at("transpose").get<int64_t>(); });
	dragBack(
		"#info-line valuefield[field=transpose]", [&] { return window.part(part).at("transpose").get<int64_t>(); });
}
