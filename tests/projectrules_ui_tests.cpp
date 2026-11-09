// SPDX-License-Identifier: GPL-3.0-or-later
// The project rule editor in the app (#179, docs/features/project-rules.md), driven with ui::Automation as MCP's UI
// tools drive it. tests/projectrules_tests.cpp checks the rules themselves.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

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

	// A MIDI track with a part at the start and a muted one a bar later, in the project window.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			for (const int64_t bar : {int64_t{0}, int64_t{1}})
				parts.push_back(
					app.call("midiPart.add",
						   {{"track", track}, {"position", bar * 4 * g_quarter}, {"length", 4 * g_quarter}})
						.at("part")
						.at("id")
						.get<uint64_t>());
			app.call("event.set", {{"events", Json::array({parts[1]})}, {"mute", true}});
			REQUIRE(app.waitUntil(
				[this]
				{
					return display.project().is_object() &&
						display.project().value("midiParts", Json::array()).size() == 2;
				}));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].at("visible").get<bool>();
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

		void choose(const std::string& select, int down)
		{
			click(select);
			for (int i = 0; i < std::abs(down); ++i)
				app.ui().key(std::nullopt, down > 0 ? "Down" : "Up");
			click(select);
		}

		void open()
		{
			click("#menu-bar button[menu=project-menu]");
			click("menuitem[command=\"project.ruleEditor\"]");
			REQUIRE(app.waitUntil([this] { return shown("#rule-editor"); }));
			app.shell().step(std::chrono::milliseconds(0)); // its Lua fills it when it shows
			const Json title = elements("#rule-editor-title");
			REQUIRE(!title.empty());
			CHECK(title[0].value("text", std::string()) == "Project Rule Editor");
		}

		void apply()
		{
			click("#rule-editor-apply");
			CHECK(app.waitUntil([this] { return elements("#rule-editor").empty(); }));
		}

		// The index of a preset in the dialog's list, after the empty choice.
		int presetIndex(const std::string& name)
		{
			const Json presets = app.call("projectRules.presets").at("presets");
			for (size_t i = 0; i < presets.size(); ++i)
				if (presets[i].at("name") == name)
					return static_cast<int>(i) + 1;
			return 0;
		}

		Json part(uint64_t id)
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		test::UiApp app{{1024, 720}};
		ui::EventDisplay& display;
		std::vector<uint64_t> parts;
	};
}

TEST_CASE("Project > Project Rule Editor selects, changes and deletes parts", "[ui]")
{
	Fixture f;

	// Select selects what the rules find in the project window.
	f.open();
	f.choose("#rule-editor-preset", f.presetIndex("Select/Muted parts and events"));
	f.apply();
	CHECK(f.app.waitUntil([&] { return f.display.selection() == std::vector<uint64_t>{f.parts[1]}; }));

	// Lines put together in the dialog, on the selection: the selected part a quarter note later, then a post command
	// that undoes it.
	f.open();
	f.click("#rule-editor-insert-filter"); // container type is part
	f.click("#rule-editor-insert-filter");
	f.choose("#rule-editor-filter-2-target", 5);    // from container type to property
	f.choose("#rule-editor-filter-2-condition", 9); // property is set
	f.choose("#rule-editor-filter-2-p1", 1);        // is selected
	f.click("#rule-editor-insert-action");          // position, add
	f.write("#rule-editor-action-1-p1", "480");
	f.apply();
	CHECK(f.app.waitUntil([&] { return f.part(f.parts[1]).at("position") == (4 * g_quarter) + g_quarter; }));
	CHECK(f.part(f.parts[0]).at("position") == 0);

	f.open();
	f.click("#rule-editor-insert-action");
	f.write("#rule-editor-action-1-p1", "480");
	f.write("#rule-editor-post-1", "history.undo");
	f.write("#rule-editor-name", "Moved and back");
	f.click("#rule-editor-save");
	CHECK(f.app.waitUntil([&] { return f.presetIndex("Moved and back") > 0; }));
	f.apply();
	CHECK(f.part(f.parts[0]).at("position") == 0); // moved, then undone
	CHECK(f.app.call("history.get").value("position", size_t{0}) + 1 == f.app.call("history.get").at("steps").size());
	const Json presets = f.app.call("projectRules.presets").at("presets");
	CHECK(presets.back().at("rules").at("post").at(0).at("command") == "history.undo");

	// Delete, and deselect.
	f.open();
	f.choose("#rule-editor-preset", f.presetIndex("Delete/Muted parts and events"));
	f.apply();
	CHECK(f.app.waitUntil([&] { return f.part(f.parts[1]).is_null(); }));
	f.display.select({f.parts[0]});
	f.open();
	f.choose("#rule-editor-preset", f.presetIndex("Deselect/All"));
	f.apply();
	CHECK(f.app.waitUntil([&] { return f.display.selection().empty(); }));
}
