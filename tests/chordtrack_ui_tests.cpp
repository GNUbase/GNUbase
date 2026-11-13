// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track in the project window (#173, #933, docs/features/chord-track.md), driven with ui::Automation as
// MCP's UI tools drive it: Add Track's Chord page, the chord events on the chord track's lane, drawn, typed, dragged
// and erased, the scale lane, the track list's chord track controls, the Inspector's chord settings and the Chord
// Track menu's dialogs.
#include "testutils/testutils.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	// The project window at a dp ratio of 0.5.
	struct Window
	{
		Window() : display(app.appUi().showProject())
		{
			app.setDpRatio(0.5F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		Json element(const std::string& selector)
		{
			const Json found = elements(selector);
			REQUIRE(found.size() == 1);
			return found[0];
		}

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json classes = element(selector).value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void type(const std::string& text)
		{
			app.ui().key(std::nullopt, "Mod+A");
			app.ui().text(std::nullopt, text);
			app.ui().key(std::nullopt, "Enter");
		}

		Json events() { return call("chord.get").at("events"); }

		// The parameters of the commands that ran, oldest first (engine.commandLog), by name.
		Json ran(const std::string& command)
		{
			Json found = Json::array();
			const Json log = call("engine.commandLog");
			for (const Json& each : log.at("commands"))
				if (each.at("command") == command)
					found.push_back(each.at("parameters"));
			return found;
		}

		test::UiApp app{{1000, 600}};
		ui::EventDisplay& display;
	};
}

TEST_CASE("Add Track adds the chord track once", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const auto elements = [&app](const std::string& selector)
	{ return app.ui().dom(std::nullopt, selector, 0).at("elements"); };
	const auto click = [&app](const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};
	click("#add-track");
	REQUIRE(app.waitUntil([&] { return !elements("#add-track-dialog").empty(); }));
	// The list scrolls: Chord is below Arranger and Transpose.
	click("#type-arranger");
	app.ui().key(std::nullopt, "Down");
	app.ui().key(std::nullopt, "Down");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(elements("#track-types").at(0).at("attributes").at("value") == "chord");
	CHECK(elements("#track-name").empty());
	click("#add-track-confirm");
	REQUIRE(app.waitUntil([&] { return app.call("chord.get").at("track") != 0; }));

	// Once: a second chord track isn't added, and the dialog says why.
	const auto chordTracks = [&app]
	{
		const Json tracks = app.call("project.get").at("tracks");
		return std::count_if(
			tracks.begin(), tracks.end(), [](const Json& track) { return track.at("type") == "chord"; });
	};
	if (elements("#add-track-dialog").empty())
	{
		click("#add-track");
		REQUIRE(app.waitUntil([&] { return !elements("#add-track-dialog").empty(); }));
	}
	click("#type-arranger");
	app.ui().key(std::nullopt, "Down");
	app.ui().key(std::nullopt, "Down");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(elements("#track-types").at(0).at("attributes").at("value") == "chord");
	click("#add-track-confirm");
	CHECK(app.waitUntil([&] { return !elements("#add-track-message").at(0).value("text", std::string()).empty(); }));
	INFO(elements("#add-track-message").dump());
	CHECK(chordTracks() == 1);
}

TEST_CASE("the chord track's lane draws, types and erases chord events", "[ui]")
{
	Window window;
	window.call("track.add", {{"type", "chord"}});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".chord-row", "hidden"); }));

	// The draw tool adds an undefined chord at the nearest bar and opens it: Enter takes the chord typed.
	window.display.setTool(ui::Tool::Draw);
	window.click(".chord-row");
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 1; }));
	CHECK(window.events()[0].at("chord") == "X");
	CHECK(window.events()[0].at("position").get<int64_t>() % (4 * g_quarter) == 0);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".chord-field", "shown"); }));
	window.type("Am7");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("chord") == "Am7"; }));
	CHECK(window.app.waitUntil([&] { return window.element(".chord-event").value("text", std::string()) == "Am7"; }));

	// The erase tool removes the event clicked.
	window.display.setTool(ui::Tool::Erase);
	window.click(".chord-event");
	CHECK(window.app.waitUntil([&] { return window.events().empty(); }));
}

TEST_CASE("a chord event drags to the nearest bar and opens for typing on a double click", "[ui]")
{
	Window window;
	window.call("track.add", {{"type", "chord"}});
	window.call("chord.add", {{"position", 0}, {"chord", "C"}});
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-event").size() == 1; }));
	window.display.setTool(ui::Tool::ObjectSelection);
	// Three fifths of a bar (1.2 seconds at 120 BPM) to the right: onto the second bar's line.
	const Json box = window.element(".chord-event").at("box");
	const double x = box.at("x").get<double>() + (box.at("width").get<double>() / 2.0);
	const double y = box.at("y").get<double>() + (box.at("height").get<double>() / 2.0);
	const double across = 1.2 / window.display.view().scale * 0.5; // pixels, at a dp ratio of 0.5
	window.app.ui().drag(std::nullopt, {".chord-event", 0, std::nullopt},
		{"", 0, ui::Point{static_cast<int>(std::lround(x + across)), static_cast<int>(std::lround(y))}});
	REQUIRE(window.app.waitUntil([&] { return window.events().at(0).at("position") == 4 * g_quarter; }));

	window.click(".chord-event", 2);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".chord-field", "shown"); }));
	window.type("G7/B");
	CHECK(window.app.waitUntil([&] { return window.events().at(0).at("chord") == "G7/B"; }));
}

TEST_CASE("Project > Chord Track converts chords to MIDI and back, the Inspector sets Follow Chord Track", "[ui]")
{
	Window window;
	window.app.appUi().docking().open("inspector");
	window.app.shell().step(std::chrono::milliseconds(0));
	const auto track = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	window.call("chord.add", {{"position", 0}, {"chord", "C"}});
	window.call("chord.add", {{"position", 4 * g_quarter}, {"chord", "Am"}});
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"] .track-name";
	REQUIRE(window.app.waitUntil([&] { return window.elements(row).size() == 1; }));
	window.click(row);
	const auto parts = [&]
	{
		Json found = Json::array();
		const Json project = window.call("project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
				found.push_back(part);
		return found;
	};
	// The MIDI Inserts and MIDI Sends sections close, so that the Chords section fits in the window.
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [section=midiSends]", "unavailable"); }));
	window.click(".inspector [section=midiInserts] .section-header");
	window.click(".inspector [section=midiSends] .section-header");
	REQUIRE(window.hasClass(".inspector [section=midiInserts]", "collapsed"));
	REQUIRE(window.hasClass(".inspector [section=midiSends]", "collapsed"));

	// Chords to MIDI: the voicings on the selected track, which then follows the chord track with Voicings.
	window.app.keys().run("track.chordsToMidiSelected", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil([&] { return parts().size() == 1; }));
	CHECK(parts()[0].at("notes").size() == 8);
	const std::string follow = ".inspector select[control=followChords]";
	REQUIRE(
		window.app.waitUntil([&] { return window.element(follow).at("attributes").value("value", "") == "voicings"; }));

	// The Inspector's Follow Chord Track: Root Notes, just above Voicings.
	window.click(follow);
	window.app.ui().key(std::nullopt, "Up");
	window.click(follow);
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("follow")[0].at("mode") == "rootNotes"; }));

	// The Inspector's Single Voice shows the voice, This Track's Voicings and the master; Live Input.
	window.call("chord.follow", {{"track", track}, {"mode", "singleVoice"}});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [row=followVoice]", "unavailable"); }));
	window.click(".inspector select[control=followVoice]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=followVoice]");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("follow")[0].at("voice") == "alto"; }));
	window.click(".inspector select[control=liveInput]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=liveInput]");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("follow")[0].at("liveInput") == "chords"; }));

	// Create Chord Events: the dialog's options reach chord.fromMidi, then the chords of the selected track's notes
	// replace those in their range.
	window.call("chord.set", {{"events", {{{"event", window.events()[1].at("id")}, {"chord", "F"}}}}});
	window.app.keys().run("track.createChordEventsSelected", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil([&] { return window.elements("#create-chord-events-dialog").size() == 1; }));
	window.click("#chord-events-bass");
	window.click("#chord-events-arpeggios");
	window.click("#chord-events-sustain");
	window.click("#chord-events-min-length", 2);
	window.type("120");
	window.click("#create-chord-events");
	CHECK(window.app.waitUntil([&] { return window.events().size() == 2 && window.events()[1].at("chord") == "Am"; }));
	REQUIRE(window.ran("chord.fromMidi").size() == 1);
	CHECK(window.ran("chord.fromMidi")[0] ==
		Json{{"parts", {parts()[0].at("id")}}, {"includeBass", false}, {"includeTensions", true},
			{"detectArpeggios", true}, {"sustainPedal", true}, {"minLength", 120 * (g_quarter / 480)}});

	// Map to Chord Track: the notes move to the chord track's chords for good.
	window.call("chord.follow", {{"track", track}, {"mode", "off"}});
	window.call("chord.set", {{"events", {{{"event", window.events()[1].at("id")}, {"chord", "Dm"}}}}});
	window.app.keys().run("track.mapToChordTrackSelected", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil([&] { return window.elements("#map-to-chord-track-dialog").size() == 1; }));
	window.click("#map-to-chord-track");
	CHECK(window.app.waitUntil(
		[&]
		{
			const Json notes = parts()[0].at("notes");
			return std::any_of(notes.begin(), notes.end(), [](const Json& note) { return note.at("pitch") == 62; });
		}));
}

TEST_CASE("the scale lane, the chord track's controls and its Inspector settings", "[ui]")
{
	Window window;
	window.app.appUi().docking().open("inspector");
	window.app.shell().step(std::chrono::milliseconds(0));
	const auto track = window.call("track.add", {{"type", "chord"}}).at("track").at("id").get<uint64_t>();
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	REQUIRE(window.app.waitUntil([&] { return window.elements(row + " button[control=showScales]").size() == 1; }));
	CHECK(window.hasClass(".scale-row", "hidden"));

	// Show Scales: the scale lane, where the draw tool adds a scale event to type.
	window.click(row + " button[control=showScales]");
	REQUIRE(window.app.waitUntil([&] { return window.call("chord.get").at("showScales") == true; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".scale-row", "hidden"); }));
	// The row loses its class before the next layout, so its box is only right a frame later.
	CHECK(window.app.waitUntil(
		[&]
		{
			const Json chordBox = window.element(".chord-row").at("box");
			const Json scaleBox = window.element(".scale-row").at("box");
			return scaleBox.at("y").get<double>() >=
				chordBox.at("y").get<double>() + chordBox.at("height").get<double>() - 1.0;
		}));
	window.display.setTool(ui::Tool::Draw);
	window.click(".scale-row");
	REQUIRE(window.app.waitUntil([&] { return window.call("chord.get").at("scales").size() == 1; }));
	CHECK(window.call("chord.get").at("events").empty());
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".scale-field", "shown"); }));
	window.type("D dorian");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("scales")[0].at("scale") == "D dorian"; }));
	CHECK(window.app.waitUntil(
		[&] { return window.element(".scale-event").value("text", std::string()) == "D dorian"; }));

	// Record Enable and Mute Chord Track; Select Track for Auditioning lists the MIDI track.
	for (const auto& [control, key] : {std::pair{"chordRecord", "recordEnable"}, std::pair{"chordMute", "mute"}})
	{
		window.click(row + " button[control=" + control + "]");
		CHECK(window.app.waitUntil([&window, key = key] { return window.call("chord.get").at(key) == true; }));
	}
	window.click(row + " select[control=chordAudition]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(row + " select[control=chordAudition]");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("audition") == midi; }));

	// The Inspector: the voicing library and its subset, the piano's range, Shift Chords.
	window.display.setTool(ui::Tool::ObjectSelection);
	window.click(row + " .track-name");
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [section=chordTrack]", "unavailable"); }));
	CHECK(window.hasClass(".inspector [row=chordVoicingStyle]", "unavailable"));
	window.click(".inspector select[control=chordVoicingLibrary]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=chordVoicingLibrary]");
	REQUIRE(window.app.waitUntil([&] { return window.call("chord.get").at("voicing").at("library") == "piano"; }));
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".inspector [row=chordLowest]", "unavailable"); }));
	window.click(".inspector select[control=chordVoicingStyle]");
	window.app.ui().key(std::nullopt, "Down");
	window.click(".inspector select[control=chordVoicingStyle]");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("voicing").at("style") == "triadsMaj9"; }));
	window.click(".inspector button[control=chordFattenUp]");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("voicing").at("fattenUp") == true; }));
	window.click(".inspector valuefield[control=chordShift]", 2);
	window.type("-10");
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("shiftChords") == -10 * (g_quarter / 480); }));
}

TEST_CASE(
	"Project > Chord Track takes the selected parts, else the selected tracks' parts and loose audio events", "[ui]")
{
	const test::TempDir dir;
	Window window;
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto audio = window.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const auto part = [&](int64_t position)
	{
		return window.call("midiPart.add", {{"track", midi}, {"position", position}, {"length", 4 * g_quarter}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = part(0);
	const uint64_t second = part(8 * g_quarter);
	// Two audio events, the second inside an audio part (reference manual p. 1344: not from audio parts).
	const auto file = dir.path() / "silence.wav";
	test::writeWav(file, {std::vector<float>(48000, 0.0F), 1, 48000});
	const Json media = window.call("media.add", {{"path", file.u8string()}}).at("media");
	const Json clip = window.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const auto event = [&](int64_t position)
	{
		return window
			.call("audioEvent.add",
				{{"track", audio}, {"clip", clip.at("id")}, {"position", position}, {"length", 48000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t loose = event(0);
	window.call("event.toPart", {{"events", {event(96000)}}});
	const auto row = [](uint64_t track)
	{ return "#track-list listitem[value=\"" + std::to_string(track) + "\"] .track-name"; };
	REQUIRE(window.app.waitUntil([&] { return window.elements(row(audio)).size() == 1; }));
	const auto run = [&](const char* action)
	{ window.app.keys().run(action, Json::object(), window.app.main(), nullptr); };

	// The selected audio track: its loose audio events, not those in its parts.
	window.click(row(audio));
	window.display.select({});
	run("track.createChordEventsSelected");
	REQUIRE(window.app.waitUntil([&] { return window.ran("chord.fromAudio").size() == 1; }));
	CHECK(window.ran("chord.fromAudio")[0] == Json{{"events", {loose}}});

	// The selected MIDI track: all its parts; with a part selected, only that one.
	window.click(row(midi));
	window.display.select({});
	run("track.mapToChordTrackSelected");
	REQUIRE(window.app.waitUntil([&] { return window.elements("#map-to-chord-track-dialog").size() == 1; }));
	window.click("#map-to-chord-track");
	REQUIRE(window.app.waitUntil([&] { return window.ran("chord.map").size() == 1; }));
	CHECK(window.ran("chord.map")[0].at("parts") == Json{first, second});
	window.display.select({second});
	run("track.createChordEventsSelected");
	REQUIRE(window.app.waitUntil([&] { return window.elements("#create-chord-events-dialog").size() == 1; }));
	window.click("#create-chord-events");
	REQUIRE(window.app.waitUntil([&] { return window.ran("chord.fromMidi").size() == 1; }));
	CHECK(window.ran("chord.fromMidi")[0].at("parts") == Json{second});
	CHECK(window.ran("chord.fromAudio").size() == 1); // no audio event selected
	run("track.mapToChordTrackSelected");
	REQUIRE(window.app.waitUntil([&] { return window.elements("#map-to-chord-track-dialog").size() == 1; }));
	window.click("#map-to-chord-track");
	REQUIRE(window.app.waitUntil([&] { return window.ran("chord.map").size() == 2; }));
	CHECK(window.ran("chord.map")[1].at("parts") == Json{second});
}

TEST_CASE("with Acoustic Feedback, pressing a chord or scale event auditions it until the button is let go", "[ui]")
{
	Window window;
	window.call("track.add", {{"type", "chord"}});
	const auto midi = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto chord = window.call("chord.add", {{"position", 0}, {"chord", "C"}}).at("event").at("id");
	const auto scale = window.call("chord.add", {{"position", 0}, {"scale", "D dorian"}}).at("event").at("id");
	window.call("chord.setTrack", {{"audition", midi}, {"acousticFeedback", true}, {"showScales", true}});
	REQUIRE(window.app.waitUntil(
		[&] { return window.elements(".chord-event").size() == 1 && window.elements(".scale-event").size() == 1; }));
	window.display.setTool(ui::Tool::ObjectSelection);
	size_t count = 0;
	for (const auto& [selector, id] : {std::pair{".chord-event", chord}, std::pair{".scale-event", scale}})
	{
		window.app.ui().press(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left);
		window.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(window.app.waitUntil([&] { return window.ran("chord.audition").size() == count + 1; }));
		CHECK(window.ran("chord.audition")[count] == Json{{"event", id}});
		window.app.ui().release(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left);
		window.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(window.app.waitUntil([&] { return window.ran("chord.audition").size() == count + 2; }));
		CHECK(window.ran("chord.audition")[count + 1] == Json{{"on", false}});
		count += 2;
	}
}
