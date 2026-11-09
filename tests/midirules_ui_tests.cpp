// SPDX-License-Identifier: GPL-3.0-or-later
// The rule editor and the project input transformer in the app (#178, docs/features/midi-rules.md), driven with
// ui::Automation as MCP's UI tools drive it. tests/logical_tests.cpp checks the rules themselves.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	// A MIDI track with a one-bar part and the notes C3, E3 and G3, each an 1/8 long, in the project window.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_quarter}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			Json items = Json::array();
			for (const int pitch : {60, 64, 67})
				items.push_back({{"position", (pitch - 60) * g_quarter / 4}, {"length", g_quarter / 2},
					{"pitch", pitch}, {"velocity", 0.5}});
			app.call("note.add", {{"part", part}, {"notes", items}});
			REQUIRE(app.waitUntil(
				[this]
				{
					return display.project().is_object() &&
						!display.project().value("midiParts", Json::array()).empty();
				}));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

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

		// Whether a drop-down list offers an option, whose options aren't in the DOM that the UI tools see.
		bool offers(const std::string& selector, const std::string& value)
		{
			Rml::Element* element = nullptr;
			for (int i = 0; element == nullptr && i < app.main().context().GetNumDocuments(); ++i)
				element = app.main().context().GetDocument(i)->QuerySelector(selector);
			auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(element);
			REQUIRE(select != nullptr);
			for (int i = 0; i < select->GetNumOptions(); ++i)
				if (select->GetOption(i)->GetAttribute<Rml::String>("value", "") == value)
					return true;
			return false;
		}

		void click(const std::string& selector)
		{
			INFO(selector);
			REQUIRE(app.waitUntil([&] { return shown(selector); }));
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void write(const std::string& selector, const std::string& value)
		{
			click(selector);
			app.ui().key(std::nullopt, "Ctrl+A");
			app.ui().text(std::nullopt, value);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Opens a drop-down list, moves the selection that many options down (up if negative) with the arrow keys, and
		// closes it.
		void choose(const std::string& select, int down)
		{
			click(select);
			for (int i = 0; i < std::abs(down); ++i)
				app.ui().key(std::nullopt, down > 0 ? "Down" : "Up");
			click(select);
		}

		// Opens the dialog from a menu of the menu bar.
		void open(const std::string& menu, const std::string& command, const std::string& title)
		{
			click("#menu-bar button[menu=" + menu + "]");
			click("menuitem[command=\"" + command + "\"]");
			REQUIRE(app.waitUntil([this] { return shown("#rule-editor"); }));
			app.shell().step(std::chrono::milliseconds(0)); // its Lua fills it when it shows
			CHECK(text("#rule-editor-title") == title);
		}

		void apply()
		{
			click("#rule-editor-apply");
			CHECK(app.waitUntil([this] { return elements("#rule-editor").empty(); }));
		}

		std::vector<int> pitches()
		{
			std::vector<int> found;
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				for (const Json& note : each.at("notes"))
					found.push_back(note.at("pitch").get<int>());
			std::sort(found.begin(), found.end());
			return found;
		}

		bool are(const std::vector<int>& expected)
		{
			return app.waitUntil([&] { return pitches() == expected; });
		}

		// The index of a preset in the dialog's list, after the empty choice.
		int presetIndex(const std::string& name)
		{
			const Json presets = app.call("midiRules.presets").at("presets");
			for (size_t i = 0; i < presets.size(); ++i)
				if (presets[i].at("name") == name)
					return static_cast<int>(i) + 1;
			return 0;
		}

		test::UiApp app{{1024, 720}};
		ui::EventDisplay& display;
		uint64_t track = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("MIDI > Rule Editor applies presets and lines to the selected parts", "[ui]")
{
	Fixture f;
	CHECK(!f.app.keys().canRun("midi.ruleEditor", f.app.main(), nullptr));
	f.display.select({f.part});

	// A factory preset.
	f.open("midi-menu", "midi.ruleEditor", "Rule Editor");
	f.choose("#rule-editor-preset", f.presetIndex("Transform/Octave up"));
	CHECK(f.shown("#rule-editor-filter-1-target"));
	f.apply();
	CHECK(f.are({72, 76, 79}));

	// Lines put together in the dialog: notes above C4 (72) a semitone down.
	f.open("midi-menu", "midi.ruleEditor", "Rule Editor");
	f.click("#rule-editor-insert-filter");
	f.click("#rule-editor-insert-filter");
	CHECK(f.offers("#rule-editor-filter-2-target", "position"));
	f.choose("#rule-editor-filter-2-target", -3);   // subtype
	f.choose("#rule-editor-filter-2-condition", 2); // bigger
	f.write("#rule-editor-filter-2-p1", "72");
	f.click("#rule-editor-insert-action");
	f.click("#rule-editor-insert-action");
	f.click("#rule-editor-remove-action"); // the last line, as none is picked
	CHECK(f.elements("#rule-editor-action-2-target").empty());
	f.write("#rule-editor-action-1-p1", "-1");
	f.apply();
	CHECK(f.are({72, 75, 78}));

	// Select finds without changing; a value that isn't a number keeps the dialog open with a message.
	f.open("midi-menu", "midi.ruleEditor", "Rule Editor");
	f.choose("#rule-editor-preset", f.presetIndex("Select/Notes above C3"));
	f.click("#rule-editor-apply");
	CHECK(f.app.waitUntil([&] { return f.text("#rule-editor-message") == "3 events found"; }));
	f.click("#rule-editor-insert-action");
	f.write("#rule-editor-action-1-p1", "lots");
	f.click("#rule-editor-apply");
	CHECK(
		f.app.waitUntil([&] { return f.text("#rule-editor-message").find("must be a number") != std::string::npos; }));

	// Save Preset keeps what the dialog shows as one of the user's presets.
	f.choose("#rule-editor-preset", f.presetIndex("Delete/Soft notes"));
	f.write("#rule-editor-name", "Soft ones");
	f.click("#rule-editor-save");
	CHECK(f.app.waitUntil([&] { return f.presetIndex("Soft ones") > 0; }));
	f.click("#rule-editor-cancel");
	CHECK(f.app.waitUntil([&] { return f.elements("#rule-editor").empty(); }));
	CHECK(f.pitches() == std::vector<int>{72, 75, 78});
}

TEST_CASE("Project > Project Input Transformer sets the project's modules", "[ui]")
{
	Fixture f;
	f.open("project-menu", "project.inputTransformer", "Project Input Transformer");
	f.choose("#rule-editor-module", 1); // module 2
	f.click("#rule-editor-on");
	f.click("#rule-editor-insert-action");
	// Live events have no position or length.
	CHECK(f.offers("#rule-editor-action-1-target", "subtype"));
	CHECK(!f.offers("#rule-editor-action-1-target", "position"));
	CHECK(!f.offers("#rule-editor-action-1-target", "length"));
	f.click("#rule-editor-insert-filter");
	CHECK(!f.offers("#rule-editor-filter-1-target", "position"));
	f.click("#rule-editor-remove-filter");
	f.write("#rule-editor-action-1-p1", "12");
	f.apply();
	const Json modules = f.app.call("project.get").at("inputTransformer");
	CHECK(modules[0].at("on") == false);
	CHECK(modules[1].at("on") == true);
	CHECK(modules[1].at("rules").at("actions")[0].at("parameter1") == 12);

	// It shows the module again.
	f.open("project-menu", "project.inputTransformer", "Project Input Transformer");
	f.choose("#rule-editor-module", 1);
	CHECK(f.elements("#rule-editor-on").at(0).at("attributes").contains("checked"));
	CHECK(f.shown("#rule-editor-action-1-p1"));
	f.click("#rule-editor-cancel");
}
