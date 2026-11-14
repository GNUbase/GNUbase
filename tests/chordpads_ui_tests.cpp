// SPDX-License-Identifier: GPL-3.0-or-later
// The chord pads zone in the project window (#174, docs/features/chord-pads.md), driven with ui::Automation as MCP's
// UI tools drive it: shown and hidden from the Project menu, pads played and recorded with the mouse, typed, swapped,
// changed with the toolbars and the wheel, presets and the root key, and the chord assistant's suggestions.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/midifile/smf.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	struct Window
	{
		Window()
		{
			app.appUi().showProject();
			app.setDpRatio(0.5F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.size() != 1)
				return false;
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		std::string text(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.size() == 1 ? found[0].value("text", std::string()) : std::string();
		}

		void click(const std::string& selector, int count = 1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void step() { app.shell().step(std::chrono::milliseconds(0)); }

		// Opens the chord pads with Project > Show/Hide Chord Pads.
		void open()
		{
			app.keys().run("window.chordPads", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([&] { return elements(".chord-pad").size() == 12; }));
		}

		Json pads() { return call("chordPads.get").at("pads"); }

		bool visible(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.size() == 1 && found[0].value("visible", false);
		}

		// The element's box, or an empty one if there isn't exactly one.
		Json box(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.size() == 1 ? found[0].at("box") : Json{{"x", 0}, {"y", 0}, {"width", 0}, {"height", 0}};
		}

		// Whether the two elements' boxes don't overlap.
		bool apart(const std::string& first, const std::string& second)
		{
			const Json a = box(first);
			const Json b = box(second);
			const auto edge = [](const Json& item, const char* origin, const char* size)
			{ return item.at(origin).get<double>() + item.at(size).get<double>(); };
			return edge(a, "x", "width") <= b.at("x").get<double>() ||
				edge(b, "x", "width") <= a.at("x").get<double>() || edge(a, "y", "height") <= b.at("y").get<double>() ||
				edge(b, "y", "height") <= a.at("y").get<double>();
		}

		// Whether the first element's box lies within the second's.
		bool within(const std::string& inner, const std::string& outer)
		{
			const Json a = box(inner);
			const Json b = box(outer);
			const auto at = [](const Json& item, const char* key) { return item.at(key).get<double>(); };
			return at(a, "width") > 0 && at(a, "x") >= at(b, "x") && at(a, "y") >= at(b, "y") &&
				at(a, "x") + at(a, "width") <= at(b, "x") + at(b, "width") &&
				at(a, "y") + at(a, "height") <= at(b, "y") + at(b, "height");
		}

		// Whether the first element's box lies in the second's upper right quarter.
		bool inUpperRight(const std::string& inner, const std::string& outer)
		{
			const Json a = box(inner);
			const Json b = box(outer);
			const auto at = [](const Json& item, const char* key) { return item.at(key).get<double>(); };
			return at(a, "width") > 0 && at(a, "x") >= at(b, "x") + (at(b, "width") / 2) &&
				at(a, "y") + at(a, "height") <= at(b, "y") + (at(b, "height") / 2);
		}

		static std::string pad(int index) { return ".chord-pad[pad=\"" + std::to_string(index) + "\"]"; }

		test::UiApp app{{1200, 700}};
	};

	// The notes of the track's MIDI parts, by pitch.
	std::vector<int> recordedPitches(const Json& project, uint64_t track)
	{
		std::vector<int> pitches;
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
				for (const Json& note : part.at("notes"))
					pitches.push_back(note.at("pitch").get<int>());
		std::sort(pitches.begin(), pitches.end());
		return pitches;
	}
}

TEST_CASE("Project > Show/Hide Chord Pads shows the pads in the lower zone and hides them", "[ui]")
{
	Window window;
	window.open();
	CHECK(window.app.appUi().docking().showing("chordPads"));
	CHECK(window.text(Window::pad(0) + " .chord-pad-chord") == "C");
	CHECK(window.text(Window::pad(4) + " .chord-pad-degree") == "V");
	CHECK(window.text(Window::pad(0) + " .chord-pad-key") == "C1");
	CHECK(window.hasClass(Window::pad(7), "unassigned"));
	// The Project menu has the item.
	CHECK(!window.elements("menuitem[command=\"window.chordPads\"]").empty());
	window.app.keys().run("window.chordPads", Json::object(), window.app.main(), nullptr);
	CHECK(window.app.waitUntil([&] { return !window.app.appUi().docking().showing("chordPads"); }));
}

TEST_CASE("a pad held with the mouse plays its chord, which a record-enabled track records", "[ui]")
{
	Window window;
	const Json added = window.call("track.add", {{"type", "instrument"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	window.call("channel.set",
		{{"channel", added.at("channel").at("id")}, {"instrument", "noteProbe"}, {"recordEnable", true}});
	window.open();
	window.call("transport.record");
	window.call("audio.render", {{"frames", 4800}});
	window.app.ui().press(std::nullopt, {Window::pad(5), 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(Window::pad(5), "triggered"); }));
	CHECK(window.hasClass(Window::pad(5), "selected"));
	window.call("audio.render", {{"frames", 4800}});
	window.app.ui().release(std::nullopt, {Window::pad(5), 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(Window::pad(5), "triggered"); }));
	window.call("audio.render", {{"frames", 4800}});
	window.call("transport.stop");
	window.call("audio.render", {{"frames", 4800}});
	REQUIRE(window.app.waitUntil(
		[&] { return recordedPitches(window.call("project.get"), track).size() == 4; }, std::chrono::seconds(10)));
	CHECK(recordedPitches(window.call("project.get"), track) == std::vector<int>{57, 69, 72, 76});
}

TEST_CASE("pads are typed, swapped, changed with the toolbars and the wheel, and filled from presets", "[ui]")
{
	Window window;
	window.open();

	// A double click opens the chord field: Enter assigns the chord typed.
	window.click(Window::pad(8), 2);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#chord-pads-field", "shown"); }));
	window.app.ui().key(std::nullopt, "Ctrl+A");
	window.app.ui().text(std::nullopt, "Fmaj7");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[8].at("chord") == "Fmaj7"; }));
	CHECK(window.app.waitUntil([&] { return window.text(Window::pad(8) + " .chord-pad-chord") == "Fmaj7"; }));

	// A pad dragged onto another swaps them.
	window.app.ui().drag(std::nullopt, {Window::pad(0), 0, std::nullopt}, {Window::pad(1), 0, std::nullopt});
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("chord") == "C"; }));
	CHECK(window.pads()[0].at("chord") == "Dm");

	// The extended toolbar on the selected pad (the one dropped on): Transpose Up, More Tensions, Lock.
	window.click("#chord-pads-transposeUp");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("chord") == "C#"; }));
	window.click("#chord-pads-moreTensions");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("chord") == "C#maj7"; }));
	window.click("#chord-pads-lock");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("locked") == true; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass(Window::pad(1), "locked"); }));

	// The wheel over a pad: down the next voicing, with Shift up a semitone.
	window.app.ui().wheel(std::nullopt, {Window::pad(3), 0, std::nullopt}, 0.0F, 1.0F);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[3].at("voicing") == 1; }));
	window.app.ui().wheel(std::nullopt, {Window::pad(3), 0, std::nullopt}, 0.0F, -1.0F, {"shift"});
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[3].at("chord") == "F#"; }));

	// Select All and Remove Assignment: the locked pad stays.
	window.click("#chord-pads-selectAll");
	window.click("#chord-pads-remove");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord").get<std::string>().empty(); }));
	CHECK(window.pads()[1].at("chord") == "C#maj7");

	// A preset with what to load, and the root key with Transpose Pads.
	window.call("chordPads.setPad", {{"pad", 1}, {"locked", false}});
	window.click("#chord-pads-loadPreset");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord") == "C"; }));
	window.click("#chord-pads-transposePads");
	window.step();
	window.click("#chord-pads-root");
	window.app.ui().key(std::nullopt, "Down");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-root");
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("rootKey") == 2; }));
	CHECK(window.pads()[0].at("chord") == "D");
}

TEST_CASE("the chord assistant suggests chords after the selected pad's and assigns them", "[ui]")
{
	Window window;
	window.open();
	// The lower zone is high enough for every suggestion and chord editor button: in a narrower window, such as CI's
	// 1024 pixels, the toolbars take more rows and the suggestions under the assistant's fold aren't hit by a click.
	window.app.appUi().docking().setSize("project", "lower", 400.0);
	window.step();
	window.call("chordPads.assign", {{"assignments", {{{"pad", 6}, {"chord", ""}}}}});
	window.click("#chord-pads-assistant");
	window.click(Window::pad(0));
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-suggestion").size() == 24; }));
	CHECK(window.text(".chord-suggestion[chord=\"F\"] .chord-suggestion-degree") == "IV");
	CHECK(window.hasClass(".chord-suggestion[chord=\"F\"]", "level-0"));

	// A double click assigns the suggestion to the first unassigned pad; it is one of the last three auditioned.
	window.click(".chord-suggestion[chord=\"F\"]", 2);
	REQUIRE(window.app.waitUntil([&] { return window.pads()[6].at("chord") == "F"; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass(".chord-suggestion[chord=\"F\"]", "recent"); }));

	// The Circle of Fifths: the root key's chord first, the key's chords with their degrees.
	window.click("#chord-pads-page-circle");
	REQUIRE(
		window.app.waitUntil([&] { return window.elements(".chord-circle-outer .chord-suggestion").size() == 12; }));
	CHECK(window.text(".chord-circle-outer .chord-suggestion[chord=\"G\"] .chord-suggestion-degree") == "V");
	CHECK(window.hasClass(".chord-circle-inner .chord-suggestion[chord=\"Am\"]", "in-key"));
	// A context click assigns it to the first unassigned pad (p. 1375).
	window.click(".chord-circle-outer .chord-suggestion[chord=\"D\"]");
	window.app.ui().click(std::nullopt, {".chord-circle-outer .chord-suggestion[chord=\"D\"]", 0, std::nullopt},
		ui::MouseButton::Right, 1, {});
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[7].at("chord") == "D"; }));

	// The chord editor's buttons (#1089): the selected pad's chord lit, a button makes another chord.
	window.click("#chord-pads-page-editor");
	REQUIRE(window.app.waitUntil(
		[&] { return window.hasClass(".chord-editor-button[part=root][value=\"0\"]", "checked"); }));
	CHECK(window.hasClass(".chord-editor-button[part=type][value=maj]", "checked"));
	window.click(".chord-editor-button[part=type][value=min]");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord") == "Cm"; }));
	// The next click builds on the panel's copy of the pads, which can lag behind the engine: wait until the panel
	// shows the chord. The buttons are toggles, so their own state changes with the click and proves nothing.
	REQUIRE(window.app.waitUntil([&] { return window.text(Window::pad(0) + " .chord-pad-chord") == "Cm"; }));
	window.click(".chord-editor-button[part=tension][value=\"7\"]");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord") == "Cm7"; }));
	REQUIRE(window.app.waitUntil([&] { return window.text(Window::pad(0) + " .chord-pad-chord") == "Cm7"; }));
	window.click(".chord-editor-button[part=bass][value=\"7\"]");

	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord") == "Cm7/G"; }));
	CHECK(window.app.waitUntil(
		[&] { return window.hasClass(".chord-editor-button[part=bass][value=\"7\"]", "checked"); }));
	window.call("chordPads.assign", {{"assignments", {{{"pad", 0}, {"chord", "C"}}}}});

	// The Proximity assistant: rows by distance above the reference (#1089); the List's cadence types.
	window.click("#chord-pads-page-proximity");
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-proximity-row").size() >= 2; }));
	// The panel shows the pad that chordPads.assign above put back once its event arrived.
	REQUIRE(window.app.waitUntil([&] { return window.text(".chord-proximity-reference") == "C"; }));
	CHECK(window.elements(".chord-proximity-row[distance=\"1\"] .chord-suggestion[chord=\"Am\"]").size() == 1);
	window.click("#chord-pads-page-list");
	REQUIRE(window.app.waitUntil([&] { return window.visible("#chord-pads-cadenceType"); }));
	window.click("#chord-pads-cadenceType");
	window.app.ui().key(std::nullopt, "Down");
	window.app.ui().key(std::nullopt, "Down");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-cadenceType");
	CHECK(window.app.waitUntil([&] { return window.elements(".chord-suggestion[chord=\"F\"]").empty(); }));
	CHECK(window.app.waitUntil([&] { return window.elements(".chord-suggestion[chord=\"G\"]").size() == 1; }));
}

TEST_CASE("the chord assistant drops an answer for a page that was left before it arrived", "[ui]")
{
	Window window;
	window.open();
	window.app.appUi().docking().setSize("project", "lower", 400.0);
	window.step();
	window.click("#chord-pads-assistant");
	window.click(Window::pad(0));
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-suggestion").size() == 24; }));
	// Two page clicks with no step between them: the first page's question is answered after the second page opened.
	const auto clickTwo = [&](const std::string& first, const std::string& second)
	{
		for (const std::string& selector : {first, second})
			window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		window.step();
		REQUIRE(window.app.waitUntil([&] { return window.app.appUi().binding().waiting("chordPads.assistant") == 0; }));
	};

	// The Circle of Fifths' answer doesn't replace the chord editor's buttons.
	clickTwo("#chord-pads-page-circle", "#chord-pads-page-editor");
	CHECK(window.hasClass(".chord-editor-button[part=root][value=\"0\"]", "checked"));
	CHECK(window.elements(".chord-circle-outer").empty());

	// The List's answer isn't drawn as the Proximity page's rows.
	clickTwo("#chord-pads-page-list", "#chord-pads-page-proximity");
	CHECK(window.elements(".chord-proximity-row").size() >= 2);
	CHECK(window.text(".chord-proximity-reference") == "C");
}

TEST_CASE("the pads show their lock and adaptive voicing, and Lock on several pads is one undo step", "[ui]")
{
	// Reference manual pp. 1353-1354: the adaptive voicing indicator and the padlock in the pad's upper right corner.
	Window window;
	window.open();
	CHECK(window.visible(Window::pad(0) + " .chord-pad-adaptive"));
	CHECK_FALSE(window.visible(Window::pad(0) + " .chord-pad-lock"));
	const auto steps = window.call("history.get").at("steps").size();
	window.click(Window::pad(0));
	window.click(Window::pad(1), 1, {"ctrl"});
	window.click("#chord-pads-lock");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("locked") == true; }));
	CHECK(window.pads()[0].at("locked") == true);
	CHECK(window.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(window.app.waitUntil([&] { return window.visible(Window::pad(0) + " .chord-pad-lock"); }));
	CHECK_FALSE(window.visible(Window::pad(0) + " .chord-pad-adaptive"));

	// Undone, the pads show it, as chordPads.changed tells them.
	window.call("history.undo");
	REQUIRE(window.app.waitUntil([&] { return !window.visible(Window::pad(0) + " .chord-pad-lock"); }));
	CHECK(window.visible(Window::pad(1) + " .chord-pad-adaptive"));
}

TEST_CASE(
	"the adaptive voicing reference shows its indicator next to the adaptive voicing indicator or the padlock", "[ui]")
{
	// Reference manual p. 1353 (#1048): the reference indicator in the pad's upper right corner, besides its frame.
	Window window;
	window.open();
	CHECK_FALSE(window.visible(Window::pad(1) + " .chord-pad-reference"));
	window.call("chordPads.setPad", {{"pad", 1}, {"reference", true}});
	REQUIRE(window.app.waitUntil([&] { return window.visible(Window::pad(1) + " .chord-pad-reference"); }));
	CHECK(window.hasClass(Window::pad(1), "reference"));
	CHECK_FALSE(window.visible(Window::pad(0) + " .chord-pad-reference"));
	REQUIRE(window.visible(Window::pad(1) + " .chord-pad-adaptive"));
	CHECK(window.apart(Window::pad(1) + " .chord-pad-reference", Window::pad(1) + " .chord-pad-adaptive"));
	CHECK(window.within(Window::pad(1) + " .chord-pad-reference", Window::pad(1)));
	CHECK(window.inUpperRight(Window::pad(1) + " .chord-pad-reference", Window::pad(1))); // #1174

	// A locked reference shows the padlock beside it; another pad as the reference takes the indicator along.
	window.call("chordPads.setPad", {{"pad", 1}, {"locked", true}});
	REQUIRE(window.app.waitUntil([&] { return window.visible(Window::pad(1) + " .chord-pad-lock"); }));
	CHECK(window.visible(Window::pad(1) + " .chord-pad-reference"));
	CHECK(window.apart(Window::pad(1) + " .chord-pad-reference", Window::pad(1) + " .chord-pad-lock"));
	CHECK(window.within(Window::pad(1) + " .chord-pad-lock", Window::pad(1)));
	window.call("chordPads.setPad", {{"pad", 2}, {"reference", true}});
	REQUIRE(window.app.waitUntil([&] { return window.visible(Window::pad(2) + " .chord-pad-reference"); }));
	CHECK_FALSE(window.visible(Window::pad(1) + " .chord-pad-reference"));
}

TEST_CASE("Adaptive Voicing on a selection with a locked pad turns it on for the unlocked pads", "[ui]")
{
	Window window;
	window.open();
	window.call("chordPads.setPad", {{"pad", 0}, {"locked", true}}); // locking turns its adaptive voicing off
	window.call("chordPads.setPad", {{"pad", 1}, {"adaptive", false}});
	REQUIRE(window.app.waitUntil([&] { return window.visible(Window::pad(0) + " .chord-pad-lock"); }));
	const auto steps = window.call("history.get").at("steps").size();
	window.click(Window::pad(0));
	window.click(Window::pad(1), 1, {"ctrl"});
	window.click("#chord-pads-adaptive");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("adaptive") == true; }));
	CHECK(window.pads()[0].at("adaptive") == false);
	CHECK(window.pads()[0].at("locked") == true);
	CHECK(window.call("history.get").at("steps").size() == steps + 1);
}

TEST_CASE("a value the engine refuses goes back, and the wheel over the root key changes it", "[ui]")
{
	Window window;
	window.open();
	// The remote range's start above its end is refused: the field shows the start it had.
	window.click("#chord-pads-remoteStart", 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "60");
	window.app.ui().key(std::nullopt, "Enter");
	window.step();
	CHECK(window.app.waitUntil(
		[&] { return window.elements("#chord-pads-remoteStart")[0].at("attributes").value("value", "") == "36"; }));
	CHECK(window.text("#chord-pads-remoteStart .valuefield-text") == "36");
	CHECK(window.call("chordPads.get").at("remoteStart") == 36);

	// The wheel over the root key: down the next key, up the previous one (p. 1364).
	window.app.ui().wheel(std::nullopt, {"#chord-pads-root", 0, std::nullopt}, 0.0F, 1.0F);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("rootKey") == 1; }));
	CHECK(window.pads()[0].at("chord") == "C");
	// Once the panel shows the new key, two notches in one frame both count, before the engine answered the first.
	REQUIRE(window.app.waitUntil(
		[&] { return window.elements("#chord-pads-root")[0].at("attributes").value("value", "") == "1"; }));
	window.app.ui().wheel(std::nullopt, {"#chord-pads-root", 0, std::nullopt}, 0.0F, -1.0F);
	window.app.ui().wheel(std::nullopt, {"#chord-pads-root", 0, std::nullopt}, 0.0F, -1.0F);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("rootKey") == 11; }));
}

TEST_CASE("with step input, Left and Right move the step position by bars", "[ui]")
{
	// Reference manual p. 1369.
	Window window;
	window.open();
	// The velocity shows that the zone has the new settings.
	window.call("chordPads.set", {{"stepInput", true}, {"velocity", 90}});
	REQUIRE(window.app.waitUntil(
		[&] { return window.elements("#chord-pads-velocity")[0].at("attributes").value("value", "") == "90"; }));
	window.click(Window::pad(0));
	REQUIRE(window.app.waitUntil([&] { return window.call("chord.get").at("events").size() == 1; }));
	const auto bar = window.call("chordPads.get").at("stepPosition").get<int64_t>();
	CHECK(bar > 0);
	window.app.ui().key(std::nullopt, "Right");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepPosition") == 2 * bar; }));
	window.app.ui().key(std::nullopt, "Left");
	window.app.ui().key(std::nullopt, "Left");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepPosition") == 0; }));
	// A pad where a chord is replaces it.
	window.click(Window::pad(4));
	REQUIRE(window.app.waitUntil([&] { return window.call("chord.get").at("events")[0].at("chord") == "G"; }));
	CHECK(window.call("chord.get").at("events").size() == 1);
}

TEST_CASE(
	"right after Step Input is turned on, Left and Right move the step position, but not in a value field", "[ui]")
{
	// Reference manual p. 1369 (#1048): while step input is on, the keys move the step input cursor wherever the focus
	// is in the zone, such as on the Step Input button, so that no pad needs to be clicked, which inserts its chord.
	Window window;
	window.open();
	window.click("#chord-pads-stepInput");
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepInput") == true; }));
	// The velocity shows that the zone has the new settings.
	window.call("chordPads.set", {{"velocity", 90}});
	REQUIRE(window.app.waitUntil(
		[&] { return window.elements("#chord-pads-velocity")[0].at("attributes").value("value", "") == "90"; }));
	window.app.ui().key(std::nullopt, "Right");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepPosition") > 0; }));
	const auto bar = window.call("chordPads.get").at("stepPosition").get<int64_t>();
	CHECK(window.call("chord.get").at("events").empty());

	// The velocity field takes them for its value: the two Left there don't take the position back to 0.
	window.click("#chord-pads-velocity");
	window.app.ui().key(std::nullopt, "Left");
	window.app.ui().key(std::nullopt, "Left");
	window.step();
	window.click("#chord-pads-moveInsert");
	window.app.ui().key(std::nullopt, "Right");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepPosition") == 2 * bar; }));
	CHECK(window.call("chord.get").at("events").empty());
}

TEST_CASE("Left and Right are no undo step, and leave the step position in the chord field, the root key menu and with "
		  "Cmd",
	"[ui]")
{
	// #1089: moving the step input cursor is navigation. #1174: the chord field and the root key menu take the keys,
	// and with Cmd (meta) they are other key commands, such as Nudge on macOS.
	Window window;
	window.open();
	window.click(Window::pad(0)); // selected before step input is on, where a click inserts the pad's chord
	window.call("chordPads.set", {{"stepInput", true}, {"velocity", 90}});
	REQUIRE(window.app.waitUntil(
		[&] { return window.elements("#chord-pads-velocity")[0].at("attributes").value("value", "") == "90"; }));
	const Json steps = window.call("history.get").at("position");
	window.app.ui().key(std::nullopt, "Right");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepPosition") > 0; }));
	const auto bar = window.call("chordPads.get").at("stepPosition").get<int64_t>();
	window.app.ui().key(std::nullopt, "Meta+Right");
	window.app.ui().key(std::nullopt, "Meta+Left");
	window.step();
	CHECK(window.call("history.get").at("position") == steps);
	CHECK(window.call("chordPads.get").at("stepPosition") == bar);

	// The chord field with the caret at the end of its text lets Right through.
	window.click("#chord-pads-edit");
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#chord-pads-field", "shown"); }));
	window.app.ui().key(std::nullopt, "End");
	window.app.ui().key(std::nullopt, "Right");
	window.app.ui().key(std::nullopt, "Escape");
	window.step();
	CHECK(window.call("chordPads.get").at("stepPosition") == bar);

	// The root key menu, open and closed: the keys move the focus on along the toolbar.
	window.click("#chord-pads-root");
	window.app.ui().key(std::nullopt, "Right");
	window.click("#chord-pads-root");
	window.app.ui().key(std::nullopt, "Left");
	window.step();
	CHECK(window.call("chordPads.get").at("stepPosition") == bar);
	// A last Right on a button moves it on, after all the keys before it were handled.
	window.click("#chord-pads-moveInsert");
	window.app.ui().key(std::nullopt, "Right");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("stepPosition") == 2 * bar; }));
	CHECK(window.call("chord.get").at("events").empty());
}

TEST_CASE("the toolbar and the settings panes change output mode, players, player mode, remote and display", "[ui]")
{
	// Reference manual pp. 1347-1349, 1376-1391 (#1089).
	Window window;
	window.open();
	const auto pads = [&] { return window.call("chordPads.get"); };
	window.click("#chord-pads-outputMode");
	REQUIRE(window.app.waitUntil([&] { return pads().at("outputMode") == false; }));
	window.click("#chord-pads-snapPlayback");
	REQUIRE(window.app.waitUntil([&] { return pads().at("snapPlayback") == true; }));
	// Enh. Shift: the pads spell with flats.
	window.call("chordPads.assign", {{"assignments", {{{"pad", 7}, {"chord", "C#m"}}}}});
	window.click("#chord-pads-flats");
	REQUIRE(window.app.waitUntil([&] { return window.text(Window::pad(7) + " .chord-pad-chord") == "Dbm"; }));

	// Player Settings: a piano player with a style; the octave offset is the basic player's.
	window.click("#chord-pads-pane-player-button");
	REQUIRE(window.visible("#chord-pads-player"));
	CHECK(window.visible("#chord-pads-octaveOffset"));
	window.click("#chord-pads-library");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-library");
	window.click("#chord-pads-addPlayer");
	REQUIRE(window.app.waitUntil([&] { return pads().at("players").size() == 2; }));
	CHECK(pads().at("players")[1].at("library") == "piano");
	REQUIRE(window.app.waitUntil([&] { return window.visible("#chord-pads-style"); }));
	CHECK_FALSE(window.visible("#chord-pads-octaveOffset"));
	window.click("#chord-pads-style");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-style");
	REQUIRE(window.app.waitUntil([&] { return pads().at("players")[1].at("style") == "triadsMaj9"; }));
	window.click("#chord-pads-player-name", 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "Keys");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return pads().at("players")[1].at("name") == "Keys"; }));
	// The player's track (p. 1377): the instrument track added meanwhile.
	const auto track = window.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	window.click("#chord-pads-pane-player-button");
	window.click("#chord-pads-pane-player-button"); // shown again, with the tracks there are now
	window.call("history.get");                     // after the zone's project.get
	window.step();
	window.click("#chord-pads-player-track");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-player-track");
	REQUIRE(window.app.waitUntil([&] { return pads().at("players")[1].at("track") == track; }));
	window.click("#chord-pads-removePlayer");
	REQUIRE(window.app.waitUntil([&] { return pads().at("players").size() == 1; }));

	// Player Mode: Overlaps and Filter Notes; one pane shows at a time.
	window.click("#chord-pads-pane-mode-button");
	CHECK_FALSE(window.visible("#chord-pads-player"));
	window.click("#chord-pads-overlaps");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-overlaps");
	REQUIRE(window.app.waitUntil([&] { return pads().at("overlaps") == "legato"; }));
	window.click("#chord-pads-filterNotes");
	REQUIRE(window.app.waitUntil([&] { return pads().at("filterNotes") == true; }));
	// The pattern player mode with a preset (pp. 1379-1380), and back.
	CHECK_FALSE(window.visible("#chord-pads-patternPreset"));
	window.click("#chord-pads-playerMode");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-playerMode");
	REQUIRE(window.app.waitUntil([&] { return pads().at("mode") == "pattern"; }));
	REQUIRE(window.app.waitUntil([&] { return window.visible("#chord-pads-patternPreset"); }));
	window.click("#chord-pads-patternPreset");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-patternPreset");
	REQUIRE(window.app.waitUntil([&] { return pads().at("pattern").at("name") == "arpeggioUp"; }));
	CHECK(window.app.waitUntil([&] { return window.text("#chord-pads-pattern-info") == "arpeggioUp (4 voices)"; }));
	window.click("#chord-pads-velocityFromKeyboard");
	REQUIRE(window.app.waitUntil([&] { return pads().at("velocityFromKeyboard") == true; }));
	// The pattern's progress shows on the playing pad (pp. 1354, 1391).
	CHECK(pads().at("pattern").at("seconds") == 2.0);
	CHECK_FALSE(window.visible(Window::pad(3) + " .chord-pad-progress"));
	window.call("chordPads.trigger", {{"pad", 3}});
	REQUIRE(window.app.waitUntil([&] { return window.visible(Window::pad(3) + " .chord-pad-progress"); }));
	window.call("chordPads.trigger", {{"pad", 3}, {"on", false}});
	REQUIRE(window.app.waitUntil([&] { return !window.visible(Window::pad(3) + " .chord-pad-progress"); }));
	window.call("chordPads.set", {{"mode", "plainChords"}});
	REQUIRE(window.app.waitUntil([&] { return window.visible("#chord-pads-overlaps"); }));

	// Player Remote Control (p. 1389) and the sections mode's settings.
	window.click("#chord-pads-pane-playerRemote-button");
	REQUIRE(window.visible("#chord-pads-sectionKeys-0"));
	window.click("#chord-pads-sectionKeys-1", 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "61");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return pads().at("sectionKeys")[1] == 61; }));
	CHECK(pads().at("sectionKeys")[0] == 55);
	window.click("#chord-pads-mute-2");
	REQUIRE(window.app.waitUntil([&] { return pads().at("mutedSections")[2] == true; }));
	window.click("#chord-pads-latchPads");
	REQUIRE(window.app.waitUntil([&] { return pads().at("latchPads") == true; }));
	window.click("#chord-pads-sectionPlay");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-sectionPlay");
	REQUIRE(window.app.waitUntil([&] { return pads().at("sectionPlay") == "chordPads"; }));

	// Remote Settings: Latch Chords, the chord modifiers and their keys, MIDI Learn.
	window.click("#chord-pads-pane-remote-button");
	window.click("#chord-pads-latch");
	REQUIRE(window.app.waitUntil([&] { return pads().at("latch") == true; }));
	window.click("#chord-pads-modifiers");
	REQUIRE(window.app.waitUntil([&] { return pads().at("modifiers") == true; }));
	window.click("#chord-pads-modifier-transposeUp", 2);
	window.app.ui().key(std::nullopt, "Mod+A");
	window.app.ui().text(std::nullopt, "60");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return pads().at("modifierKeys").at("transposeUp") == 60; }));
	// Transpose All Pads by a controller (p. 1387).
	window.click("#chord-pads-controlSource-2");
	window.app.ui().key(std::nullopt, "Down");
	window.app.ui().key(std::nullopt, "Down");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-controlSource-2");
	REQUIRE(window.app.waitUntil([&] { return pads().at("controlSources")[2] == "controller"; }));
	window.click("#chord-pads-learn-range");
	REQUIRE(window.app.waitUntil([&] { return pads().at("listening") == Json{{"learn", "range"}}; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass("#chord-pads-learn-range", "checked"); }));
	window.click("#chord-pads-learn-range");
	REQUIRE(window.app.waitUntil([&] { return pads().at("listening").is_null(); }));

	// Display: roman numerals as the primary symbol, Nashville numbers below, the grid layout.
	window.click("#chord-pads-pane-display-button");
	window.click("#chord-pads-primary");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-primary");
	REQUIRE(window.app.waitUntil([&] { return window.text(Window::pad(4) + " .chord-pad-chord") == "V"; }));
	window.call(
		"settings.set", {{"key", "ui.chordPads"}, {"value", {{"primary", "roman"}, {"secondary", "nashville"}}}});
	REQUIRE(window.app.waitUntil([&] { return window.text(Window::pad(1) + " .chord-pad-degree") == "2m"; }));
	CHECK(window.text(Window::pad(1) + " .chord-pad-voicing") == "0");
	window.click("#chord-pads-voicingIndicators");
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#chord-pads-pads", "no-voicings"); }));
	window.click("#chord-pads-layout");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-layout");
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#chord-pads-pads", "grid"); }));
	CHECK(window.visible("#chord-pads-columns"));
}

TEST_CASE("presets are saved and stepped through, and the pads' key commands select, remove, edit and lock", "[ui]")
{
	// Reference manual pp. 1348, 1351, 1357, 1362, 1371.
	Window window;
	window.open();
	window.click("#chord-pads-savePreset");
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#chord-pads-preset-name", "shown"); }));
	window.app.ui().text(std::nullopt, "Mine");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.presets").at("presets").back() == "Mine"; }));
	// Next and Previous Preset load the neighbors in the list: minor after major, then back, and before major the
	// last one, the user's.
	// The zone shows the preset loaded before the next button goes from it.
	const auto shows = [&](const std::string& preset)
	{
		return window.app.waitUntil(
			[&]
			{
				return window.call("chordPads.get").at("preset") == preset &&
					window.elements("#chord-pads-preset")[0].at("attributes").value("value", "") == preset;
			});
	};
	window.click("#chord-pads-nextPreset");
	REQUIRE(shows("minor"));
	window.click("#chord-pads-previousPreset");
	REQUIRE(shows("major"));
	window.click("#chord-pads-previousPreset");
	REQUIRE(shows("Mine"));

	// The key commands while the zone has the focus.
	window.click(Window::pad(1));
	window.app.ui().key(std::nullopt, "Mod+Shift+L");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("locked") == true; }));
	window.app.ui().key(std::nullopt, "Mod+A");
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(Window::pad(11), "selected"); }));
	window.app.ui().key(std::nullopt, "Delete");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord").get<std::string>().empty(); }));
	CHECK(window.pads()[1].at("chord") == "Dm"); // locked
	window.click(Window::pad(2));
	window.app.ui().key(std::nullopt, "Enter");
	CHECK(window.app.waitUntil([&] { return window.hasClass("#chord-pads-field", "shown"); }));
}

TEST_CASE("pad modifier buttons play the pad, the keyboard shows the notes, and chords drop onto pads", "[ui]")
{
	// Reference manual pp. 1351, 1353-1360.
	Window window;
	window.open();
	const auto key = [](int pitch) { return ".chord-pads-key[pitch=" + std::to_string(pitch) + "]"; };
	window.call("chordPads.set", {{"remote", true}});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(key(36), "remote"); }));
	CHECK(window.hasClass(key(47), "remote"));
	CHECK_FALSE(window.hasClass(key(48), "remote"));
	CHECK(window.hasClass(key(37), "black"));

	// Dragging the keyboard scrolls it to the left and right and zooms it up and down (p. 1355).
	const auto keys = [&] { return window.elements(".chord-pads-key"); };
	REQUIRE(keys().size() == 73);
	CHECK(keys()[0].at("attributes").at("pitch") == "24");
	const Json strip = window.box("#chord-pads-keyboard");
	const auto at = [&](double x)
	{
		return ui::Target{"", 0,
			ui::Point{static_cast<int>(std::lround(strip.at("x").get<double>() + x)),
				static_cast<int>(std::lround(strip.at("y").get<double>() + (strip.at("height").get<double>() / 2.0)))}};
	};
	const double keyWidth = strip.at("width").get<double>() / 73.0;
	window.app.ui().drag(std::nullopt, at(strip.at("width").get<double>() / 2.0),
		at((strip.at("width").get<double>() / 2.0) - (12.0 * keyWidth)));
	window.step();
	// About an octave on: the strip's padding makes a key's width a little different from the box's share.
	REQUIRE(window.app.waitUntil(
		[&] { return std::abs(std::stoi(keys()[0].at("attributes").at("pitch").get<std::string>()) - 36) <= 1; }));
	window.app.ui().press(std::nullopt, at(100.0), ui::MouseButton::Left);
	window.app.ui().move(std::nullopt,
		{"", 0,
			ui::Point{static_cast<int>(strip.at("x").get<double>() + 100.0),
				static_cast<int>(strip.at("y").get<double>() - 40.0)}});
	window.app.ui().release(std::nullopt, {}, ui::MouseButton::Left);
	window.step();
	CHECK(window.app.waitUntil([&] { return keys().size() < 73; }));

	// Transpose Up plays the first selected pad's new chord while the button is down.
	window.click(Window::pad(0));
	window.app.ui().press(std::nullopt, {"#chord-pads-transposeUp", 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(Window::pad(0), "triggered"); }));
	CHECK(window.pads()[0].at("chord") == "C#");
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(key(49), "played"); }));
	window.app.ui().release(std::nullopt, {"#chord-pads-transposeUp", 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(Window::pad(0), "triggered"); }));
	CHECK(window.pads()[0].at("chord") == "C#"); // once, not again on the click
	CHECK_FALSE(window.hasClass(key(49), "played"));

	// The hovered pad's trigger key.
	window.app.ui().move(std::nullopt, {Window::pad(2), 0, std::nullopt});
	window.step();
	CHECK(window.app.waitUntil([&] { return window.hasClass(key(38), "hover"); }));

	// A pad that a remote key triggers lights up.
	window.call("chordPads.remoteInput", {{"events", {{{"kind", "triggered"}, {"key", 39}, {"velocity", 90}}}}});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(Window::pad(3), "triggered"); }));
	window.call("chordPads.remoteInput", {{"events", {{{"kind", "released"}, {"key", 39}}}}});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(Window::pad(3), "triggered"); }));

	// An assistant's suggestion dragged onto a pad goes onto it. The lower zone is high enough for the suggestion to be
	// above the assistant's fold, in a narrow window too, where the toolbars take more rows.
	window.app.appUi().docking().setSize("project", "lower", 400.0);
	window.step();
	window.click("#chord-pads-assistant");
	window.click(Window::pad(0));
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-suggestion").size() == 24; }));
	const Json first = window.elements(".chord-suggestion")[0];
	const std::string chord = first.at("attributes").at("chord").get<std::string>();
	// While it is dragged, the pad it would go onto shows the drop location's frame (p. 1354).
	window.app.ui().press(std::nullopt, {".chord-suggestion", 0, std::nullopt}, ui::MouseButton::Left);
	window.app.ui().move(std::nullopt, {Window::pad(5), 0, std::nullopt});
	window.step();
	CHECK(window.app.waitUntil([&] { return window.hasClass(Window::pad(5), "drop-target"); }));
	window.app.ui().release(std::nullopt, {Window::pad(5), 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[5].at("chord") == chord; }));
	CHECK_FALSE(window.hasClass(Window::pad(5), "drop-target"));

	// The List assistant's colors on the pads, after the first selected pad's chord.
	window.call("settings.set", {{"key", "ui.chordPads"}, {"value", {{"listColors", true}}}});
	window.click(Window::pad(0));
	CHECK(window.app.waitUntil([&] { return window.hasClass(Window::pad(5), "level-0"); }));

	// Assign from MIDI Input on the selected pad: its red frame until it's off again.
	window.click("#chord-pads-assignMidi");
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(Window::pad(0), "listening"); }));
	CHECK(window.call("chordPads.get").at("listening") == Json{{"assign", 0}});
	window.click("#chord-pads-assignMidi");
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(Window::pad(0), "listening"); }));
}

TEST_CASE("the step input cursor shows in the ruler and the event display while step input is on", "[ui]")
{
	// Reference manual p. 1369 (#1089): a line with a handle in the ruler, at the step position.
	Window window;
	window.open();
	const std::string cursor = ".event-display .step-input-cursor";
	REQUIRE(window.elements(cursor).size() == 1);
	CHECK(window.hasClass(cursor, "hidden"));
	window.call("chordPads.set", {{"stepInput", true}});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(cursor, "hidden"); }));
	CHECK(window.elements(cursor + " .step-input-handle").size() == 1);
	window.step();
	const double start = window.box(cursor).at("x").get<double>();
	window.call("chordPads.set", {{"stepBars", 2}});
	REQUIRE(window.app.waitUntil(
		[&]
		{
			window.step();
			return window.box(cursor).at("x").get<double>() > start;
		}));
	window.call("chordPads.set", {{"stepInput", false}});
	CHECK(window.app.waitUntil([&] { return window.hasClass(cursor, "hidden"); }));
}

TEST_CASE("a pad dragged onto a track's lane makes a MIDI part there, onto the chord track a chord event", "[ui]")
{
	// Reference manual p. 1370 (#1089).
	Window window;
	const auto track = window.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	window.open();
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"]";
	REQUIRE(window.app.waitUntil([&] { return window.elements(row).size() == 1; }));
	window.step();
	const auto onLane = [&](const std::string& rowSelector)
	{
		const Json lanes = window.box(".event-display .events");
		const Json at = window.box(rowSelector);
		const double x = lanes.at("x").get<double>() + (lanes.at("width").get<double>() / 4.0);
		const double y = at.at("y").get<double>() + (at.at("height").get<double>() / 2.0);
		return ui::Target{"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
	};
	window.app.ui().drag(std::nullopt, {Window::pad(0), 0, std::nullopt}, onLane(row));
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("project.get").at("midiParts").size() == 1; }));
	const Json part = window.call("project.get").at("midiParts")[0];
	CHECK(part.at("track") == track);
	std::vector<int> pitches;
	for (const Json& note : part.at("notes"))
		pitches.push_back(note.at("pitch").get<int>());
	std::sort(pitches.begin(), pitches.end());
	CHECK(pitches == std::vector<int>{48, 60, 64, 67});
	CHECK(window.pads()[0].at("chord") == "C"); // no swap with a pad

	// Onto the chord track a chord event.
	const auto chords = window.call("track.add", {{"type", "chord"}}).at("track").at("id").get<uint64_t>();
	const std::string chordRow = "#track-list listitem[value=\"" + std::to_string(chords) + "\"]";
	REQUIRE(window.app.waitUntil([&] { return window.elements(chordRow).size() == 1; }));
	window.step();
	window.app.ui().drag(std::nullopt, {Window::pad(4), 0, std::nullopt}, onLane(chordRow));
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.call("chord.get").at("events").size() == 1; }));
	CHECK(window.call("chord.get").at("events")[0].at("chord") == "G");
}

TEST_CASE("the chord assistant keeps the hint for a pad without a chord selected before an answer came", "[ui]")
{
	// #1391: only the latest question's answer is drawn.
	Window window;
	window.open();
	window.app.appUi().docking().setSize("project", "lower", 400.0);
	window.step();
	window.click("#chord-pads-assistant");
	window.click(Window::pad(0));
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-suggestion").size() == 24; }));
	// A question for pad 1's chord, and then a pad without a chord selected, with no step between them.
	for (const int pad : {1, 9})
		window.app.ui().click(std::nullopt, {Window::pad(pad), 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.app.appUi().binding().waiting("chordPads.assistant") == 0; }));
	window.step();
	CHECK(window.text("#chord-pads-suggestions .chord-pads-hint") == "Select a pad with a chord");
	CHECK(window.elements(".chord-suggestion").empty());
}

TEST_CASE(
	"MIDI Learn of every remote key and control, the section keys, the pad layout and Import from MIDI Loop", "[ui]")
{
	// Reference manual pp. 1355, 1379, 1387-1391 (#1236).
	Window window;
	window.open();
	const auto pads = [&] { return window.call("chordPads.get"); };
	const auto learning = [&](const std::string& target)
	{
		window.click("#chord-pads-learn-" + target);
		REQUIRE(window.app.waitUntil([&] { return pads().at("listening") == Json{{"learn", target}}; }));
		CHECK(window.app.waitUntil([&] { return window.hasClass("#chord-pads-learn-" + target, "checked"); }));
	};
	window.click("#chord-pads-pane-remote-button");
	learning("nextVoicing");
	learning("transposeAllPads"); // another target instead
	CHECK_FALSE(window.hasClass("#chord-pads-learn-nextVoicing", "checked"));
	window.click("#chord-pads-learn-transposeAllPads");
	REQUIRE(window.app.waitUntil([&] { return pads().at("listening").is_null(); }));
	window.click("#chord-pads-pane-playerRemote-button");
	learning("sectionKeys");
	learning("playerKeys");
	window.click("#chord-pads-learn-playerKeys");
	REQUIRE(window.app.waitUntil([&] { return pads().at("listening").is_null(); }));

	// The keyboard shows the section and subsection keys in the sections player mode (p. 1355).
	const auto key = [](int pitch) { return ".chord-pads-key[pitch=" + std::to_string(pitch) + "]"; };
	window.call("chordPads.set", {{"remote", true}, {"subsectionKeys", {62}}});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(key(36), "remote"); }));
	CHECK_FALSE(window.hasClass(key(55), "section"));
	window.call("chordPads.set", {{"mode", "sections"}});
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(key(55), "section"); }));
	CHECK(window.hasClass(key(59), "section"));
	CHECK(window.hasClass(key(62), "section"));
	CHECK_FALSE(window.hasClass(key(60), "section"));

	// Number of Pads and Start Note in the keyboard layout, Number of Rows in the grid (p. 1391).
	const auto type = [&](const std::string& field, const std::string& text)
	{
		window.click(field, 2);
		window.app.ui().key(std::nullopt, "Mod+A");
		window.app.ui().text(std::nullopt, text);
		window.app.ui().key(std::nullopt, "Enter");
	};
	window.click("#chord-pads-pane-display-button");
	REQUIRE(window.visible("#chord-pads-padCount"));
	CHECK(window.visible("#chord-pads-startNote"));
	CHECK_FALSE(window.visible("#chord-pads-rows"));
	type("#chord-pads-padCount", "16");
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-pad").size() == 16; }));
	type("#chord-pads-startNote", "48");
	REQUIRE(window.app.waitUntil([&] { return pads().at("remoteStart") == 48; }));
	CHECK(pads().at("remoteEnd") == 59); // the range as long as it was
	window.call("settings.set", {{"key", "ui.chordPads"}, {"value", {{"layout", "grid"}, {"columns", 4}}}});
	REQUIRE(window.app.waitUntil([&] { return window.visible("#chord-pads-rows"); }));
	CHECK_FALSE(window.visible("#chord-pads-padCount"));
	type("#chord-pads-rows", "3");
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-pad").size() == 12; }));

	// Import from MIDI Loop: the loop chosen in the file dialog becomes the pattern (p. 1379).
	const gnubase::test::TempDir dir;
	const std::filesystem::path loop = dir.path() / "Loop.mid";
	engine::midifile::File file;
	file.ticksPerQuarter = 480;
	engine::midifile::Track track;
	for (int i = 0; i < 3; ++i)
	{
		const int64_t tick = int64_t{i} * 480;
		track.events.push_back(engine::midifile::channelEvent(tick, 0x90, static_cast<uint8_t>(50 + i), 100));
		track.events.push_back(engine::midifile::channelEvent(tick + 240, 0x80, static_cast<uint8_t>(50 + i), 0));
	}
	track.end = int64_t{4} * 480;
	file.tracks.push_back(track);
	engine::midifile::save(file, loop.u8string());
	window.call("chordPads.set", {{"mode", "pattern"}});
	window.click("#chord-pads-pane-mode-button");
	REQUIRE(window.app.waitUntil([&] { return window.visible("#chord-pads-importLoop"); }));
	window.click("#chord-pads-importLoop");
	REQUIRE(window.app.waitUntil([&] { return window.app.shell().fileDialogs().size() == 1; }));
	window.app.shell().answerFileDialog(window.app.shell().fileDialogs().begin()->first, {loop});
	REQUIRE(window.app.waitUntil([&] { return pads().at("pattern").at("name") == "Loop"; }));
}
