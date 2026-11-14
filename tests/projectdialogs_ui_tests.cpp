// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs that app::AppUi opens for tracks, parts or notes of the project close when a new or loaded project replaces
// it (#1464): object ids are per project, so OK would act on whatever has those ids in the new project. Dialogs for the
// whole project stay open. Driven with ui::Automation as MCP's UI tools drive them.
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	bool open(test::UiApp& app, const std::string& selector)
	{
		return !app.ui().dom(std::nullopt, selector, 0).at("elements").empty();
	}

	// A MIDI track with a part, both selected in the project window.
	void selectPart(test::UiApp& app, ui::EventDisplay& display)
	{
		const auto track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
		const auto part =
			app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 3840}}).at("part").at("id");
		app.call("note.add",
			{{"part", part},
				{"notes", Json::array({{{"position", 0}, {"length", 480}, {"pitch", 60}, {"velocity", 0.5}}})}});
		REQUIRE(app.waitUntil(
			[&display, &part]
			{
				const Json parts =
					display.project().is_object() ? display.project().value("midiParts", Json::array()) : Json::array();
				return std::any_of(parts.begin(), parts.end(), [&part](const Json& p) { return p.at("id") == part; });
			},
			std::chrono::seconds(30)));
		app.appUi().trackList()->select({track});
		display.select({part.get<uint64_t>()});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void run(test::UiApp& app, const std::string& command, const std::string& dialog)
	{
		INFO(command);
		app.keys().run(command, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, &dialog] { return open(app, dialog); }));
		app.shell().step(std::chrono::milliseconds(0));
	}

	// project.new, and the steps until the project window shows the empty project: the new project's ids start again,
	// so the next part may have the id of the old project's.
	void newProject(test::UiApp& app, ui::EventDisplay& display)
	{
		app.call("project.new");
		REQUIRE(app.waitUntil([&display]
			{ return display.project().is_object() && display.project().value("tracks", Json::array()).empty(); }));
		app.shell().step(std::chrono::milliseconds(0));
	}
}

TEST_CASE("dialogs for the project's tracks, parts or notes close when a new project replaces it", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const std::pair<const char*, const char*> dialogs[] = {{"track.autoFades", "#auto-fades"},
		{"project.addTrack", "#add-track-dialog"}, {"track.addEffectToSelected", "#add-track-dialog"},
		{"track.saveTrackPreset", "#track-preset-dialog"}, {"midi.transposeSetup", "#midi-function"},
		{"midi.ruleEditor", "#rule-editor"}};
	for (const auto& [command, dialog] : dialogs)
	{
		INFO(command);
		selectPart(app, display);
		run(app, command, dialog);
		newProject(app, display);
		CHECK(app.waitUntil([&app, dialog = std::string(dialog)] { return !open(app, dialog); }));
	}

	// The project's auto fades and the project input transformer aren't for objects of the project: they stay.
	for (const auto& [command, dialog] :
		{std::pair{"project.autoFades", "#auto-fades"}, std::pair{"project.inputTransformer", "#rule-editor"}})
	{
		INFO(command);
		selectPart(app, display);
		run(app, command, dialog);
		newProject(app, display);
		CHECK(open(app, dialog));
	}
}
