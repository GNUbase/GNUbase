// SPDX-License-Identifier: GPL-3.0-or-later
// Quantizing in the app (#132, docs/features/quantize.md), driven with ui::Automation as MCP's UI tools drive it: the
// Edit menu's quantize items on the project window's selected MIDI parts and on an editor's selected notes, the
// quantize panel and the toolbar's quantize section. tests/quantize_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_sixteenth = time::g_ticksPerQuarter / 4;
	constexpr int64_t g_bar = 4 * time::g_ticksPerQuarter;
	constexpr int64_t g_unit = g_sixteenth / 24;         // the notes' offsets from the grid are multiples of it
	constexpr int64_t g_displayTick = g_sixteenth / 120; // the quantize panel's ticks (p. 375)

	// A MIDI track with a one-bar part "Keys" at the project start and three notes just off the 1/16 grid, a 1/16 being
	// 24 g_unit: at 26, 50 and 73 g_unit, each 20 long, in the project window.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Keys"}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			Json items = Json::array();
			for (const int64_t position : {26 * g_unit, 50 * g_unit, 73 * g_unit})
				items.push_back({{"position", position}, {"length", 20 * g_unit}, {"pitch", 60}, {"velocity", 0.5}});
			const Json added = app.call("note.add", {{"part", part}, {"notes", items}});
			for (const Json& note : added.at("notes"))
				notes.push_back(note.at("id").get<uint64_t>());
			REQUIRE(app.waitUntil(
				[this]
				{
					if (!display.project().is_object())
						return false;
					const Json parts = display.project().value("midiParts", Json::array());
					return std::any_of(
						parts.begin(), parts.end(), [this](const Json& p) { return p.at("id") == part; });
				}));
		}

		Json elements(const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
		{
			return app.ui().dom(window, selector, 0).at("elements");
		}

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].at("visible").get<bool>();
		}

		std::string text(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.empty() ? std::string() : found[0].value("text", std::string());
		}

		bool hasAttribute(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].value("attributes", Json::object()).contains(name);
		}

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector, int count = 1)
		{
			INFO(selector);
			REQUIRE(app.waitUntil([&] { return shown(selector); }));
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Edit > item, or Edit > submenu > item.
		void menu(const std::string& command, const char* submenu = nullptr)
		{
			click("#menu-bar button[menu=edit-menu]");
			if (submenu != nullptr)
			{
				app.ui().move(std::nullopt, {submenu, 0, std::nullopt});
				app.shell().step(std::chrono::milliseconds(0));
			}
			click("menuitem[command=\"" + command + "\"]");
		}

		// Types a number into a value field.
		void type(const std::string& selector, const std::string& value)
		{
			click(selector, 2);
			app.ui().key(std::nullopt, "Ctrl+A");
			app.ui().text(std::nullopt, value);
			app.ui().key(std::nullopt, "Enter");
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Types text into a text field.
		void write(const std::string& selector, const std::string& value)
		{
			click(selector);
			app.ui().key(std::nullopt, "Ctrl+A");
			app.ui().text(std::nullopt, value);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json partNotes()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
				{
					std::vector<Json> sorted(each.at("notes").begin(), each.at("notes").end());
					std::sort(sorted.begin(), sorted.end(),
						[](const Json& a, const Json& b) { return a.at("id").get<int>() < b.at("id").get<int>(); });
					return sorted;
				}
			return Json::array();
		}

		// The notes' starts or lengths, in the order they were added.
		std::vector<int64_t> values(const char* field)
		{
			std::vector<int64_t> found;
			for (const Json& note : partNotes())
				found.push_back(note.at(field).get<int64_t>());
			return found;
		}

		bool startsAre(const std::vector<int64_t>& expected)
		{
			return app.waitUntil([&] { return values("position") == expected; });
		}

		bool lengthsAre(const std::vector<int64_t>& expected)
		{
			return app.waitUntil([&] { return values("length") == expected; });
		}

		Json settings() { return app.call("quantize.get").at("settings"); }

		bool hasPreset(const std::string& name)
		{
			const Json presets = app.call("quantize.presets").at("presets");
			return std::any_of(presets.begin(), presets.end(), [&](const Json& p) { return p.at("name") == name; });
		}

		void openPanel()
		{
			app.keys().run("window.quantizePanel", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([this] { return shown("#quantize-panel"); }));
		}

		size_t lines() { return elements("#quantize-panel .quantize-line").size(); }

		test::UiApp app{{1024, 720}}; // as large as CI runners' displays give, where the toolbar is tight
		ui::EventDisplay& display;
		uint64_t track = 0;
		uint64_t part = 0;
		std::vector<uint64_t> notes;
	};
}

TEST_CASE("Edit > Quantize, Reset Quantize and Advanced Quantize quantize the selected MIDI parts", "[ui]")
{
	Fixture f;
	// Without a selection there is nothing to quantize.
	CHECK(!f.app.keys().canRun("edit.quantize", f.app.main(), nullptr));
	f.display.select({f.part});
	CHECK(f.app.keys().canRun("edit.quantize", f.app.main(), nullptr));
	CHECK(f.app.keys().keysFor("edit.quantize", Json::object()) == std::vector<std::string>{"Q"});

	f.menu("edit.quantize");
	CHECK(f.startsAre({24 * g_unit, 48 * g_unit, 72 * g_unit}));
	f.menu("edit.resetQuantize");
	CHECK(f.startsAre({26 * g_unit, 50 * g_unit, 73 * g_unit}));

	// Ends to the grid (46, 70 and 93 g_unit to 48, 72 and 96), then lengths to the grid's step: Quantize Link in the
	// project window.
	f.menu("edit.quantizeEnds", "#advanced-quantize-submenu");
	CHECK(f.lengthsAre({22 * g_unit, 22 * g_unit, 23 * g_unit}));
	f.menu("edit.resetQuantize");
	CHECK(f.lengthsAre({20 * g_unit, 20 * g_unit, 20 * g_unit}));
	f.menu("edit.quantizeLengths", "#advanced-quantize-submenu");
	CHECK(f.lengthsAre({g_sixteenth, g_sixteenth, g_sixteenth}));
	f.menu("edit.resetQuantize");
	CHECK(f.lengthsAre({20 * g_unit, 20 * g_unit, 20 * g_unit}));

	// Frozen, the quantized starts stay when quantizing is reset.
	f.menu("edit.quantize");
	CHECK(f.startsAre({24 * g_unit, 48 * g_unit, 72 * g_unit}));
	f.menu("edit.freezeQuantize", "#advanced-quantize-submenu");
	f.menu("edit.resetQuantize");
	f.app.shell().step(std::chrono::milliseconds(50));
	CHECK(f.startsAre({24 * g_unit, 48 * g_unit, 72 * g_unit}));

	// Each is one undo step.
	f.app.call("history.undo");
	f.app.call("history.undo");
	CHECK(f.startsAre({26 * g_unit, 50 * g_unit, 73 * g_unit}));
}

TEST_CASE("the quantize commands take an editor's selected notes, or all of its notes", "[ui]")
{
	Fixture f;
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part})}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Element* zone = window->context().GetDocument(0)->GetElementById("main-zone");
	REQUIRE(zone != nullptr);
	const auto select = [&](const std::vector<uint64_t>& notes)
	{
		Json selection = Json::array();
		for (const uint64_t note : notes)
			selection.push_back({{"part", f.part}, {"note", note}});
		f.app.call("editor.set", {{"editor", "window"}, {"selection", selection}});
		f.app.shell().step(std::chrono::milliseconds(0)); // editor.changed
	};

	// In the editor: its selected notes.
	select({f.notes[0]});
	f.app.keys().run("edit.quantize", Json::object(), *window, zone);
	CHECK(f.startsAre({24 * g_unit, 50 * g_unit, 73 * g_unit}));
	// Without a selection, every note it edits.
	select({});
	f.app.keys().run("edit.quantize", Json::object(), *window, zone);
	CHECK(f.startsAre({24 * g_unit, 48 * g_unit, 72 * g_unit}));
	f.app.keys().run("edit.resetQuantize", Json::object(), *window, zone);
	CHECK(f.startsAre({26 * g_unit, 50 * g_unit, 73 * g_unit}));

	// Quantize Lengths takes the editor's length quantize.
	select({f.notes[1]});
	f.app.call("editor.set", {{"editor", "window"}, {"lengthQuantize", "1/8"}});
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.keys().run("edit.quantizeLengths", Json::object(), *window, zone);
	CHECK(f.lengthsAre({20 * g_unit, 2 * g_sixteenth, 20 * g_unit}));

	// From no zone, as the quantize panel, Lua and MCP run them: an open editor's selected notes first, even with
	// parts selected in the project window.
	f.display.select({f.part});
	select({f.notes[2]});
	f.app.keys().run("edit.quantize", Json::object(), f.app.main(), nullptr);
	CHECK(f.startsAre({26 * g_unit, 50 * g_unit, 72 * g_unit}));
	// The project window's selected parts, while no editor has selected notes.
	select({});
	f.app.keys().run("edit.quantize", Json::object(), f.app.main(), nullptr);
	CHECK(f.startsAre({24 * g_unit, 48 * g_unit, 72 * g_unit}));
}

TEST_CASE("the quantize panel shows and changes the project's quantize settings", "[ui]")
{
	Fixture f;
	f.menu("window.quantizePanel");
	REQUIRE(f.app.waitUntil([&] { return f.shown("#quantize-panel"); }));
	// The grid display shows a bar of the grid: 16 positions of 1/16.
	CHECK(f.app.waitUntil([&] { return f.lines() == 16; }));
	CHECK(f.text("#quantize-grid") == "1/16");
	CHECK(f.text("#quantize-presets") == "1/16");

	f.click("#quantize-grid");
	f.click("menuitem[grid=\"1/8\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("grid") == "1/8"; }));
	CHECK(f.app.waitUntil([&] { return f.lines() == 8; }));

	// Triplets: three steps of a 1/8 in the time of two, so 12 in a bar. Swing waits for a straight grid.
	f.click("#quantize-tuplet");
	f.click("menuitem[tuplet=\"3\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("tuplet") == 3; }));
	CHECK(f.app.waitUntil([&] { return f.lines() == 12; }));
	CHECK(f.hasAttribute("#quantize-swing", "disabled"));
	f.click("#quantize-tuplet");
	f.click("menuitem[tuplet=\"0\"]");
	CHECK(f.app.waitUntil([&] { return !f.hasAttribute("#quantize-swing", "disabled"); }));
	f.type("#quantize-swing", "50");
	CHECK(f.app.waitUntil([&] { return f.settings().at("swing") == 0.5; }));

	f.type("#quantize-catch-range", "30");
	CHECK(f.app.waitUntil([&] { return f.settings().at("catchRange") == 30 * g_displayTick; }));
	CHECK(f.app.waitUntil(
		[&] { return f.elements("#quantize-panel .quantize-catch").size() == 8; })); // a range around each position
	f.type("#quantize-safe-range", "10");
	CHECK(f.app.waitUntil([&] { return f.settings().at("safeRange") == 10 * g_displayTick; }));
	f.type("#quantize-randomize", "5");
	CHECK(f.app.waitUntil([&] { return f.settings().at("randomize") == 5 * g_displayTick; }));
	f.click("#quantize-soft");
	CHECK(f.app.waitUntil([&] { return f.settings().at("soft") == true; }));
	f.type("#quantize-strength", "50");
	CHECK(f.app.waitUntil([&] { return f.settings().at("strength") == 0.5; }));
	// Settings that match no preset.
	CHECK(f.text("#quantize-presets") == "No Preset");

	// The panel follows settings changed by other clients.
	f.app.call("quantize.set", {{"preset", "1/4"}});
	CHECK(f.app.waitUntil([&] { return f.text("#quantize-grid") == "1/4" && f.lines() == 4; }));
	CHECK(f.text("#quantize-presets") == "1/4");
	CHECK(!f.hasClass("#quantize-soft", "checked"));

	// Presets: a factory one sets every setting; the user's are saved, renamed and removed.
	f.click("#quantize-presets");
	f.click("menuitem[preset=\"1/16\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("grid") == "1/16"; }));
	CHECK(f.hasAttribute("#quantize-rename-preset", "disabled"));
	CHECK(f.hasAttribute("#quantize-remove-preset", "disabled"));
	f.click("#quantize-grid");
	f.click("menuitem[grid=\"1/32\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("grid") == "1/32"; }));
	f.type("#quantize-swing", "20"); // so that no factory preset has these settings
	CHECK(f.app.waitUntil([&] { return f.settings().at("swing") == 0.2; }));
	f.write("#quantize-preset-name", "Mine");
	f.click("#quantize-save-preset");
	CHECK(f.app.waitUntil([&] { return f.hasPreset("Mine"); }));
	CHECK(f.app.waitUntil([&] { return f.text("#quantize-presets") == "Mine"; }));
	f.write("#quantize-preset-name", "Yours");
	f.click("#quantize-rename-preset");
	CHECK(f.app.waitUntil([&] { return f.hasPreset("Yours") && !f.hasPreset("Mine"); }));
	CHECK(f.app.waitUntil([&] { return f.text("#quantize-presets") == "Yours"; }));
	f.click("#quantize-remove-preset");
	CHECK(f.app.waitUntil([&] { return !f.hasPreset("Yours"); }));
	CHECK(f.app.waitUntil([&] { return f.text("#quantize-presets") == "No Preset"; }));
}

TEST_CASE("the quantize panel's Apply, Reset and Auto Apply quantize the selected MIDI parts", "[ui]")
{
	Fixture f;
	f.display.select({f.part});
	f.openPanel();
	f.click("#quantize-apply");
	CHECK(f.startsAre({24 * g_unit, 48 * g_unit, 72 * g_unit}));
	f.click("#quantize-reset");
	CHECK(f.startsAre({26 * g_unit, 50 * g_unit, 73 * g_unit}));

	// Auto Apply quantizes again with every change.
	f.click("#quantize-auto-apply");
	CHECK(f.app.waitUntil([&] { return f.hasClass("#quantize-auto-apply", "checked"); }));
	f.click("#quantize-grid");
	f.click("menuitem[grid=\"1/8\"]");
	CHECK(f.startsAre({48 * g_unit, 48 * g_unit, 96 * g_unit}));
	f.click("#quantize-auto-apply");
	f.click("#quantize-grid");
	f.click("menuitem[grid=\"1/16\"]");
	f.app.shell().step(std::chrono::milliseconds(50));
	CHECK(f.startsAre({48 * g_unit, 48 * g_unit, 96 * g_unit}));
}

TEST_CASE("the toolbar's quantize section applies, chooses presets, switches soft quantize and opens the panel", "[ui]")
{
	Fixture f;
	f.display.select({f.part});
	f.click("#toolbar-quantize-presets");
	f.click("menuitem[toolbar-preset=\"1/8\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("grid") == "1/8"; }));
	CHECK(f.app.waitUntil([&] { return f.text("#toolbar-quantize-presets") == "1/8"; }));
	f.click("#toolbar-quantize-soft");
	CHECK(f.app.waitUntil([&] { return f.settings().at("soft") == true; }));
	CHECK(f.app.waitUntil([&] { return f.hasClass("#toolbar-quantize-soft", "checked"); }));
	f.click("#toolbar-quantize-soft");
	CHECK(f.app.waitUntil([&] { return f.settings().at("soft") == false; }));
	f.click("#toolbar-quantize-apply");
	CHECK(f.startsAre({48 * g_unit, 48 * g_unit, 96 * g_unit}));
	f.click("#toolbar-quantize-panel");
	CHECK(f.app.waitUntil([&] { return f.shown("#quantize-panel"); }));
}

TEST_CASE("Create Groove Quantize Preset makes a groove of the selected MIDI part, which the panel then shows", "[ui]")
{
	Fixture f;
	f.display.select({f.part});
	f.openPanel();
	CHECK(f.shown("#quantize-grid-settings"));
	CHECK(!f.shown("#quantize-groove-settings"));
	f.menu("edit.createGrooveQuantize", "#advanced-quantize-submenu");
	CHECK(f.app.waitUntil([&] { return f.hasPreset("Keys"); }));
	CHECK(f.app.waitUntil([&] { return f.settings().at("groove").is_object(); }));
	CHECK(f.app.waitUntil([&] { return f.text("#quantize-presets") == "Keys"; }));
	// The groove's settings instead of the grid's, and its positions in the grid display.
	CHECK(f.app.waitUntil([&] { return f.shown("#quantize-groove-settings"); }));
	CHECK(!f.shown("#quantize-grid-settings"));
	CHECK(f.app.waitUntil([&] { return f.lines() == 3; }));

	f.type("#quantize-velocity", "50");
	CHECK(f.app.waitUntil([&] { return f.settings().at("velocityStrength") == 0.5; }));
	f.type("#quantize-length", "25");
	CHECK(f.app.waitUntil([&] { return f.settings().at("lengthStrength") == 0.25; }));
	f.click("#quantize-pre-quantize");
	f.click("menuitem[pre-quantize=\"1/16\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("preQuantize") == "1/16"; }));
	f.click("#quantize-max-move");
	f.click("menuitem[max-move=\"1/32\"]");
	CHECK(f.app.waitUntil([&] { return f.settings().at("maxMove") == g_sixteenth / 2; }));
	f.type("#quantize-origin", "480");
	CHECK(f.app.waitUntil([&] { return f.settings().at("origin") == 480 * g_displayTick; }));

	// A grid preset goes back to the grid.
	f.click("#quantize-presets");
	f.click("menuitem[preset=\"1/16\"]");
	CHECK(f.app.waitUntil([&] { return f.shown("#quantize-grid-settings"); }));
	CHECK(f.app.waitUntil([&] { return f.lines() == 16; }));
}
