// SPDX-License-Identifier: GPL-3.0-or-later
// The info line and the inspector (#89, docs/features/inspector.md) in the app's project window, driven with
// ui::Automation as MCP's UI tools drive them: the info line shows the first selected event or part and its edits
// change every selected one, as one undo step; the inspector shows the selected track's basics, routing, inserts and
// notepad, and its controls change the track's channel.
#include "testutils/commandholds.h"
#include "testutils/testutils.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/midi/modifiers.h"
#include "engine/time/tempomap.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/markers.h"
#include "ui/rmlui.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
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
		explicit Window(int width = 1200, std::vector<std::unique_ptr<midi::Backend>> backends = midi::makeBackends())
			: app{{width, 600}, std::move(backends)}, display(app.appUi().showProject()), list(*app.appUi().trackList())
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

		test::UiApp app;
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

TEST_CASE("the info line renames audio parts, with the MIDI parts selected with them", "[ui]")
{
	Window window;
	const test::TempDir dir;
	test::Audio audio;
	audio.samples.assign(48000, 0.0F);
	const auto file = dir.path() / "silence.wav";
	test::writeWav(file, audio);
	const uint64_t track = window.addTrack({{"type", "audio"}, {"channels", 1}});
	const auto media = window.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
	const auto clip = window.call("clip.add", {{"media", media}}).at("clip").at("id");
	const auto event =
		window.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 24000}})
			.at("event")
			.at("id");
	const auto part = window.call("event.toPart", {{"events", {event}}}).at("events")[0].get<uint64_t>();
	const uint64_t midi = window.addPart(window.addTrack({{"type", "midi"}}), 0, 4 * g_quarter, "Keys");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioParts").size() == 1; }));

	window.display.select({part, midi});
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, "#info-line", "empty"); }));
	window.type("#info-line textdisplay[field=name]", "Verse");
	REQUIRE(window.app.waitUntil([&] { return window.find("audioParts", part).at("name") == "Verse"; }));
	CHECK(window.part(midi).at("name") == "Verse");
	// One undo step for both.
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.part(midi).at("name") == "Keys"; }));
	CHECK(window.find("audioParts", part).at("name") != "Verse");
}

TEST_CASE("the info line shows the note an in-place editor has selected, and edits it", "[ui]")
{
	// Reference manual p. 1259: the same information the key editor's info line shows, and its value editing
	// (p. 1140): relative to the first note, with Ctrl+Enter the same value for all.
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Keys");
	const Json added = window.call("note.add",
		{{"part", part},
			{"notes",
				{{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", 2 * g_quarter}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5},
						{"channel", 3}, {"releaseVelocity", 0.25}}}}});
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("notes").size() == 2; }));
	const uint64_t first = added.at("notes").at(0).at("id").get<uint64_t>();
	const uint64_t second = added.at("notes").at(1).at("id").get<uint64_t>();
	const auto noteOf = [&](uint64_t id)
	{
		const Json shown = window.part(part); // the temporary must outlive the loop over its notes
		for (const Json& note : shown.at("notes"))
			if (note.at("id") == id)
				return note;
		return Json(nullptr);
	};
	const std::string editor = "inPlace:" + std::to_string(midi);
	const auto select = [&](const Json& notes)
	{ window.call("editor.set", {{"editor", editor}, {"selection", notes}}); };
	window.call("editor.inPlace", {{"tracks", {midi}}, {"open", true}});
	REQUIRE(window.app.waitUntil(
		[&] { return window.call("editor.get", {{"editor", editor}}).value("open", false) == true; }));

	// One note selected: the info line shows it as the key editor's info line does; the other fields are
	// unavailable.
	select(Json::array({{{"part", part}, {"note", first}}}));
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "1.2.1.0"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "empty"));
	CHECK(attribute(window.app, "#info-line textdisplay[field=end]", "value") == "1.3.1.0");
	CHECK(attribute(window.app, "#info-line textdisplay[field=length]", "value") == "0.1.0.0");
	CHECK(attribute(window.app, "#info-line textdisplay[field=pitch]", "value") == "C3");
	CHECK(attribute(window.app, "#info-line valuefield[field=velocity]", "value") == "64");
	CHECK(attribute(window.app, "#info-line textdisplay[field=releaseVelocity]", "value") == "64");
	CHECK(attribute(window.app, "#info-line textdisplay[field=channel]", "value") == "1");
	CHECK(hasClass(window.app, "#info-line [info-field=name]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=volume]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=mute]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=transpose]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=rootKey]", "unavailable"));

	// Two notes: the one that starts first shows, in the multiple color.
	select(Json::array({{{"part", part}, {"note", second}}, {{"part", part}, {"note", first}}}));
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	CHECK(attribute(window.app, "#info-line textdisplay[field=pitch]", "value") == "C3");
	const auto steps = window.historyPosition();

	// A pitch typed moves both notes by as much, as one undo step; with Ctrl+Enter both get the value typed.
	window.type("#info-line textdisplay[field=pitch]", "D3");
	REQUIRE(window.app.waitUntil([&] { return noteOf(first).at("pitch") == 62; }));
	CHECK(noteOf(second).at("pitch") == 66);
	CHECK(window.historyPosition() == steps + 1);
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "D3"));
	window.type("#info-line textdisplay[field=pitch]", "C3", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return noteOf(second).at("pitch") == 60; }));
	CHECK(noteOf(first).at("pitch") == 60);

	// A velocity typed moves both by as much; a channel is set for all; a start moves both.
	window.type("#info-line valuefield[field=velocity]", "100");
	REQUIRE(window.app.waitUntil([&] { return noteOf(first).at("velocity").get<double>() > 0.78; }));
	CHECK(noteOf(second).at("velocity").get<double>() == noteOf(first).at("velocity").get<double>());
	REQUIRE(window.shows("#info-line valuefield[field=velocity]", "100"));
	window.type("#info-line textdisplay[field=channel]", "5");
	REQUIRE(window.app.waitUntil([&] { return noteOf(first).at("channel") == 4; }));
	CHECK(noteOf(second).at("channel") == 4);
	window.type("#info-line textdisplay[field=start]", "1.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return noteOf(first).at("position") == 2 * g_quarter; }));
	CHECK(noteOf(second).at("position") == 3 * g_quarter);
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "1.3.1.0"));

	// Undo takes the last edit back.
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return noteOf(first).at("position") == g_quarter; }));

	// A part selected in the project window wins; the editor's next selection change shows its notes again,
	// and an empty one leaves the part.
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Keys"));
	select(Json::array({{{"part", part}, {"note", first}}}));
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "C3"));
	select(Json::array());
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Keys"));

	// The editor closing ends its notes; a new selection shows them again.
	select(Json::array({{{"part", part}, {"note", first}}}));
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "C3"));
	window.call("editor.inPlace", {{"tracks", {midi}}, {"open", false}});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Keys"));
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

TEST_CASE(
	"the info line moves cycle markers by position alone, and parses a typed length at each marker (#599)", "[ui]")
{
	Window window;
	const uint64_t track = window.addTrack({{"type", "marker"}, {"timeBase", "musical"}});
	// 120 BPM for two bars, 60 BPM after, so that a time in seconds isn't the same stretch in beats everywhere.
	window.call("tempo.add", {{"position", 8 * g_quarter}, {"bpm", 60.0}, {"ramp", false}});
	const auto add = [&window, track](int64_t position, int64_t end, const char* name)
	{
		return window
			.call("marker.add",
				{{"track", track}, {"type", "cycle"}, {"position", position}, {"end", end}, {"name", name}})
			.at("marker")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t early = add(0, 4 * g_quarter, "Early");
	const uint64_t late = add(12 * g_quarter, 16 * g_quarter, "Late");
	const auto marker = [&window](uint64_t id) { return window.find("markers", id); };
	const ui::MarkerLines& lines = *window.app.appUi().markerLines();
	const auto select = [&window, &lines](const std::vector<uint64_t>& ids, const char* first)
	{
		window.app.appUi().markerLines()->setSelection(ids);
		window.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(window.shows("#info-line textdisplay[field=name]", first));
		REQUIRE(lines.selection() == ids);
	};

	// A new position alone: the marker keeps its length in beats, though 4 beats across the tempo change are not as
	// many seconds as before.
	select({early}, "Early");
	window.type("#info-line textdisplay[field=start]", "2.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return marker(early).at("position") == 6 * g_quarter; }));
	CHECK(marker(early).at("end") == 10 * g_quarter);

	// Ctrl+Enter on the length gives every selected cycle marker the length typed, counted from its own start.
	select({early, late}, "Early");
	window.type("#info-line textdisplay[field=length]", "0.2.0.0", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return marker(late).at("end") == 14 * g_quarter; }));
	CHECK(marker(early).at("end") == 8 * g_quarter);
}

TEST_CASE("an untouched Ctrl+Enter on a marker's start gives every selected marker the first one's position", "[ui]")
{
	Window window;
	window.addTrack({{"type", "marker"}});
	const auto add = [&window](const Json& parameters)
	{ return window.call("marker.add", parameters).at("marker").at("id").get<uint64_t>(); };
	const uint64_t verse =
		add({{"type", "cycle"}, {"position", 4 * g_quarter}, {"end", 8 * g_quarter}, {"name", "Verse"}});
	// Every label in view: CI's screens make the window 1024 pixels wide, where the timeline beside the inspector shows
	// less than 10 quarters.
	const uint64_t outro = add({{"position", 6 * g_quarter}, {"name", "Outro"}});
	// The first marker in time added last: the info line shows the first one in timeline order, not by id (#684).
	const uint64_t intro = add({{"position", 2 * g_quarter}, {"name", "Intro"}});
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

TEST_CASE("the info line shows the selected automation points, and its Terminator field makes a gap", "[ui]")
{
	Window window;
	const uint64_t track = window.addTrack({{"type", "audio"}});
	window.call("track.set", {{"track", track}, {"timeBase", "musical"}});
	const auto lane = window.call("automationLane.add", {{"track", track}, {"target", {{"type", "volume"}}}})
						  .at("lane")
						  .at("id")
						  .get<uint64_t>();
	// The first point in time added last: the one that starts first shows.
	window.call("automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 2 * g_quarter}, {"value", -6.0}}, {{"position", 4 * g_quarter}, {"value", 0.0}},
					{{"position", g_quarter}, {"value", -12.0}}}}});
	const auto points = [&window, track]
	{
		const Json all = window.call("automationLane.list", {{"track", track}}).at("lanes").at(0).at("points");
		return std::vector<Json>(all.begin(), all.end()); // by position
	};
	const uint64_t first = points().at(0).at("id").get<uint64_t>();
	const uint64_t second = points().at(1).at("id").get<uint64_t>();
	window.display.showAutomation(track, true);
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return window.display.automationTargets(track) != nullptr &&
				!window.display.project().at("automation").empty();
		}));

	// Two selected: the first one's position and value, the other fields unavailable.
	window.display.selectPoints({{lane, {second, first}}});
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "1.2.1.0"));
	CHECK(hasClass(window.app, "#info-line", "points"));
	CHECK(hasClass(window.app, "#info-line", "multiple"));
	CHECK(attribute(window.app, "#info-line textdisplay[field=value]", "value") == "-12");
	CHECK(element(window.app, "#info-line [info-field=start] .info-label").at("text") == "Position");
	CHECK(hasClass(window.app, "#info-line [info-field=name]", "unavailable"));
	CHECK(hasClass(window.app, "#info-line [info-field=volume]", "unavailable"));
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=terminator]", "unavailable"));
	CHECK(element(window.app, "#info-line button[field=terminator]").at("text") == "No");

	// Relative to the first one's position and value, each one an undo step.
	const int64_t position = window.historyPosition();
	window.type("#info-line textdisplay[field=start]", "1.3.1.0");
	REQUIRE(window.app.waitUntil([&] { return points().at(0).at("position") == 2 * g_quarter; }));
	CHECK(points().at(1).at("position") == 3 * g_quarter);
	window.type("#info-line textdisplay[field=value]", "-9");
	REQUIRE(window.app.waitUntil([&] { return points().at(0).at("value") == Catch::Approx(-9.0); }));
	CHECK(points().at(1).at("value") == Catch::Approx(-3.0));
	CHECK(window.historyPosition() == position + 2);
	// With Ctrl+Enter the value typed for all, kept in the parameter's range.
	window.type("#info-line textdisplay[field=value]", "100", "Ctrl+Enter");
	REQUIRE(window.app.waitUntil([&] { return points().at(0).at("value") == Catch::Approx(6.0); }));
	CHECK(points().at(1).at("value") == Catch::Approx(6.0));
	CHECK(points().at(2).at("value") == Catch::Approx(0.0));

	// One point selected, Terminator set to Yes: the gap up to the next point (p. 881), one undo step.
	window.display.selectPoints({{lane, {second}}});
	REQUIRE(window.shows("#info-line textdisplay[field=start]", "1.4.1.0"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "multiple"));
	const int64_t before = window.historyPosition();
	window.click("#info-line button[field=terminator]");
	REQUIRE(window.app.waitUntil([&] { return points().at(1).value("terminator", false); }));
	CHECK_FALSE(points().at(0).value("terminator", false));
	CHECK(window.historyPosition() == before + 1);
	REQUIRE(window.app.waitUntil(
		[&] { return element(window.app, "#info-line button[field=terminator]").at("text") == "Yes"; }));
	// Clicked again, it clears Terminator.
	window.click("#info-line button[field=terminator]");
	REQUIRE(window.app.waitUntil([&] { return !points().at(1).value("terminator", false); }));
	REQUIRE(window.app.waitUntil(
		[&] { return element(window.app, "#info-line button[field=terminator]").at("text") == "No"; }));
	CHECK(window.historyPosition() == before + 2);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return points().at(1).value("terminator", false); }));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return !points().at(1).value("terminator", false); }));

	// An event selected gives the info line back to the events.
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Part");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Part"));
	CHECK_FALSE(hasClass(window.app, "#info-line", "points"));
	CHECK(window.display.selectedPoints().empty());
	CHECK(hasClass(window.app, "#info-line [info-field=terminator]", "unavailable"));
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=name]", "unavailable"));

	// A marker selected after the points shows its description.
	window.addTrack({{"type", "marker"}});
	const uint64_t marker = window.call("marker.add", {{"position", 2 * g_quarter}, {"name", "Intro"}})
								.at("marker")
								.at("id")
								.get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("markers").size() == 1; }));
	window.display.selectPoints({{lane, {first}}});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "points"); }));
	window.app.appUi().markerLines()->pick({marker}); // as a click does, which deselects the points (#2011)
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	CHECK(hasClass(window.app, "#info-line", "markers"));
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=name]", "unavailable"));
	CHECK(element(window.app, "#info-line [info-field=name] .info-label").at("text") == "Description");
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
	// Escape closes the pane, which would cover the buttons below.
	window.app.ui().key(std::nullopt, "Escape");
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, "#inspector-setup", "open"); }));

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
		std::vector<std::string>{"name", "start", "end", "length", "topTrack", "bottomTrack", "pitch", "volume", "mute",
			"lock", "transpose", "velocity", "releaseVelocity", "channel", "rootKey", "globalTranspose", "value",
			"terminator"});
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

namespace
{
	// What the user sees of the project window's toolbar, measured at its visible buttons rather than at the sections'
	// boxes, which hold margins of their own and shrink to fit (#1050).
	struct ToolbarGaps
	{
		double inset = 0.0;    // from the toolbar's left edge to its first visible button
		double within = 0.0;   // between two buttons of one section
		double tightest = 0.0; // the smallest gap between two neighbouring buttons, negative where one covers the next
		double spacer = 0.0;   // the width of the spacer before the last place
		// From a section's last visible button to the first of the next one, except before the last place, which the
		// spacer widens.
		std::vector<double> between;
	};

	// Steps the shell first, unless step is false, to measure what the last step drew.
	ToolbarGaps toolbarGaps(Window& window, const std::vector<std::string>& order, bool step = true)
	{
		if (step)
			window.app.shell().step(std::chrono::milliseconds(0));
		ToolbarGaps result;
		result.tightest = 1.0e9;
		result.spacer = element(window.app, "#toolbar .spacer").at("box").at("width").get<double>();
		const double barLeft = element(window.app, "#toolbar").at("box").at("x").get<double>();
		double previousEnd = 0.0;
		bool anyShown = false;
		for (const std::string& name : order)
		{
			bool firstOfSection = true;
			for (const Json& button : elements(window.app, "#toolbar [section=" + name + "] button"))
			{
				if (!button.at("visible").get<bool>())
					continue;
				const double x = button.at("box").at("x").get<double>();
				if (!anyShown)
				{
					result.inset = x - barLeft;
				}
				else
				{
					result.tightest = std::min(result.tightest, x - previousEnd);
					if (!firstOfSection)
						result.within = x - previousEnd;
					else if (name != order.back())
						result.between.push_back(x - previousEnd);
				}
				firstOfSection = false;
				anyShown = true;
				previousEnd = x + button.at("box").at("width").get<double>();
			}
		}
		return result;
	}

	// Every gap between sections is the same and wider than one inside a section, and the first button stands as far
	// from the toolbar's edge as in the order `expected` measured, whichever sections stand where.
	void checkToolbarGaps(const ToolbarGaps& now, const ToolbarGaps& expected, double ratio)
	{
		REQUIRE(!now.between.empty());
		// 4 dp wider, which pixels round to less at small dp ratios: 3 against 2 pixels at 0.7, so wider by more than a
		// dp (#1239).
		CHECK(now.between.front() > now.within + ratio);
		// The boxes round their position and width each on its own, so a gap measures up to 1.5 pixels off: two equal
		// gaps differ by up to 2 (4.85 pixels each measure 4 to 6 at 0.97).
		for (const double gap : now.between)
			CHECK(std::abs(gap - now.between.front()) <= 2.0);
		CHECK(std::abs(now.inset - expected.inset) <= 1.0);
	}
}

TEST_CASE("the inspector's record and monitor buttons switch those of MIDI and instrument tracks", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}, {"name", "Keys"}});
	const uint64_t instrument = window.addTrack({{"type", "instrument"}, {"name", "Synth"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	for (const std::pair<uint64_t, const char*>& each : {std::pair{midi, "Keys"}, std::pair{instrument, "Synth"}})
	{
		const uint64_t track = each.first;
		const std::string name = each.second;
		window.list.select({track});
		REQUIRE(window.shows(".inspector textdisplay.inspector-name", name));
		for (const char* control : {"record", "monitor"})
		{
			INFO(name << " " << control);
			const std::string key = std::string(control) == "record" ? "recordEnable" : control;
			const std::string button = ".inspector button[control=" + std::string(control) + "]";
			const bool before = window.channel(track).at(key).get<bool>();
			window.click(button);
			REQUIRE(window.app.waitUntil([&] { return window.channel(track).at(key) == !before; }));
			REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, button, "checked") != before; }));
			window.call("channel.set", {{"channel", window.channel(track).at("id")}, {key, before}});
			REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, button, "checked") == before; }));
		}
	}
}

TEST_CASE("the inspector's Retrospective Recording menu inserts the track's buffer linearly or stacked, or empties it",
	"[ui]")
{
	// Reference manual pp. 354-356.
	Window window;
	const Json added = window.call("track.add", {{"type", "instrument"}, {"name", "Synth"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	const Json channel = added.at("channel").at("id");
	window.call("channel.set", {{"channel", channel}, {"instrument", "noteProbe"}});
	window.list.select({track});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Synth"));
	const auto play = [&](int pitch, int64_t from, int64_t to, int64_t frames)
	{
		const auto now = window.call("audio.get").at("position").get<int64_t>();
		window.call("midi.inject",
			{{"channel", channel},
				{"messages",
					{{{"position", now + from}, {"bytes", {0x90, pitch, 100}}},
						{{"position", now + to}, {"bytes", {0x80, pitch, 0}}}}}});
		window.call("audio.render", {{"frames", frames}});
	};
	// Insert as Cycle Recording is enabled once the track's buffer says it holds two cycle laps (#2067).
	const std::string cycleDisabled = "#inspector-retrospective-menu menuitem.retrospective-cycle[disabled]";
	const auto choose = [&](const std::string& item)
	{
		window.click(".inspector .inspector-retrospective");
		if (item == "retrospective-cycle")
			REQUIRE(window.app.waitUntil([&] { return elements(window.app, cycleDisabled).empty(); }));
		window.click("#inspector-retrospective-menu menuitem." + item);
	};
	const auto pitches = [&]
	{
		std::vector<std::vector<int64_t>> parts;
		const Json project = window.call("project.get");
		for (const Json& part : project.at("midiParts"))
		{
			parts.emplace_back();
			for (const Json& note : part.at("notes"))
				parts.back().push_back(note.at("pitch").get<int64_t>());
		}
		std::sort(parts.begin(), parts.end());
		return parts;
	};

	// Empty Retrospective Record Buffer, then Insert as Linear Recording: only what came after.
	play(60, 100, 1000, 2000);
	choose("retrospective-empty");
	play(62, 100, 1000, 2000);
	choose("retrospective-linear");
	REQUIRE(window.app.waitUntil([&] { return pitches() == std::vector<std::vector<int64_t>>{{62}}; }));
	// Without input from cycle laps it stays disabled.
	play(63, 100, 1000, 2000);
	window.click(".inspector .inspector-retrospective");
	CHECK(window.call("midi.getRetrospective", {{"track", track}}).at("cycleLaps") == 0);
	REQUIRE(window.app.waitUntil([&] { return elements(window.app, cycleDisabled).size() == 1; }));
	window.click("#inspector-retrospective-menu menuitem.retrospective-empty");

	// Insert as Cycle Recording: a part per lap.
	window.call("transport.set", {{"left", 0}, {"right", 20000}, {"cycle", true}});
	window.call("transport.play");
	play(64, 5000, 8000, 20000);
	play(65, 5000, 8000, 10000);
	window.call("transport.stop");
	window.call("audio.render", {{"frames", 64}});
	choose("retrospective-cycle");
	REQUIRE(window.app.waitUntil([&] { return pitches() == std::vector<std::vector<int64_t>>{{62}, {64}, {65}}; }));
	CHECK(elements(window.app, ".inspector .inspector-retrospective.unavailable").empty());
	window.list.select({window.addTrack({{"type", "audio"}})});
	CHECK(window.app.waitUntil(
		[&] { return hasClass(window.app, ".inspector .inspector-retrospective", "unavailable"); }));
}

TEST_CASE("the inspector's S solos exclusively and switches solo defeat, and its L listens", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t other = window.addTrack({{"type", "audio"}, {"name", "Bass"}});
	const uint64_t midi = window.addTrack({{"type", "midi"}, {"name", "Keys"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 3; }));
	window.call("channel.set", {{"channel", window.channel(other).at("id")}, {"solo", true}});
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	const std::string solo = ".inspector button[control=solo]";
	const auto clickWith = [&window](const std::string& selector, const char* modifier)
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {modifier});
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// Ctrl-click solos exclusively; Alt-click and click and hold switch solo defeat, which S shows (reference manual
	// p. 466).
	clickWith(solo, "ctrl");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("solo") == true; }));
	CHECK(window.channel(other).at("solo") == false);
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, solo, "checked"); }));
	clickWith(solo, "alt");
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, solo, "defeated"); }));
	CHECK(window.channel(audio).at("soloDefeat") == true);
	window.app.ui().press(std::nullopt, {solo, 0, std::nullopt}, ui::MouseButton::Left);
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->advance(1.0);
	window.app.ui().release(std::nullopt, {solo, 0, std::nullopt}, ui::MouseButton::Left);
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, solo, "defeated"); }));
	CHECK(window.channel(audio).at("soloDefeat") == false);
	CHECK(window.channel(audio).at("solo") == true);

	// L listens, on channels with audio.
	const std::string listen = ".inspector button[control=listen]";
	CHECK_FALSE(hasClass(window.app, listen, "unavailable"));
	window.click(listen);
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("listen") == true; }));
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, listen, "checked"); }));
	window.call("channel.set", {{"channel", window.channel(audio).at("id")}, {"listen", false}});
	REQUIRE(window.app.waitUntil([&] { return !hasClass(window.app, listen, "checked"); }));
	window.list.select({midi});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK(hasClass(window.app, listen, "unavailable"));
}

TEST_CASE("the toolbar's state buttons deactivate all mute, solo and listen states", "[ui]")
{
	Window window;
	// CI's monitors can be too narrow for every toolbar section, so only the state buttons are shown.
	ui::Docking& docking = window.app.appUi().docking();
	for (const std::string& section : docking.sections("project", "toolbar"))
		docking.setSectionHidden("project", "toolbar", section, section != "states");
	const uint64_t audio = window.addTrack({{"type", "audio"}});
	const uint64_t other = window.addTrack({{"type", "audio"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	const auto lit = [&window](const std::string& state)
	{ return hasClass(window.app, "#toolbar button[deactivate=" + state + "]", "checked"); };
	const Json audioChannel = window.channel(audio).at("id");
	const Json otherChannel = window.channel(other).at("id");

	// They light up while any channel has their state, solo defeat counting as solo (reference manual p. 58).
	CHECK_FALSE(lit("solo"));
	window.call("channel.set", {{"channel", otherChannel}, {"soloDefeat", true}});
	REQUIRE(window.app.waitUntil([&] { return lit("solo"); }));
	window.call("channel.set", {{"channel", audioChannel}, {"solo", true}, {"mute", true}});
	window.call("channel.set", {{"channel", otherChannel}, {"listen", true}});
	REQUIRE(window.app.waitUntil([&] { return lit("mute") && lit("listen"); }));
	// A click turns the state off on every channel.
	for (const char* state : {"mute", "solo", "listen"})
		window.click(std::string("#toolbar button[deactivate=") + state + "]");
	REQUIRE(window.app.waitUntil([&] { return !lit("mute") && !lit("solo") && !lit("listen"); }));
	CHECK(window.channel(audio).at("mute") == false);
	CHECK(window.channel(audio).at("solo") == false);
	CHECK(window.channel(other).at("soloDefeat") == false);
	CHECK(window.channel(other).at("listen") == false);
}

TEST_CASE("the toolbar's setup pane shows, hides and orders its sections, with presets, Show All and Reset All", "[ui]")
{
	// 1300 dp is wide enough for every section, so the buttons are all in the clear.
	Window window(1300);
	ui::Docking& docking = window.app.appUi().docking();
	const std::vector<std::string> defaults = docking.sections("project", "toolbar");
	CHECK(defaults ==
		std::vector<std::string>{"tools", "autoScroll", "zoomPresets", "snap", "gridType", "quantize", "color",
			"trackVisibility", "states", "rootKey", "zones"});
	const auto openPane = [&window]
	{
		if (!hasClass(window.app, "#toolbar-setup", "open"))
			window.click("#toolbar", ui::MouseButton::Right);
		REQUIRE(hasClass(window.app, "#toolbar-setup", "open"));
	};
	const auto snapHidden = [&window] { return hasClass(window.app, "#toolbar [section=snap]", "hidden"); };
	// How far the last section of an order ends from the toolbar's right edge.
	const auto rightInset = [&window](const std::vector<std::string>& order)
	{
		window.app.shell().step(std::chrono::milliseconds(0));
		const Json bar = element(window.app, "#toolbar").at("box");
		const Json last = element(window.app, "#toolbar [section=" + order.back() + "]").at("box");
		return bar.at("x").get<double>() + bar.at("width").get<double>() - last.at("x").get<double>() -
			last.at("width").get<double>();
	};
	const double rightEdge = rightInset(defaults);
	const ToolbarGaps reference = toolbarGaps(window, defaults);
	const auto atEdges = [&](const std::vector<std::string>& order)
	{
		// Every place between sections has the same gap, whichever sections are in it, and the first button the same
		// distance from the toolbar's left edge, as the user sees them (#1050).
		const ToolbarGaps now = toolbarGaps(window, order);
		checkToolbarGaps(now, reference, window.app.main().dpRatio());
		CHECK(now.tightest >= 0.0);
		CHECK(now.spacer >= 1.0);
		CHECK(std::abs(rightInset(order) - rightEdge) <= 1.0);
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

	// The quantize controls bring no gap of their own: first in the order they start as far from the left edge as the
	// tools do, and the other sections keep their gaps.
	openPane();
	window.click("#toolbar-setup [setup-reset]");
	REQUIRE(window.app.waitUntil([&] { return !snapHidden(); }));
	docking.moveSection("project", "toolbar", "quantize", "tools");
	const std::vector<std::string> quantizeFirst = docking.sections("project", "toolbar");
	REQUIRE(quantizeFirst.front() == "quantize");
	atEdges(quantizeFirst);
}

namespace
{
	// Checks what the room in the project window's toolbar shows of an order of its sections (#1178, #1188): the
	// shown sections before the spacer come first, those the window has no room for (.no-room) after them, and the
	// first of those would not fit where the spacer is; no button covers the next one, the gaps are those of the
	// order `expected` measured, and the last section ends as far from the toolbar's right edge as the first button
	// starts from its left one. Returns the sections shown. Steps the shell first, unless step is false.
	std::vector<std::string> checkToolbarRoom(
		Window& window, const std::vector<std::string>& order, const ToolbarGaps& expected, bool step = true)
	{
		const ToolbarGaps gaps = toolbarGaps(window, order, step);
		const double ratio = window.app.main().dpRatio();
		if (gaps.between.empty())
			CHECK(std::abs(gaps.inset - expected.inset) <= 1.0); // no gap between sections before the last place
		else
			checkToolbarGaps(gaps, expected, ratio);
		CHECK(gaps.tightest >= 0.0);
		CHECK(gaps.spacer >= 0.0);
		std::vector<std::string> shown;
		bool roomLeft = true;
		const bool warpGone = hasClass(window.app, "#toolbar-quantize-warp", "no-room");
		for (size_t i = 0; i + 1 < order.size(); ++i)
		{
			INFO("section " << order[i]);
			const Json section = element(window.app, "#toolbar [section=" + order[i] + "]");
			const bool noRoom = hasClass(window.app, "#toolbar [section=" + order[i] + "]", "no-room");
			CHECK(section.at("visible").get<bool>() == !noRoom);
			if (noRoom && roomLeft)
			{
				// With its 3 dp gap it needs more than the spacer's width.
				CHECK(section.at("box").at("width").get<double>() + (3.0 * ratio) > gaps.spacer);
				CHECK(section.at("box").at("width").get<double>() > 0.0); // laid out, where it can't be seen
			}
			CHECK((noRoom || roomLeft)); // the last before the spacer go first
			roomLeft = roomLeft && !noRoom;
			if (!noRoom)
				shown.push_back(order[i]);
		}
		CHECK(!hasClass(window.app, "#toolbar [section=" + order.back() + "]", "no-room")); // after the spacer
		shown.push_back(order.back());
		// Warp Quantize On/Off goes before any section, and only while the shown sections fit without it (#1234).
		if (warpGone)
		{
			CHECK(!element(window.app, "#toolbar-quantize-warp").at("visible").get<bool>());
			if (shown.size() == order.size()) // with its 2 dp margin it needs more than the spacer's width
				CHECK(
					element(window.app, "#toolbar-quantize-warp").at("box").at("width").get<double>() + (2.0 * ratio) >
					gaps.spacer);
		}
		else if (order.back() != "quantize" && std::find(shown.begin(), shown.end(), "quantize") != shown.end())
		{
			CHECK(shown.size() == order.size());
		}
		// The shown sections fit inside the toolbar. x and width are rounded separately at fractional dp ratios, so
		// the inset may differ by up to 2 px.
		const Json bar = element(window.app, "#toolbar").at("box");
		const Json last = element(window.app, "#toolbar [section=" + order.back() + "]").at("box");
		const double rightInset = bar.at("x").get<double>() + bar.at("width").get<double>() -
			last.at("x").get<double>() - last.at("width").get<double>();
		CHECK(std::abs(rightInset - gaps.inset) <= 2.0);
		return shown;
	}
}

TEST_CASE("the toolbar shows the sections it has room for, the last before the spacer going first", "[ui]")
{
	// The shell fits windows into their monitor, 1024 pixels wide on CI: smaller dp ratios give the wider dp widths,
	// 0.7 about 1463 dp, 0.8 1280 dp and 0.9316 about 1099.2 dp, between the widths of whole dp (#1178).
	for (const int width : {1024, 900, 840, 800, 700})
	{
		INFO("window width " << width);
		Window window(width);
		REQUIRE(window.app.main().pixelSize().width == width); // a smaller display would test something else
		ui::Docking& docking = window.app.appUi().docking();
		const std::vector<std::string> defaults = docking.sections("project", "toolbar");
		// Narrower in dp first, then wider: the sections the room hid show again.
		std::vector<float> ratios = {1.25F, 1.0F};
		if (width == 1024)
			ratios.insert(ratios.end(), {0.9316F, 0.8F, 0.7F});
		for (const float ratio : ratios)
		{
			const double dp = width / static_cast<double>(ratio);
			INFO("dp ratio " << ratio << ", " << dp << " dp");
			window.app.setDpRatio(ratio);
			docking.resetSections("project", "toolbar");
			const ToolbarGaps reference = toolbarGaps(window, defaults);
			const std::vector<std::string> shown = checkToolbarRoom(window, defaults, reference);
			if (dp >= 1400.0)
				CHECK(shown == defaults);
			if (dp <= 840.0)
				CHECK(shown.size() < defaults.size());

			// Quantize first: its controls have no gap of their own before them.
			docking.moveSection("project", "toolbar", "quantize", "tools");
			checkToolbarRoom(window, docking.sections("project", "toolbar"), reference);

			// The sections that go first in the default order, in places in the middle.
			docking.moveSection("project", "toolbar", "rootKey", "tools");
			docking.moveSection("project", "toolbar", "trackVisibility", "autoScroll");
			const std::vector<std::string> inside = docking.sections("project", "toolbar");
			REQUIRE(inside.at(1) == "rootKey");
			checkToolbarRoom(window, inside, reference);
		}
	}
}

TEST_CASE("Warp Quantize On/Off goes from the toolbar before any section does", "[ui]")
{
	// The reference's toolbar hides its items one after another as the window narrows (the reference manual, p. 65).
	Window window(1024);
	const ui::Docking& docking = window.app.appUi().docking();
	const std::vector<std::string> defaults = docking.sections("project", "toolbar");
	const auto noRoom = [&window](const std::string& selector) { return hasClass(window.app, selector, "no-room"); };
	const std::string warp = "#toolbar-quantize-warp";
	// Narrower in dp step by step, from about 1463 dp, until the quantize section goes, and on until the section
	// before it goes too: the steps, about 10 to 20 dp, are narrower than the toggle, so they pass through the room the
	// toggle frees, which only the quantize section may take (#1382).
	const auto quantize = std::find(defaults.begin(), defaults.end(), "quantize");
	REQUIRE(quantize != defaults.end());
	REQUIRE(quantize != defaults.begin());
	const std::string before = "#toolbar [section=" + *(quantize - 1) + "]";
	bool warpGone = false;
	bool quantizeGone = false;
	bool alone = false; // gone while every section showed
	for (int step = 0; !noRoom(before); ++step)
	{
		const float ratio = 0.7F + (0.01F * static_cast<float>(step));
		REQUIRE(ratio < 2.0F);
		INFO("dp ratio " << ratio);
		window.app.setDpRatio(ratio);
		window.app.shell().step(std::chrono::milliseconds(0));
		if (noRoom("#toolbar [section=quantize]"))
		{
			CHECK(!noRoom(warp)); // the section's parts show with it again
			quantizeGone = true;
		}
		else
		{
			CHECK(!quantizeGone); // once gone, it stays gone in narrower windows
			if (warpGone)
				CHECK(noRoom(warp)); // so does the toggle while the section shows
			warpGone = noRoom(warp);
		}
		const ToolbarGaps reference = toolbarGaps(window, defaults, false); // the gaps at this dp ratio
		const std::vector<std::string> shown = checkToolbarRoom(window, defaults, reference, false);
		alone = alone || (warpGone && shown.size() == defaults.size());
	}
	CHECK(quantizeGone);
	CHECK(alone);

	// Wider again: the section and its toggle show.
	window.app.setDpRatio(0.7F);
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(element(window.app, warp).at("visible").get<bool>());
	CHECK(!noRoom(warp));
}

TEST_CASE("the toolbar makes room for its sections with the widest labels, and for those the user hides", "[ui]")
{
	for (const int width : {1000, 840})
	{
		INFO("window width " << width);
		Window window(width);
		REQUIRE(window.app.main().pixelSize().width == width);
		ui::Docking& docking = window.app.appUi().docking();
		const std::vector<std::string> defaults = docking.sections("project", "toolbar");
		const ToolbarGaps reference = toolbarGaps(window, defaults);
		const std::vector<std::string> before = checkToolbarRoom(window, defaults, reference);

		// The snap type's and the grid type's longest names make their buttons wider (#1188).
		window.call("snap.set", {{"type", "eventsGridCursor"}, {"grid", "adaptToZoom"}});
		REQUIRE(window.app.waitUntil(
			[&] { return element(window.app, "#grid-type").value("text", "") == "Adapt to Zoom"; }));
		// Fitted in the step that shows the new labels, before the shell drew them.
		const std::vector<std::string> wide = checkToolbarRoom(window, defaults, reference, false);
		CHECK(wide.size() < before.size());

		// The sections the user hides in the setup pane make room for the next one, which comes after them: the tools,
		// the widest, make room for any one section, such as the Quantize section at 1000 pixels.
		REQUIRE(wide.size() >= 4);
		const std::string& next = defaults.at(wide.size() - 1); // the first one without room
		docking.setSectionHidden("project", "toolbar", "zoomPresets", true);
		docking.setSectionHidden("project", "toolbar", "tools", true);
		std::vector<std::string> order;
		for (const std::string& name : defaults)
			if (name != "zoomPresets" && name != "tools")
				order.push_back(name);
		const std::vector<std::string> roomMade = checkToolbarRoom(window, order, reference);
		CHECK(std::find(roomMade.begin(), roomMade.end(), next) != roomMade.end());
		CHECK(!hasClass(window.app, "#toolbar [section=zoomPresets]", "no-room"));
	}
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

TEST_CASE("a drag, wheel turns or a held arrow key on a volume or pan field is one undo step, up to the project's "
		  "maximum, and a typed value one of its own",
	"[ui]")
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

	// Up held on the focused volume, its key down repeating until it is released: one step. A typed value is one of
	// its own.
	window.click(".inspector valuefield[control=volume]");
	position = window.historyPosition();
	for (int repeat = 0; repeat < 3; ++repeat)
		window.app.ui().key(std::nullopt, "Up", ui::KeyAction::Down);
	window.app.ui().key(std::nullopt, "Up", ui::KeyAction::Up);
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == 1.5; }));
	CHECK(window.historyPosition() == position + 1);
	window.type(".inspector valuefield[control=volume]", "-6");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == -6.0; }));
	CHECK(window.historyPosition() == position + 2);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == 1.5; }));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return window.channel(audio).at("volume") == 0.0; }));

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

TEST_CASE(
	"the inspector's dragged volume stays while it shows a project from before the drag's last step again", "[ui]")
{
	test::CommandHolds holds; // declared before the window: its engine's command thread waits in hold()
	Window window;
	const test::ReleaseHolds releaseHolds{holds};
	window.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(window.app.engine(), "holder");
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t other = window.addTrack({{"type", "audio"}, {"name", "Bass"}});
	window.call("channel.set", {{"channel", window.channel(other).at("id")}, {"volume", 4.0}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 2; }));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	const Json channel = window.channel(audio).at("id");
	// The volume in the project that the event display, and with it the inspector, shows.
	const auto shown = [&]
	{
		const Json& channels = window.display.project().at("channels");
		const auto found = std::find_if(
			channels.begin(), channels.end(), [&channel](const Json& each) { return each.at("id") == channel; });
		REQUIRE(found != channels.end());
		return found->at("volume").get<double>();
	};

	// A drag while the engine answers late. Once no step of it is out, its last one goes at once, here behind a hold:
	// the display shows the project from before the last step until the test lets the engine go on (#1305). A step
	// the engine took brings its channel.changed, which the display's project takes before the answer (#1881).
	const std::string field = ".inspector valuefield[control=volume]";
	ui::EngineBinding& binding = window.app.appUi().binding();
	test::LateDrag drag(window.app, std::nullopt, field);
	drag.move(15, 1);
	REQUIRE(window.app.waitUntil([&] { return binding.waiting("history.transaction") == 0; }));
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	drag.nudge(1);
	const double dragged = drag.value();
	drag.release(false);
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(binding.waiting("history.transaction") != 0);
	REQUIRE(shown() != Catch::Approx(dragged).margin(1e-5));
	CHECK(drag.value() == Catch::Approx(dragged).margin(1e-5));

	// Another audio track shows its own volume in the same field, and back on the dragged one that older project shows
	// again: the field shows where the drag left it, not the other track's volume (#1421).
	window.list.select({other});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bass"));
	CHECK(window.shows(field, "4"));
	window.list.select({audio});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	CHECK(drag.value() == Catch::Approx(dragged).margin(1e-5));

	// The engine's value once it took the last step agrees with it.
	holds.release(1);
	REQUIRE(window.app.waitUntil([&] { return shown() == Catch::Approx(dragged).margin(1e-5); }));
	CHECK(window.channel(audio).at("volume").get<double>() == Catch::Approx(dragged).margin(1e-5));
	CHECK(drag.value() == Catch::Approx(dragged).margin(1e-5));
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

TEST_CASE(
	"the info line's volume turned on one event, then on the next with the mouse still over it: two undo steps", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto add = [&](int64_t position)
	{
		return window
			.call("audioEvent.add",
				{{"track", audio}, {"clip", clip.at("id")}, {"position", position}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = add(96000);
	const uint64_t second = add(192000);
	const auto volume = [&window](uint64_t id) { return window.find("audioEvents", id).at("volume").get<double>(); };
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 2; }));
	const std::string field = "#info-line valuefield[field=volume]";
	const auto wheel = [&] { window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F); };
	window.display.select({first});
	REQUIRE(window.shows(field, "0"));
	const int64_t position = window.historyPosition();

	// One wheel gesture: the mouse stays over the field while the next event is selected.
	wheel();
	REQUIRE(window.app.waitUntil([&] { return volume(first) != 0.0; }));
	const double turned = volume(first);
	window.display.select({second});
	REQUIRE(window.shows(field, "0")); // the second one's, not the turned first one's
	wheel();
	REQUIRE(window.app.waitUntil([&] { return volume(second) != 0.0; }));

	// Each event's change is an undo step of its own.
	CHECK(window.historyPosition() == position + 2);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return volume(second) == 0.0; }));
	CHECK(volume(first) == turned);
}

TEST_CASE("the info line shows another event's own volume while a dragged one's last step is out, and back on the "
		  "dragged one, alone or with the other, the drag's",
	"[ui]")
{
	test::CommandHolds holds; // declared before the window: its engine's command thread waits in hold()
	Window window;
	const test::ReleaseHolds releaseHolds{holds};
	// Without the inspector: it reads the in-place editors (editor.inPlace, not a query) for each history.changed,
	// which sends a waiting step at once, before the display's read for the step before.
	window.app.appUi().docking().setTabHidden("inspector", true);
	window.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(window.app.engine(), "holder");
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto addEvent = [&](int64_t at)
	{
		return window
			.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", at}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t dragged = addEvent(96000);
	const uint64_t other = addEvent(192000);
	window.call("audioEvent.set", {{"event", other}, {"volume", 3.0}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 2; }));
	window.display.select({dragged});
	const std::string field = "#info-line valuefield[field=volume]";
	REQUIRE(window.shows(field, "0"));
	ui::EngineBinding& binding = window.app.appUi().binding();
	// The dragged event's volume in the project that the event display, and with it the info line, shows.
	const auto shown = [&]
	{
		const Json& events = window.display.project().at("audioEvents");
		const auto found = std::find_if(
			events.begin(), events.end(), [dragged](const Json& each) { return each.at("id") == dragged; });
		REQUIRE(found != events.end());
		return found->at("volume").get<double>();
	};

	// A drag while the engine answers late, and two steps while the test holds the engine: the first goes behind the
	// hold, the last waits in the binding.
	test::LateDrag drag(window.app, std::nullopt, field);
	drag.move(15, 1);
	// Once the reads after it, and what they send, are answered: no other command goes out while the steps wait.
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return binding.waiting("history.transaction") == 0 && binding.waiting("project.get") == 0 &&
				binding.waiting("editor.inPlace") == 0;
		}));
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	drag.nudge(1);
	drag.nudge(1);
	const double value = drag.value();
	drag.release(false);
	REQUIRE(binding.waiting("history.transaction") == 1);
	// The other event, asked for (event.get), a query, so before the last step. A second hold after that question
	// keeps the project read that the display sends for the first step's history.changed out, and the last step
	// goes behind it.
	window.display.select({other});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(binding.waiting("event.get") != 0);
	holder.send("test.hold");
	holds.release(1);
	REQUIRE(holds.waitEntered(2));
	CHECK(window.shows(field, "3")); // its own, not the dragged event's
	REQUIRE(binding.waiting("project.get") != 0);

	// Back on the dragged event, alone or with the other one, asked for after the last step. A third hold keeps the
	// project read after the last step out: the display shows the read after the first one, and the field where the
	// drag left it, not the other event's volume (#1421).
	const auto heldBehindLastStep = [&](std::vector<uint64_t> events)
	{
		window.display.select(std::move(events));
		window.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(binding.waiting("event.get") != 0);
		holder.send("test.hold");
		holds.release(2);
		REQUIRE(holds.waitEntered(3));
		CHECK(window.app.waitUntil([&] { return drag.value() == Catch::Approx(value).margin(1e-5); }));
		REQUIRE(binding.waiting("history.transaction") == 0);
		REQUIRE(binding.waiting("project.get") != 0);
		REQUIRE(shown() != Catch::Approx(value).margin(1e-5));
	};
	const auto volume = [&](uint64_t event) { return window.find("audioEvents", event).at("volume").get<double>(); };

	SECTION("alone")
	{
		heldBehindLastStep({dragged});

		// The read after the last step agrees with it, and so does the engine.
		holds.release(3);
		REQUIRE(window.app.waitUntil([&] { return shown() == Catch::Approx(value).margin(1e-5); }));
		CHECK(drag.value() == Catch::Approx(value).margin(1e-5));
		CHECK(volume(dragged) == Catch::Approx(value).margin(1e-5));
		CHECK(volume(other) == 3.0);
	}

	SECTION("with the other event, one wheel turn moves each by the turn's step from what it shows (#1632)")
	{
		heldBehindLastStep({dragged, other});
		window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F);
		const double turn = drag.value() - value;
		REQUIRE(turn != Catch::Approx(0.0).margin(1e-5));

		// From where the drag left the dragged event, not from the project's older value: the other event moves by
		// the turn alone, not also by what the project still lacks of the dragged event's volume.
		holds.release(3);
		REQUIRE(window.app.waitUntil([&] { return volume(dragged) == Catch::Approx(value + turn).margin(1e-4); }));
		CHECK(window.app.waitUntil([&] { return volume(other) == Catch::Approx(3.0 + turn).margin(1e-4); }));
	}
}

TEST_CASE("the info line shows another part's own transpose while a dragged selection's last step is out, and one "
		  "dragged part, alone or with the other, the drag's",
	"[ui]")
{
	test::CommandHolds holds; // declared before the window: its engine's command thread waits in hold()
	Window window;
	const test::ReleaseHolds releaseHolds{holds};
	// Without the inspector: it reads the in-place editors (editor.inPlace, not a query) for each history.changed,
	// which sends a waiting step at once, before the display's read for the step before.
	window.app.appUi().docking().setTabHidden("inspector", true);
	window.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(window.app.engine(), "holder");
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	const uint64_t first = window.addPart(keys, 0, 4 * g_quarter, "First");
	const uint64_t second = window.addPart(keys, 4 * g_quarter, 4 * g_quarter, "Second");
	const uint64_t other = window.addPart(keys, 8 * g_quarter, 4 * g_quarter, "Other");
	window.call("midiPart.set", {{"part", other}, {"transpose", 5}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 3; }));
	window.display.select({first, second});
	const std::string field = "#info-line valuefield[field=transpose]";
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "First"));
	REQUIRE(window.shows(field, "0"));
	ui::EngineBinding& binding = window.app.appUi().binding();
	// The first part's transpose in the project that the event display, and with it the info line, shows.
	const auto shown = [&]
	{
		const Json& parts = window.display.project().at("midiParts");
		const auto found =
			std::find_if(parts.begin(), parts.end(), [first](const Json& each) { return each.at("id") == first; });
		REQUIRE(found != parts.end());
		return found->value("transpose", 0);
	};

	// A drag of both parts' transpose while the engine answers late, and two steps while the test holds the engine:
	// the first goes behind the hold, the last waits in the binding.
	test::LateDrag drag(window.app, std::nullopt, field);
	drag.move(5, 1);
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return binding.waiting("history.transaction") == 0 && binding.waiting("project.get") == 0 &&
				binding.waiting("editor.inPlace") == 0;
		}));
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	drag.nudge(1);
	drag.nudge(1);
	const auto value = static_cast<double>(std::lround(drag.value())); // the field shows the whole semitones sent
	drag.release(false);
	REQUIRE(binding.waiting("history.transaction") == 1);
	// The other part, asked for (event.get), a query, so before the last step. A second hold after that question
	// keeps the project read that the display sends for the first step's history.changed out, and the last step
	// goes behind it.
	window.display.select({other});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(binding.waiting("event.get") != 0);
	holder.send("test.hold");
	holds.release(1);
	REQUIRE(holds.waitEntered(2));
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Other"));
	CHECK(window.shows(field, "5")); // its own, not the dragged parts'
	REQUIRE(binding.waiting("project.get") != 0);

	// The first dragged part, asked for after the last step, alone or with the other one. A third hold keeps the
	// project read after the last step out: the display shows the read after the first one, and the field where the
	// drag left the part, though the drag changed it with another part selected (#1471).
	const auto heldBehindLastStep = [&](std::vector<uint64_t> parts)
	{
		window.display.select(std::move(parts));
		window.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(binding.waiting("event.get") != 0);
		holder.send("test.hold");
		holds.release(2);
		REQUIRE(holds.waitEntered(3));
		REQUIRE(window.shows("#info-line textdisplay[field=name]", "First"));
		CHECK(window.app.waitUntil([&] { return drag.value() == value; }));
		REQUIRE(binding.waiting("history.transaction") == 0);
		REQUIRE(binding.waiting("project.get") != 0);
		REQUIRE(shown() != value);
	};

	SECTION("alone")
	{
		heldBehindLastStep({first});

		// The read after the last step agrees with it, and so does the engine, for both dragged parts.
		holds.release(3);
		REQUIRE(window.app.waitUntil([&] { return shown() == value; }));
		CHECK(drag.value() == value);
		CHECK(window.part(first).value("transpose", 0) == value);
		CHECK(window.part(second).value("transpose", 0) == value);
		CHECK(window.part(other).value("transpose", 0) == 5);
	}

	SECTION("with the other part, one wheel turn moves each by one from what it shows (#1632)")
	{
		heldBehindLastStep({first, other});
		window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F);
		const double turn = static_cast<double>(std::lround(drag.value())) - value;
		REQUIRE(std::abs(turn) == 1.0);

		// From where the drag left the first part, not from the project's older value.
		holds.release(3);
		REQUIRE(window.app.waitUntil([&] { return window.part(first).value("transpose", 0) == value + turn; }));
		CHECK(window.app.waitUntil([&] { return window.part(other).value("transpose", 0) == 5 + turn; }));
		CHECK(window.part(second).value("transpose", 0) == value);
	}
}

TEST_CASE("the info line shows another note's own velocity while a dragged note's last step is out, and back on the "
		  "dragged one, alone or with the other, the drag's",
	"[ui]")
{
	test::CommandHolds holds; // declared before the window: its engine's command thread waits in hold()
	Window window;
	const test::ReleaseHolds releaseHolds{holds};
	// Without the inspector: it reads the in-place editors (editor.inPlace, not a query) for each history.changed,
	// which sends a waiting step at once, before the display's read for the step before.
	window.app.appUi().docking().setTabHidden("inspector", true);
	window.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(window.app.engine(), "holder");
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Keys");
	const Json added = window.call("note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 100.0 / 127.0}}}}});
	const uint64_t dragged = added.at("notes").at(0).at("id").get<uint64_t>();
	const uint64_t other = added.at("notes").at(1).at("id").get<uint64_t>();
	const std::string editor = "inPlace:" + std::to_string(midi);
	window.call("editor.inPlace", {{"tracks", {midi}}, {"open", true}});
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("notes").size() == 2; }));
	const auto selection = [&](uint64_t note) { return Json::array({{{"part", part}, {"note", note}}}); };
	window.call("editor.set", {{"editor", editor}, {"selection", selection(dragged)}});
	const std::string field = "#info-line valuefield[field=velocity]";
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "C3"));
	REQUIRE(window.shows(field, "64"));
	ui::EngineBinding& binding = window.app.appUi().binding();
	// The dragged note's velocity, 0-127, in the project that the event display, and with it the info line, shows.
	const auto shown = [&]
	{
		for (const Json& each : window.display.project().at("midiParts"))
			if (each.at("id") == part)
				for (const Json& note : each.at("notes"))
					if (note.at("id") == dragged)
						return static_cast<double>(std::lround(note.at("velocity").get<double>() * 127.0));
		return -1.0; // gone
	};

	// A drag while the engine answers late, and two steps while the test holds the engine: the first goes behind the
	// hold, the last waits in the binding.
	test::LateDrag drag(window.app, std::nullopt, field);
	drag.move(5, 1);
	REQUIRE(window.app.waitUntil(
		[&]
		{
			return binding.waiting("history.transaction") == 0 && binding.waiting("project.get") == 0 &&
				binding.waiting("editor.inPlace") == 0;
		}));
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	drag.nudge(1);
	drag.nudge(1);
	const auto value = static_cast<double>(std::lround(drag.value())); // the field shows whole velocities sent
	drag.release(false);
	REQUIRE(binding.waiting("history.transaction") == 1);
	// The other note, selected behind the first step. A second hold after it keeps the project read that the display
	// sends for the first step's history.changed out, and the last step goes behind it.
	holder.send("editor.set", {{"editor", editor}, {"selection", selection(other)}});
	holder.send("test.hold");
	holds.release(1);
	REQUIRE(holds.waitEntered(2));
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "E3"));
	CHECK(window.shows(field, "100")); // its own, not the dragged note's (#1550)
	REQUIRE(binding.waiting("project.get") != 0);

	// Back on the dragged note, alone or with the other one, after the last step. A third hold keeps the project
	// read after the last step out: the display shows the read after the first one, and the field where the drag
	// left the note.
	const auto heldBehindLastStep = [&](Json notes)
	{
		holder.send("editor.set", {{"editor", editor}, {"selection", std::move(notes)}});
		holder.send("test.hold");
		holds.release(2);
		REQUIRE(holds.waitEntered(3));
		REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "C3"));
		CHECK(window.app.waitUntil([&] { return drag.value() == value; }));
		REQUIRE(binding.waiting("history.transaction") == 0);
		REQUIRE(binding.waiting("project.get") != 0);
		REQUIRE(shown() != value);
	};
	// Each note's velocity, 0-127, in the engine.
	const auto velocities = [&]
	{
		std::map<uint64_t, double> found;
		const Json notes = window.part(part); // the temporary must outlive the loop over its notes
		for (const Json& note : notes.at("notes"))
			found[note.at("id").get<uint64_t>()] =
				static_cast<double>(std::lround(note.at("velocity").get<double>() * 127.0));
		return found;
	};

	SECTION("alone")
	{
		heldBehindLastStep(selection(dragged));

		// The read after the last step agrees with it, and so does the engine; the other note kept its own.
		holds.release(3);
		REQUIRE(window.app.waitUntil([&] { return shown() == value; }));
		CHECK(drag.value() == value);
		CHECK(window.shows(field, std::to_string(std::lround(value))));
		CHECK(velocities() == std::map<uint64_t, double>{{dragged, value}, {other, 100.0}});
	}

	SECTION("with the other note, one wheel turn moves each by one from what it shows (#1632)")
	{
		Json both = selection(dragged);
		both.push_back({{"part", part}, {"note", other}});
		heldBehindLastStep(std::move(both));
		window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F);
		const double turn = static_cast<double>(std::lround(drag.value())) - value;
		REQUIRE(std::abs(turn) == 1.0);

		// From where the drag left the dragged note, not from the project's older value.
		holds.release(3);
		CHECK(window.app.waitUntil([&]
			{ return velocities() == std::map<uint64_t, double>{{dragged, value + turn}, {other, 100.0 + turn}}; }));
	}
}

TEST_CASE("the info line's note velocity turned on one note, then on the next with the mouse still over it: two "
		  "undo steps",
	"[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 0, 4 * g_quarter, "Keys");
	const Json added = window.call("note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}}}});
	const uint64_t first = added.at("notes").at(0).at("id").get<uint64_t>();
	const uint64_t second = added.at("notes").at(1).at("id").get<uint64_t>();
	const std::string editor = "inPlace:" + std::to_string(midi);
	window.call("editor.inPlace", {{"tracks", {midi}}, {"open", true}});
	const auto velocity = [&](uint64_t id)
	{
		const Json shown = window.part(part); // the temporary must outlive the loop over its notes
		for (const Json& note : shown.at("notes"))
			if (note.at("id") == id)
				return note.at("velocity").get<double>();
		return -1.0; // gone
	};
	const auto select = [&](uint64_t note)
	{
		window.call("editor.set", {{"editor", editor}, {"selection", Json::array({{{"part", part}, {"note", note}}})}});
	};
	const std::string field = "#info-line valuefield[field=velocity]";
	const auto wheel = [&] { window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F); };
	select(first);
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "C3"));
	REQUIRE(window.shows(field, "64"));
	const int64_t position = window.historyPosition();
	const double untouched = velocity(second); // the engine keeps velocities in steps of 1/65535

	// One wheel gesture: the mouse stays over the field while the next note is selected.
	wheel();
	REQUIRE(window.app.waitUntil([&] { return velocity(first) != untouched; }));
	const double turned = velocity(first);
	select(second);
	REQUIRE(window.shows("#info-line textdisplay[field=pitch]", "E3"));
	REQUIRE(window.shows(field, "64")); // the second one's, not the turned first one's
	wheel();
	REQUIRE(window.app.waitUntil([&] { return velocity(second) != untouched; }));

	// Each note's change is an undo step of its own (#1550).
	CHECK(window.historyPosition() == position + 2);
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return velocity(second) == untouched; }));
	CHECK(velocity(first) == turned);
}

TEST_CASE("the info line's refused volume step shows the engine's value again", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto addEvent = [&](int64_t at)
	{
		return window
			.call("audioEvent.add", {{"track", audio}, {"clip", clip.at("id")}, {"position", at}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = addEvent(96000);
	const uint64_t second = addEvent(192000);
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 2; }));
	window.display.select({first, second});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "take"));
	const std::string field = "#info-line valuefield[field=volume]";
	REQUIRE(window.shows(field, "0"));
	const int64_t position = window.historyPosition();

	// Two wheel turns: the first step goes, the second waits for its answer. Removing one of the events in between
	// makes the engine refuse the second step: it changes an event that is gone.
	window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F);
	window.app.ui().wheel(std::nullopt, {field, 0, std::nullopt}, 0.0F, 1.0F);
	window.call("audioEvent.remove", {{"event", second}});
	REQUIRE(window.app.waitUntil(
		[&]
		{
			const Json commands = window.call("engine.commandLog").at("commands");
			return std::any_of(commands.begin(), commands.end(), [](const Json& command)
				{ return command.at("command") == "history.transaction" && command.contains("error"); });
		}));
	// The refused step shows the events as they are again (m_loads = 0): the field shows the engine's volume, what
	// the first turn took the event that is left to, not the refused second turn's.
	CHECK(window.shows(field, "-0.5"));
	CHECK(window.find("audioEvents", first).at("volume") == -0.5);
	CHECK(window.historyPosition() == position + 2); // the gesture's step and the removal; the refused one left none
}

TEST_CASE("the inspector's output list leaves out the channels whose audio would come back", "[ui]")
{
	Window window;
	const uint64_t drums = window.addTrack({{"type", "group"}, {"name", "Drums"}});
	const uint64_t bus = window.addTrack({{"type", "group"}, {"name", "Bus"}});
	const uint64_t reverb = window.addTrack({{"type", "effect"}, {"name", "Reverb"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 3; }));
	const auto id = [&](uint64_t track) { return std::to_string(window.channel(track).at("id").get<uint64_t>()); };
	// The values of the output list's options.
	const auto options = [&]
	{
		std::vector<std::string> values;
		Rml::Context& context = window.app.main().context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(
					context.GetDocument(i)->QuerySelector(".inspector select[control=output]")))
				for (int option = 0; option < select->GetNumOptions(); ++option)
					values.push_back(select->GetOption(option)->GetAttribute<Rml::String>("value", ""));
		return values;
	};
	const auto lists = [&](const std::string& value)
	{
		const std::vector<std::string> values = options();
		return std::find(values.begin(), values.end(), value) != values.end();
	};

	// Drums play into Bus and send to Reverb, so neither Bus nor Reverb can play into Drums (the engine refuses
	// it); Reverb can still play into Bus.
	window.call("channel.set",
		{{"channel", window.channel(drums).at("id")}, {"output", window.channel(bus).at("id")},
			{"sends", {{{"send", 1}, {"destination", window.channel(reverb).at("id")}}}}});
	window.list.select({bus});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bus"));
	CHECK(window.app.waitUntil([&] { return !lists(id(drums)) && lists(id(reverb)); }));
	window.list.select({reverb});
	REQUIRE(window.app.waitUntil([&] { return lists(id(bus)); }));
	CHECK(!lists(id(drums)));
	window.list.select({drums});
	REQUIRE(window.app.waitUntil([&] { return lists(id(reverb)); }));
	CHECK(lists(id(bus)));
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

TEST_CASE("another project drops the info line's edit in progress", "[ui]")
{
	Window window;
	const test::TempDir folder;
	const std::string file = (folder.path() / "song.dawproject").string();
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t part = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 1; }));
	window.call("project.save", {{"path", file}});
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));

	// A name typed, not yet entered, when another client loads the project, whose part has the same id: the name is
	// dropped, not given to the loaded project's part when the field is left.
	window.click("#info-line textdisplay[field=name]", ui::MouseButton::Left, 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "Verse");
	window.app.shell().step(std::chrono::milliseconds(0));
	const uint64_t loads = window.display.loads();
	window.call("project.load", {{"path", file}});
	REQUIRE(window.app.waitUntil([&] { return window.display.loads() > loads; }));
	window.display.select({part});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	window.click("#info-line textdisplay[field=start]");
	CHECK_FALSE(
		window.app.waitUntil([&] { return window.part(part).at("name") != "Intro"; }, std::chrono::milliseconds(300)));
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
	// The info line edits from the events it last showed. Typed while it still has the clamped length, the same length
	// would change nothing, so the next edit waits until it has shown the clamped length and then the undone one.
	const auto undo = [&]
	{
		REQUIRE(window.app.waitUntil(
			[&] { return attribute(window.app, "#info-line textdisplay[field=length]", "value") != "0:00:01.000"; }));
		window.call("history.undo");
		REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == 48000; }));
		REQUIRE(window.shows("#info-line textdisplay[field=length]", "0:00:01.000"));
	};

	window.type("#info-line textdisplay[field=end]", "999999999999999");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == time::g_maxPosition - 48000; }));
	undo();
	window.type("#info-line textdisplay[field=length]", "999999999999999");
	REQUIRE(window.app.waitUntil([&] { return window.part(part).at("length") == time::g_maxPosition - 48000; }));
	undo();
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

	// Changes from another client show; markers have no transpose, audio events no velocity (#1555).
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
	CHECK_FALSE(hasClass(window.app, "#info-line [info-field=transpose]", "unavailable"));
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

TEST_CASE("Ctrl+Enter on the first part's untouched Transpose or Velocity takes its value for all", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t first = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	const uint64_t second = window.addPart(midi, 12 * g_quarter, 8 * g_quarter, "Verse");
	window.call("midiPart.set", {{"part", first}, {"transpose", 3}, {"velocity", -8}});
	window.call("midiPart.set", {{"part", second}, {"transpose", -5}, {"velocity", 20}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));
	window.display.select({second, first});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	for (const auto& each : std::vector<std::pair<std::string, int>>{{"transpose", 3}, {"velocity", -8}})
	{
		const std::string& field = each.first;
		const int value = each.second;
		INFO(field);
		const std::string selector = "#info-line valuefield[field=" + field + "]";
		REQUIRE(window.shows(selector, std::to_string(value)));
		window.click(selector, ui::MouseButton::Left, 2);
		window.app.ui().key(std::nullopt, "Ctrl+Enter");
		REQUIRE(window.app.waitUntil([&] { return window.part(second).at(field) == value; }));
		CHECK(window.part(first).at(field) == value);
	}
}

TEST_CASE("a part held at its limit by a relative drag of the info line's Transpose ends where it started", "[ui]")
{
	Window window;
	const uint64_t midi = window.addTrack({{"type", "midi"}});
	const uint64_t first = window.addPart(midi, 4 * g_quarter, 4 * g_quarter, "Intro");
	const uint64_t second = window.addPart(midi, 12 * g_quarter, 8 * g_quarter, "Verse");
	window.call("midiPart.set", {{"part", second}, {"transpose", 120}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("midiParts").size() == 2; }));
	window.display.select({first, second});
	REQUIRE(window.shows("#info-line textdisplay[field=name]", "Intro"));
	const std::string field = "#info-line valuefield[field=transpose]";
	REQUIRE(window.shows(field, "0"));

	// Up until the second part is held at 127, the project arriving after each step, and back down to the start.
	test::LateDrag drag(window.app, std::nullopt, field);
	int steps = 0;
	while (drag.value() < 10.0 && steps < 200)
	{
		drag.move(1, -1);
		++steps;
	}
	REQUIRE(drag.value() >= 10.0);
	REQUIRE(window.app.waitUntil([&] { return window.part(second).at("transpose") == 127; }));
	drag.move(steps, 1);
	drag.release();
	REQUIRE(window.app.waitUntil([&] { return window.part(first).at("transpose") == 0; }));
	CHECK(window.part(second).at("transpose") == 120);
}

TEST_CASE("an audio event held at 6 dB by a relative drag of the info line's volume ends where it started", "[ui]")
{
	Window window;
	const uint64_t audio = window.addTrack({{"type", "audio"}, {"timeBase", "linear"}});
	const Json media =
		window.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto add = [&](int64_t position)
	{
		return window
			.call("audioEvent.add",
				{{"track", audio}, {"clip", clip.at("id")}, {"position", position}, {"length", 24000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = add(0);
	const uint64_t second = add(48000);
	window.call("audioEvent.set", {{"event", second}, {"volume", 5.0}});
	const auto volume = [&](uint64_t id) { return window.find("audioEvents", id).at("volume").get<double>(); };
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("audioEvents").size() == 2; }));
	window.display.select({first, second});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, "#info-line", "multiple"); }));
	const std::string field = "#info-line valuefield[field=volume]";
	REQUIRE(window.app.waitUntil(
		[&]
		{
			const std::string shown = attribute(window.app, field, "value");
			return !shown.empty() && std::stod(shown) == 0.0;
		}));

	// Up until the second event is held at 6 dB, the project arriving after each step, and back down to the start.
	test::LateDrag drag(window.app, std::nullopt, field);
	int steps = 0;
	while (drag.value() < 2.0 && steps < 200)
	{
		drag.move(1, -1);
		++steps;
	}
	REQUIRE(drag.value() >= 2.0);
	REQUIRE(window.app.waitUntil([&] { return volume(second) == 6.0; }));
	drag.move(steps, 1);
	drag.release();
	REQUIRE(window.app.waitUntil([&] { return volume(first) == 0.0; }));
	CHECK(volume(second) == 5.0);
}

TEST_CASE("the inspector's random generators keep the values they show when their target changes unit", "[ui]")
{
	Window window;
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 1; }));
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	for (const char* section : {"basics", "routing"})
		window.click(".inspector [section=" + std::string(section) + "] .section-header");
	const auto random = [&] { return window.channel(keys).at("modifiers").at("random")[1]; };
	const std::string target = ".inspector select[control=modifiers-random-1-target]";
	const std::string minimum = ".inspector valuefield[control=modifiers-random-1-minimum]";
	const std::string maximum = ".inspector valuefield[control=modifiers-random-1-maximum]";
	// Moves the target's choice by steps in its list: off, position, pitch, velocity, length.
	const auto choose = [&](const char* key, int steps)
	{
		window.click(target);
		for (int i = 0; i < steps; ++i)
			window.app.ui().key(std::nullopt, key);
		window.click(target);
	};
	constexpr int64_t perShown = time::g_ticksPerQuarter / 480;
	window.call("channel.set",
		{{"channel", window.channel(keys).at("id")},
			{"modifiers", {{"random", {Json::object(), {{"target", "pitch"}, {"minimum", -12}, {"maximum", 12}}}}}}});
	REQUIRE(window.shows(minimum, "-12"));

	// Pitch to Position: the same numbers in ticks of 480 to a quarter note, with the limits of ticks, as one undo
	// step.
	const int64_t position = window.historyPosition();
	choose("Up", 1);
	REQUIRE(window.app.waitUntil([&] { return random().at("target") == "position"; }));
	CHECK(random() == Json{{"target", "position"}, {"minimum", -12 * perShown}, {"maximum", 12 * perShown}});
	CHECK(window.historyPosition() == position + 1);
	CHECK(window.shows(minimum, "-12"));
	CHECK(window.shows(maximum, "12"));
	REQUIRE(window.app.waitUntil([&] { return attribute(window.app, maximum, "max") == "480"; }));

	// Length is in ticks too: the values stay; its maximum goes up to a quarter note.
	choose("Down", 3);
	REQUIRE(window.app.waitUntil([&] { return random().at("target") == "length"; }));
	CHECK(random().at("minimum") == -12 * perShown);
	window.type(maximum, "480");
	REQUIRE(window.app.waitUntil([&] { return random().at("maximum") == engine::midi::g_maxRandomTicks; }));

	// Back to Velocity: the numbers shown, kept within -127 to 127, which are its limits again.
	choose("Up", 1);
	REQUIRE(window.app.waitUntil([&] { return random().at("target") == "velocity"; }));
	CHECK(random() == Json{{"target", "velocity"}, {"minimum", -12}, {"maximum", 127}});
	CHECK(window.shows(minimum, "-12"));
	CHECK(window.shows(maximum, "127"));
	REQUIRE(window.app.waitUntil([&] { return attribute(window.app, minimum, "min") == "-127"; }));
	CHECK(attribute(window.app, maximum, "max") == "127");
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

TEST_CASE("the inspector routes a MIDI or instrument track's MIDI input from all MIDI inputs or the chord pads", "[ui]")
{
	// Reference manual p. 1367: Chord Pads as the input routing (#1089).
	Window window;
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 1; }));
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK_FALSE(hasClass(window.app, ".inspector [row=input]", "unavailable"));
	REQUIRE(window.shows(".inspector select[control=input]", "all"));
	window.click(".inspector select[control=input]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=input]");
	REQUIRE(window.app.waitUntil([&] { return window.channel(keys).at("midiInput") == "chordPads"; }));
	// Another client's choice shows.
	window.call("channel.set", {{"channel", window.channel(keys).at("id")}, {"midiInput", "none"}});
	CHECK(window.shows(".inspector select[control=input]", "none"));
}

TEST_CASE("the inspector's MIDI Inserts and MIDI Sends sections show and change the track's MIDI effects", "[ui]")
{
	Window window;
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	const uint64_t bass = window.addTrack({{"type", "instrument"}, {"name", "Bass"}});
	const uint64_t vox = window.addTrack({{"type", "audio"}, {"name", "Vox"}});
	const uint64_t marker = window.addTrack({{"type", "marker"}});
	REQUIRE(window.app.waitUntil([&] { return window.display.project().at("tracks").size() == 4; }));
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK_FALSE(hasClass(window.app, ".inspector [section=midiInserts]", "unavailable"));
	CHECK_FALSE(hasClass(window.app, ".inspector [section=midiSends]", "unavailable"));
	// The sections above them closed, so that their controls are in view.
	for (const char* section : {"basics", "routing", "modifiers"})
		window.click(".inspector [section=" + std::string(section) + "] .section-header");
	const auto inserts = [&] { return window.channel(keys).at("midiInserts"); };
	const auto sends = [&] { return window.channel(keys).at("midiSends"); };
	const auto choose = [&](const std::string& select, int downs)
	{
		window.click(select);
		for (int i = 0; i < downs; ++i)
			window.app.ui().key(std::nullopt, "Down");
		window.click(select);
	};

	// An effect chosen is on and its control panel opens (p. 1105): the chord generator, after No Effect and the
	// arpeggiator.
	choose(".inspector select[control=midiInserts-0-effect]", 2);
	REQUIRE(window.app.waitUntil([&] { return inserts().at("slots")[0].at("effect") == "chordGenerator"; }));
	const std::string first = ".inspector [section=midiInserts] .midi-effect-slot[slot=0]";
	CHECK(hasClass(window.app, first, "open"));
	REQUIRE(window.app.waitUntil(
		[&] { return !elements(window.app, ".inspector select[control=midiInserts-0-parameter-chord]").empty(); }));
	choose(".inspector select[control=midiInserts-0-parameter-chord]", 1);
	REQUIRE(window.app.waitUntil([&] { return inserts().at("slots")[0].at("parameters").at("chord") == 1; }));
	window.click(".inspector button[control=midiInserts-0-on]");
	REQUIRE(window.app.waitUntil([&] { return inserts().at("slots")[0].at("on") == false; }));
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, first, "off"); }));
	window.click(".inspector button[control=midiInserts-bypass]");
	REQUIRE(window.app.waitUntil([&] { return inserts().at("bypass") == true; }));
	// The divider below the first slot: that insert is recorded.
	choose(".inspector select[control=midiInserts-divider]", 1);
	REQUIRE(window.app.waitUntil([&] { return inserts().at("divider") == 1; }));
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, first, "recorded"); }));
	// Dragged below the third slot, the divider goes there in one step (p. 1105).
	const int64_t beforeDrag = window.historyPosition();
	CHECK(hasClass(window.app, ".inspector .midi-divider[gap=1]", "at"));
	window.app.ui().drag(std::nullopt, {".inspector .midi-divider[gap=1]", 0, std::nullopt},
		{".inspector .midi-divider[gap=3]", 0, std::nullopt});
	REQUIRE(window.app.waitUntil([&] { return inserts().at("divider") == 3; }));
	CHECK(window.historyPosition() == beforeDrag + 1);
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, ".inspector .midi-divider[gap=3]", "at"); }));
	CHECK_FALSE(hasClass(window.app, ".inspector .midi-divider[gap=1]", "at"));
	REQUIRE(window.app.waitUntil([&]
		{ return hasClass(window.app, ".inspector [section=midiInserts] .midi-effect-slot[slot=2]", "recorded"); }));
	// When the inspector shows another track during a drag, the drag ends without moving a divider, neither the
	// dragged track's nor the one shown at its end.
	const ui::Target gap2 = {".inspector .midi-divider[gap=2]", 0, std::nullopt};
	window.app.ui().press(std::nullopt, {".inspector .midi-divider[gap=3]", 0, std::nullopt}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, gap2);
	window.list.select({bass});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Bass"));
	window.app.ui().release(std::nullopt, gap2, ui::MouseButton::Left);
	CHECK(hasClass(window.app, ".inspector .midi-divider[gap=0]", "at"));
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK(window.historyPosition() == beforeDrag + 1);
	CHECK(window.channel(bass).at("midiInserts").at("divider") == 0);
	CHECK(inserts().at("divider") == 3);
	CHECK(hasClass(window.app, ".inspector .midi-divider[gap=3]", "at"));
	// Nor when it shows a track without a channel, a marker track, released where the mouse is.
	window.app.ui().press(std::nullopt, {".inspector .midi-divider[gap=3]", 0, std::nullopt}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, gap2);
	window.list.select({marker});
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, ".inspector [section=basics]", "unavailable"); }));
	window.app.ui().release(std::nullopt, {}, ui::MouseButton::Left);
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK(window.historyPosition() == beforeDrag + 1);
	CHECK(inserts().at("divider") == 3);
	CHECK(hasClass(window.app, ".inspector .midi-divider[gap=3]", "at"));
	// The edit button closes the control panel.
	window.click(first + " .midi-effect-edit");
	CHECK_FALSE(hasClass(window.app, first, "open"));
	window.click(".inspector [section=midiInserts] .section-header");

	// A send: the arpeggiator with its gate, to the other instrument track, before the modifiers and inserts.
	choose(".inspector select[control=midiSends-0-effect]", 1);
	REQUIRE(window.app.waitUntil([&] { return sends().at("slots")[0].at("effect") == "arpeggiator"; }));
	REQUIRE(window.app.waitUntil(
		[&] { return !elements(window.app, ".inspector valuefield[control=midiSends-0-parameter-gate]").empty(); }));
	window.type(".inspector valuefield[control=midiSends-0-parameter-gate]", "75");
	REQUIRE(window.app.waitUntil([&] { return sends().at("slots")[0].at("parameters").at("gate") == 75.0; }));
	choose(".inspector select[control=midiSends-0-output]", 1);
	REQUIRE(window.app.waitUntil([&] { return sends().at("slots")[0].at("output") == window.channel(bass).at("id"); }));
	window.click(".inspector button[control=midiSends-pre]");
	REQUIRE(window.app.waitUntil([&] { return sends().at("pre") == true; }));

	// Changes from another client show, and undo restores them.
	window.call("channel.set",
		{{"channel", window.channel(keys).at("id")}, {"midiSends", {{"slots", {{{"parameters", {{"gate", 20}}}}}}}}});
	CHECK(window.shows(".inspector valuefield[control=midiSends-0-parameter-gate]", "20"));
	window.call("history.undo");
	CHECK(window.shows(".inspector valuefield[control=midiSends-0-parameter-gate]", "75"));

	// Tracks without notes don't have them.
	window.list.select({vox});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Vox"));
	CHECK(hasClass(window.app, ".inspector [section=midiInserts]", "unavailable"));
	CHECK(hasClass(window.app, ".inspector [section=midiSends]", "unavailable"));
}

TEST_CASE("the inspector's Note Expression section shows and changes the track's input assignments, ranges and bypass",
	"[ui]")
{
	auto backend = std::make_unique<midi::DummyBackend>();
	backend->addLoopback("Keys");
	backend->addLoopback("Pads");
	midi::DummyBackend& dummy = *backend;
	std::vector<std::unique_ptr<midi::Backend>> backends;
	backends.push_back(std::move(backend));
	Window window(1200, std::move(backends));
	const uint64_t keys = window.addTrack({{"type", "instrument"}, {"name", "Keys"}});
	const uint64_t voice = window.addTrack({{"type", "audio"}, {"name", "Voice"}});
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK_FALSE(hasClass(window.app, ".inspector [section=noteExpression]", "unavailable"));
	for (const char* section : {"basics", "routing", "modifiers", "midiInserts", "midiSends"})
		window.click(".inspector [section=" + std::string(section) + "] .section-header");
	const auto setup = [&] { return window.channel(keys).at("noteExpression"); };
	const std::string section = ".inspector [section=noteExpression] ";

	window.click(section + "button[control=noteExpression-bypass]");
	REQUIRE(window.app.waitUntil([&] { return setup().at("bypass") == true; }));
	// Tuning selected: its range in Parameter Details, its input from the list and its range typed.
	window.click(section + "[expression=\"2\"]");
	REQUIRE(window.app.waitUntil([&] { return hasClass(window.app, section + "[expression=\"2\"]", "selected"); }));
	CHECK(attribute(window.app, section + "valuefield[control=noteExpression-max]", "max") == "120");
	CHECK(window.shows(section + "valuefield[control=noteExpression-min]", "-120.00"));
	window.click(section + "select[control=noteExpression-input]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(section + "select[control=noteExpression-input]");
	window.type(section + "valuefield[control=noteExpression-max]", "12");
	const Json tuning = {{"type", "tuning"}, {"input", "pitchBend"}, {"min", -120.0}, {"max", 12.0}};
	REQUIRE(window.app.waitUntil([&] { return setup().at("inputs") == Json::array({tuning}); }));
	CHECK(element(window.app, section + "[expression=\"2\"] .expression-in").value("text", "") == "PB");

	// MIDI Learn on brightness takes the next controller the track receives: none from an input that isn't among
	// all MIDI inputs.
	std::string pads;
	const Json ports = window.call("midi.ports");
	for (const Json& port : ports.at("ports"))
		if (port.at("input") == true && port.at("name") == "Pads")
			pads = port.at("id").get<std::string>();
	REQUIRE(!pads.empty());
	window.call("midi.setPort", {{"id", pads}, {"input", true}, {"inAllInputs", false}});
	window.click(section + "[expression=\"5\"]");
	window.click(section + "button[control=noteExpression-learn]");
	const std::unique_ptr<midi::Output> keysOut = dummy.openOutput("Keys");
	const std::unique_ptr<midi::Output> padsOut = dummy.openOutput("Pads");
	// The device sends a controller, and the inspector hears of it (midi.activity, which offline the test runs).
	const auto send = [&](midi::Output& device, uint8_t number)
	{
		audio::MidiMessage message;
		message.size = 3;
		message.bytes = {0xB0, number, 64};
		device.send(message, hostTime() - 1000000000);
		return window.app.waitUntil(
			[&]
			{
				if (window.call("audio.get").at("running").get<bool>())
					return true; // the engine runs midi.activity itself
				window.call("audio.render", {{"frames", 128}});
				return !window.call("midi.activity").at("received").empty();
			});
	};
	REQUIRE(send(*padsOut, 22));
	for (int i = 0; i < 3; ++i)
		window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(setup().at("inputs").size() == 1);
	CHECK(hasClass(window.app, section + "button[control=noteExpression-learn]", "checked"));
	REQUIRE(send(*keysOut, 21));
	REQUIRE(window.app.waitUntil([&] { return setup().at("inputs").size() == 2; }));
	CHECK(setup().at("inputs").at(1) == Json{{"type", "brightness"}, {"input", "cc21"}, {"min", 0.0}, {"max", 1.0}});
	CHECK_FALSE(hasClass(window.app, section + "button[control=noteExpression-learn]", "checked"));

	// Learn ends when the inspector shows another track, so that the next controller goes to none.
	window.click(section + "button[control=noteExpression-learn]");
	REQUIRE(window.app.waitUntil(
		[&] { return hasClass(window.app, section + "button[control=noteExpression-learn]", "checked"); }));
	window.list.select({voice});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Voice"));
	window.list.select({keys});
	REQUIRE(window.shows(".inspector textdisplay.inspector-name", "Keys"));
	CHECK_FALSE(hasClass(window.app, section + "button[control=noteExpression-learn]", "checked"));
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return setup().at("inputs") == Json::array({tuning}); }));
}
