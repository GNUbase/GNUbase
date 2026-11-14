// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs for tracks, parts, events or notes of the project close when a new or loaded project replaces it (#1464,
// #1586): object ids are per project, so OK would act on whatever has those ids in the new project. Dialogs for the
// whole project stay open, and an OS file dialog's answer for objects of the old project is dropped. Driven with
// ui::Automation as MCP's UI tools drive them.
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

	// An audio track with an event of a second of silence, the event selected in the project window.
	void selectEvent(test::UiApp& app, ui::EventDisplay& display, const std::filesystem::path& file)
	{
		const auto track = app.call("track.add", {{"type", "audio"}}).at("track").at("id");
		const Json media =
			app.call(
				   "media.add", {{"path", file.u8string()}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
				.at("media");
		const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id");
		const auto event =
			app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 48000}})
				.at("event")
				.at("id")
				.get<uint64_t>();
		REQUIRE(app.waitUntil(
			[&display] { return display.project().is_object() && display.project().at("audioEvents").size() == 1; }));
		display.select({event});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// The OS file dialog the command opened, answered with the file once the project was replaced. The new project has
	// a track, selected or not, with the id of the old project's.
	void answerAfterNewProject(
		test::UiApp& app, ui::EventDisplay& display, const std::filesystem::path& file, bool select)
	{
		REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
		const uint64_t dialog = app.shell().fileDialogs().begin()->first;
		newProject(app, display);
		const auto track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
		REQUIRE(app.waitUntil(
			[&display] { return display.project().is_object() && display.project().at("tracks").size() == 1; }));
		if (select)
			app.appUi().trackList()->select({track});
		REQUIRE(app.shell().answerFileDialog(dialog, {file}));
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
		{"midi.ruleEditor", "#rule-editor"}, {"midi.mergeInLoop", "#midi-function"},
		{"track.removeSelected", "#remove-tracks-dialog"},
		{"track.createChordEventsSelected", "#create-chord-events-dialog"},
		{"track.mapToChordTrackSelected", "#map-to-chord-track-dialog"},
		{"midi.calculateTempo", "#calculate-tempo-dialog"}, {"edit.repeat", "#repeat-events-dialog"},
		{"tempo.detection", "#tempo-detection-dialog"}};
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

TEST_CASE("the event display's dialogs for audio events close when a new project replaces the project", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "silence.wav";
	test::Audio audio;
	audio.sampleRate = 48000;
	audio.samples.resize(48000);
	test::writeWav(file, audio);
	const std::pair<const char*, const char*> dialogs[] = {{"audio.fadeEditor", "#fade-editor"},
		{"audio.detectSilence", "#detect-silence-dialog"}, {"audio.spectrumAnalyzer", "#spectrum-analyzer-dialog"},
		{"audio.statistics", "#statistics-dialog"}, {"audio.setDefinitionFromTempo", "#definition-from-tempo-dialog"}};
	for (const auto& [command, dialog] : dialogs)
	{
		INFO(command);
		selectEvent(app, display, file);
		run(app, command, dialog);
		newProject(app, display);
		CHECK(app.waitUntil([&app, dialog = std::string(dialog)] { return !open(app, dialog); }));
	}

	// Process Tempo is for the whole project: it stays.
	selectEvent(app, display, file);
	run(app, "tempo.openProcessTempo", "#process-tempo-dialog");
	newProject(app, display);
	CHECK(open(app, "#process-tempo-dialog"));
}

TEST_CASE("an OS file dialog's answer doesn't act on the old project's tracks after project.reset", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const test::TempDir dir;

	// Export Selected Tracks exports nothing: its tracks were the old project's.
	selectPart(app, display);
	app.keys().run("file.exportTracks", Json::object(), app.main(), nullptr);
	answerAfterNewProject(app, display, dir.path() / "Tracks.dawproject", true);
	app.shell().step(std::chrono::milliseconds(100));
	CHECK_FALSE(std::filesystem::exists(dir.path() / "Tracks.dawproject"));

	// Import Tracks imports below the track selected when the file is chosen, of the new project: none here.
	newProject(app, display);
	selectPart(app, display);
	app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
	answerAfterNewProject(app, display, dir.path() / "Other.dawproject", false);
	REQUIRE(app.waitUntil([&app] { return open(app, "#track-import-dialog"); }));
	Rml::Context& context = app.main().context();
	std::string below = "none";
	for (int i = 0; i < context.GetNumDocuments(); ++i)
		if (Rml::ElementDocument* document = context.GetDocument(i);
			document->GetElementById("track-import-dialog") != nullptr)
			below = document->GetAttribute<Rml::String>("below", "none");
	CHECK(below.empty());
}
