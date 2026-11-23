// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs for tracks, parts, events or notes of the project close when a new or loaded project replaces it (#1464,
// #1586): object ids are per project, so OK would act on whatever has those ids in the new project. Dialogs for the
// whole project stay open, and an OS file dialog's answer for objects of the old project is dropped (#1648). Driven
// with ui::Automation as MCP's UI tools drive them.
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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	bool open(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		if (window)
			return !app.ui().dom(window, selector, 0).at("elements").empty();
		for (const ui::Window* shown : app.shell().windows())
			if (!app.ui().dom(shown->id(), selector, 0).at("elements").empty())
				return true;
		return false;
	}

	void click(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt,
		int clicks = 1)
	{
		app.ui().click(window, {selector, 0, std::nullopt}, ui::MouseButton::Left, clicks, {});
		app.shell().step(std::chrono::milliseconds(0));
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

	// An audio track with an event of a second of silence, the track and the event selected in the project window.
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
		const std::vector<uint64_t> tracks = {track.get<uint64_t>()};
		REQUIRE(app.waitUntil(
			[&app, &tracks]
			{
				app.appUi().trackList()->select(tracks);
				return app.appUi().trackList()->selection() == tracks;
			}));
		display.select({event});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// The OS file dialog the command opened, answered with the file once the project was replaced and again made what
	// the old project had, with the same ids. The app.call after the answer waits for all the UI sent with it, as the
	// engine runs commands in the order they come: what the answer would have done is done by then.
	void answerAfterNewProject(test::UiApp& app, ui::EventDisplay& display, const std::filesystem::path& file,
		const std::function<void()>& again)
	{
		REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
		const uint64_t dialog = app.shell().fileDialogs().begin()->first;
		newProject(app, display);
		again();
		REQUIRE(app.shell().answerFileDialog(dialog, {file}));
		app.shell().step(std::chrono::milliseconds(0));
		app.call("project.get");
	}

	// A MIDI track in the new project, with the id of the old project's, selected or not.
	void addTrack(test::UiApp& app, ui::EventDisplay& display, bool select)
	{
		const auto track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
		REQUIRE(app.waitUntil(
			[&display] { return display.project().is_object() && display.project().at("tracks").size() == 1; }));
		if (select)
			app.appUi().trackList()->select({track});
	}

	void selectTrack(test::UiApp& app, uint64_t track)
	{
		REQUIRE(app.waitUntil(
			[&app, track]
			{
				app.appUi().trackList()->select({track});
				return app.appUi().trackList()->selection() == std::vector<uint64_t>{track};
			}));
	}

	size_t mediaCount(test::UiApp& app)
	{
		return app.call("project.get").at("media").size();
	}
}

TEST_CASE("dialogs for the project's tracks, parts or notes close when a new project replaces it", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const std::pair<const char*, const char*> dialogs[] = {{"project.addTrack", "#add-track-dialog"},
		{"track.addEffectToSelected", "#add-track-dialog"}, {"track.saveTrackPreset", "#track-preset-dialog"},
		{"midi.transposeSetup", "#midi-function"}, {"midi.ruleEditor", "#rule-editor"},
		{"midi.mergeInLoop", "#midi-function"}, {"track.removeSelected", "#remove-tracks-dialog"},
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
	const std::pair<const char*, const char*> dialogs[] = {{"track.autoFades", "#auto-fades"},
		{"audio.fadeEditor", "#fade-editor"}, {"audio.detectSilence", "#detect-silence-dialog"},
		{"audio.spectrumAnalyzer", "#spectrum-analyzer-dialog"}, {"audio.statistics", "#statistics-dialog"},
		{"audio.setDefinitionFromTempo", "#definition-from-tempo-dialog"}};
	for (const auto& [command, dialog] : dialogs)
	{
		INFO(command);
		selectEvent(app, display, file);
		run(app, command, dialog);
		newProject(app, display);
		CHECK(app.waitUntil([&app, dialog = std::string(dialog)] { return !open(app, dialog); }));
	}

	// The crossfade editor for two overlapping events: from and to, not events.
	const auto track = app.call("track.add", {{"type", "audio"}}).at("track").at("id");
	const Json media =
		app.call("media.add", {{"path", file.u8string()}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id");
	std::vector<uint64_t> events;
	for (const int position : {0, 36000})
		events.push_back(
			app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", position}, {"length", 48000}})
				.at("event")
				.at("id")
				.get<uint64_t>());
	app.call("event.crossfade", {{"events", events}});
	REQUIRE(app.waitUntil(
		[&display]
		{
			const Json shown = display.project().is_object() ? display.project().at("audioEvents") : Json::array();
			return std::any_of(
				shown.begin(), shown.end(), [](const Json& event) { return event.at("crossfade").is_object(); });
		}));
	display.select(events);
	run(app, "audio.crossfadeEditor", "#crossfade-editor");
	newProject(app, display);
	CHECK(app.waitUntil([&app] { return !open(app, "#crossfade-editor"); }));

	// The audio Import Options dialog: for the files and the tracks of an import.
	app.keys().run("file.importAudio", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
	REQUIRE(app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {file}));
	REQUIRE(app.waitUntil([&app] { return open(app, "#import-options-dialog"); }));
	newProject(app, display);
	CHECK(app.waitUntil([&app] { return !open(app, "#import-options-dialog"); }));

	// The zoom presets' dialogs are for the presets, not the project: they stay.
	for (const auto& [item, dialog] : {std::pair{"#zoom-preset-add", "#zoom-preset-dialog"},
			 std::pair{"#zoom-presets-organize", "#zoom-presets-dialog"}})
	{
		INFO(dialog);
		click(app, "#zoom-presets");
		click(app, item);
		REQUIRE(app.waitUntil([&app, dialog = std::string(dialog)] { return open(app, dialog); }));
		newProject(app, display);
		CHECK(open(app, dialog));
		click(app, std::string(dialog) + " button[result=cancel]");
		REQUIRE(app.waitUntil([&app, dialog = std::string(dialog)] { return !open(app, dialog); }));
	}

	// Process Tempo is for the whole project: it stays.
	selectEvent(app, display, file);
	run(app, "tempo.openProcessTempo", "#process-tempo-dialog");
	newProject(app, display);
	CHECK(open(app, "#process-tempo-dialog"));
}

TEST_CASE("an OS file dialog's answer doesn't act on the old project's objects after project.reset", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const test::TempDir dir;
	const auto exists = [&dir](const char* name) { return std::filesystem::exists(dir.path() / name); };

	// Export Selected Tracks exports nothing: its tracks were the old project's.
	selectPart(app, display);
	app.keys().run("file.exportTracks", Json::object(), app.main(), nullptr);
	answerAfterNewProject(
		app, display, dir.path() / "Tracks.dawproject", [&app, &display] { addTrack(app, display, true); });
	CHECK_FALSE(exists("Tracks.dawproject"));

	// Import Tracks imports below the track selected when the file is chosen, of the new project: none here.
	newProject(app, display);
	selectPart(app, display);
	app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
	answerAfterNewProject(
		app, display, dir.path() / "Other.dawproject", [&app, &display] { addTrack(app, display, false); });
	REQUIRE(app.waitUntil([&app] { return open(app, "#track-import-dialog"); }));
	Rml::Context& context = app.main().context();
	Rml::ElementDocument* import = nullptr;
	for (int i = 0; i < context.GetNumDocuments(); ++i)
		if (Rml::ElementDocument* document = context.GetDocument(i);
			document->GetElementById("track-import-dialog") != nullptr)
			import = document;
	REQUIRE(import != nullptr);
	CHECK(import->GetAttribute<Rml::String>("below", "none").empty());
	click(app, "#track-import-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !open(app, "#track-import-dialog"); }));

	// The track picture, chosen with a double click on the picture of the track's mixer channel, isn't set.
	newProject(app, display);
	const Json added = app.call("track.add", {{"type", "audio"}});
	const std::string picture =
		".mixer-channel[channel=" + std::to_string(added.at("channel").at("id").get<uint64_t>()) + "] .mixer-picture";
	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	const ui::WindowId mixer = app.appUi().mixerWindow(1);
	REQUIRE(app.waitUntil([&app, &picture, mixer] { return open(app, picture, mixer); }));
	click(app, "#mixer-layout", mixer);
	click(app, "#mixer-layout-pane [setup-section=picture]", mixer);
	app.ui().key(mixer, "Escape");
	REQUIRE(app.waitUntil([&app, &picture, mixer] { return open(app, picture + ":not(.hidden)", mixer); }));
	click(app, picture, mixer, 2);
	answerAfterNewProject(app, display, dir.path() / "guitar.png", [&app, &added]
		{ REQUIRE(app.call("track.add", {{"type", "audio"}}).at("track").at("id") == added.at("track").at("id")); });
	CHECK(app.call("project.get").at("tracks").at(0).at("picture").get<std::string>().empty());

	// Save Pattern Preset saves nothing for the editor's active track.
	newProject(app, display);
	const auto patterned = [&app]
	{
		const auto track = app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
		app.call("pattern.add", {{"track", track}});
		return track;
	};
	const uint64_t patternTrack = patterned();
	const Json part =
		app.call("midiPart.add", {{"track", patternTrack}, {"position", 0}, {"length", 3840}}).at("part").at("id");
	app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
	app.keys().run("pattern.savePreset", Json::object(), app.main(), nullptr);
	answerAfterNewProject(app, display, dir.path() / "Beat.patternpreset",
		[&patterned, patternTrack] { REQUIRE(patterned() == patternTrack); });
	CHECK_FALSE(exists("Beat.patternpreset"));

	// The Drum Map Setup's Save and the Articulation Map Setup's Export save nothing for the map.
	for (const auto& [add, save, file] : {std::tuple{"drumMap.add", "gnubase.ui.saveDrumMap{map = %}", "Kit.drm"},
			 std::tuple{"articulationMap.add", "gnubase.ui.exportArticulationMap{map = %, name = 'Map'}",
				 "Map.articulationmap"}})
	{
		INFO(add);
		newProject(app, display);
		const auto map = [&app, command = std::string(add)]
		{
			const Json parameters = command == "articulationMap.add" ? Json{{"name", "Map"}} : Json::object();
			return app.call(command, parameters).at("map").at("id").get<uint64_t>();
		};
		const uint64_t id = map();
		std::string code = save;
		code.replace(code.find('%'), 1, std::to_string(id));
		REQUIRE(!app.lua().run(code, "save").has_value());
		answerAfterNewProject(app, display, dir.path() / file, [&map, id] { REQUIRE(map() == id); });
		CHECK_FALSE(exists(file));
	}

	// The sampler panel's Import Audio File and the drum machine's Load Sample load nothing onto the track, or its
	// pad.
	test::Audio audio;
	audio.sampleRate = 48000;
	audio.samples.resize(4800);
	const std::filesystem::path sample = dir.path() / "tick.wav";
	const std::filesystem::path other = dir.path() / "other.wav";
	test::writeWav(sample, audio);
	test::writeWav(other, audio);
	for (const auto& [create, panel, button] : {std::tuple{"sampler.create", "window.sampler", "#sampler-import"},
			 std::tuple{"drum.create", "window.drumMachine", "#drum-load"}})
	{
		INFO(create);
		newProject(app, display);
		const auto instrument = [&app, &sample, command = std::string(create)]
		{
			const Json parameters = command == "sampler.create" ? Json{{"path", sample.u8string()}} : Json::object();
			return app.call(command, parameters).at("track").at("id").get<uint64_t>();
		};
		const uint64_t track = instrument();
		selectTrack(app, track);
		app.keys().run(panel, Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app, button = std::string(button)] { return open(app, button); }));
		click(app, button);
		size_t before = 0;
		answerAfterNewProject(app, display, other,
			[&app, &instrument, &before, track]
			{
				REQUIRE(instrument() == track);
				before = mediaCount(app);
			});
		CHECK(mediaCount(app) == before);
	}
}
