// SPDX-License-Identifier: GPL-3.0-or-later
// Importing audio and MIDI files in the project window (#92, docs/features/file-import.md): File > Import, the Import
// Options dialog and files dropped on the event display, in the app's UI (test::UiApp). The OS's file dialog itself
// is left out, since tests can't answer it; the event display gets what it would return. tests/audioimport_tests.cpp
// and tests/midifile_tests.cpp check the engine's imports.
#include "engine/midifile/smf.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	bool disabled(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].value("attributes", Json::object()).contains("disabled");
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool dialogShown(test::UiApp& app)
	{
		return elements(app, "#import-options-dialog").size() == 1;
	}

	std::string writeAudio(const std::filesystem::path& path, size_t frames, int channels = 1, int sampleRate = 48000)
	{
		test::writeWav(path, {std::vector<float>(frames * static_cast<size_t>(channels), 0.25f), channels, sampleRate});
		return path.u8string();
	}

	bool checked(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].value("attributes", Json::object()).contains("checked");
	}

	// File > Import > Audio File: the file dialog answered with files as the user would, then OK in the Import
	// Options dialog with Import Files on One Track and Split Channels as oneTrack and split say.
	void importAudio(test::UiApp& app, const std::vector<std::string>& files, bool oneTrack = false, bool split = false)
	{
		app.keys().run("file.importAudio", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
		std::vector<std::filesystem::path> paths;
		paths.reserve(files.size());
		for (const std::string& file : files)
			paths.push_back(std::filesystem::u8path(file));
		const bool answered = app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, std::move(paths));
		CHECK(answered);
		REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
		if (files.size() > 1 && checked(app, "#import-one-track") != oneTrack)
			click(app, "#import-one-track");
		if (!disabled(app, "#import-split") && checked(app, "#import-split") != split)
			click(app, "#import-split");
		click(app, "#import-options-dialog button.primary");
		REQUIRE(app.waitUntil([&app] { return !dialogShown(app); }));
	}

	// Two notes on channels 1 and 2: as a type 0 file in one track, as a type 1 file in a track each.
	std::string writeMidi(const std::filesystem::path& path, int format)
	{
		engine::midifile::File file;
		file.format = format;
		engine::midifile::Track first;
		first.events = {
			engine::midifile::channelEvent(0, 0x90, 60, 100), engine::midifile::channelEvent(480, 0x80, 60)};
		first.end = 480;
		engine::midifile::Track second;
		second.events = {
			engine::midifile::channelEvent(0, 0x91, 64, 100), engine::midifile::channelEvent(480, 0x81, 64)};
		second.end = 480;
		if (format == 0)
		{
			first.events.insert(first.events.begin() + 1, second.events[0]);
			first.events.push_back(second.events[1]);
			file.tracks = {first};
		}
		else
		{
			file.tracks = {first, second};
		}
		engine::midifile::save(file, path.u8string());
		return path.u8string();
	}

	// Drops files on the event lanes x and y dp from their top left corner, through the automation API (ui_drop).
	void drop(test::UiApp& app, const std::vector<std::string>& files, double x, double y)
	{
		const Json lanes = elements(app, "event-lanes.events");
		REQUIRE(lanes.size() == 1);
		const Json& box = lanes[0].at("box");
		const float ratio = app.main().context().GetDensityIndependentPixelRatio();
		const ui::Point at{box.at("x").get<int>() + static_cast<int>(std::lround(x * ratio)),
			box.at("y").get<int>() + static_cast<int>(std::lround(y * ratio))};
		std::vector<std::filesystem::path> paths;
		paths.reserve(files.size());
		for (const std::string& file : files)
			paths.push_back(std::filesystem::u8path(file));
		CHECK(!app.ui().drop(std::nullopt, {"", 0, at}, paths).at("element").is_null());
		app.shell().step(std::chrono::milliseconds(0));
	}
}

TEST_CASE("File > Import > Audio File imports through the Import Options dialog at the project cursor", "[ui]")
{
	const test::TempDir dir;
	const std::string kick = writeAudio(dir.path() / "kick.wav", 4800);
	const std::string snare = writeAudio(dir.path() / "snare.wav", 4800);
	test::UiApp app({900, 640});
	ui::EventDisplay& display = app.appUi().showProject();

	// File > Import has the items, which run while the project window shows.
	CHECK(elements(app, "#file-menu menuitem[command=\"file.importAudio\"]").size() == 1);
	CHECK(elements(app, "#file-menu menuitem[command=\"file.importMidi\"]").size() == 1);
	CHECK(app.keys().canRun("file.importAudio", app.main(), nullptr));
	CHECK(app.keys().canRun("file.importMidi", app.main(), nullptr));

	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(app.waitUntil([&display] { return display.cursor() == 1.0; }));
	display.importFiles({kick, snare}); // what the file dialog returns
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	// The project has no folder yet to copy or convert files into.
	CHECK(disabled(app, "#import-copy"));
	CHECK(disabled(app, "#import-convert"));
	CHECK(disabled(app, "#import-split"));
	CHECK(!disabled(app, "#import-one-track"));

	// Cancel imports nothing; on one track, both files go one after the other from the cursor.
	click(app, "#import-options-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !dialogShown(app); }));
	CHECK(app.call("project.get").at("audioEvents").empty());
	display.importFiles({kick, snare});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	click(app, "#import-one-track");
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 2; }));
	const Json project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("tracks")[0].at("name") == "kick");
	const time::TempoMap tempo(120.0);
	CHECK(project.at("audioEvents")[0].at("position") == tempo.toTicks(48000, 48000.0));
	CHECK(project.at("audioEvents")[1].at("position") == tempo.toTicks(52800, 48000.0));

	// The choice is remembered.
	display.importFiles({kick, snare});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(elements(app, "#import-one-track")[0].value("attributes", Json::object()).contains("checked"));
}

TEST_CASE("files dropped on the event display are imported where they are dropped", "[ui]")
{
	const test::TempDir dir;
	const std::string take = writeAudio(dir.path() / "take.wav", 4800);
	test::UiApp app({900, 640});
	const ui::EventDisplay& display = app.appUi().showProject();
	const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && lanes->lanes().size() == 1;
		}));

	// An audio file on the audio track's lane, two seconds in: an event there on that track.
	const double x = (2.0 - display.view().start) / display.view().scale;
	drop(app, {take}, x, 20.0);
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(disabled(app, "#import-one-track"));
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 1; }));
	const Json event = app.call("project.get").at("audioEvents")[0];
	CHECK(event.at("track") == track.at("id"));
	const double pixel = display.view().scale * 48000.0; // samples per dp
	CHECK(std::abs(event.at("position").get<int64_t>() - 96000) <= std::llround(pixel));

	// Two of them on the lane: both on the track, one after the other.
	drop(app, {take, take}, x, 20.0);
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(disabled(app, "#import-one-track"));
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 3; }));
	Json project = app.call("project.get");
	CHECK(project.at("tracks").size() == 1);
	CHECK(project.at("audioEvents")[1].at("track") == track.at("id"));
	CHECK(project.at("audioEvents")[2].at("track") == track.at("id"));
	CHECK(project.at("audioEvents")[2].at("position").get<int64_t>() -
			project.at("audioEvents")[1].at("position").get<int64_t>() ==
		4800);

	// MIDI files of type 0 and 1, dropped below the tracks, become MIDI tracks without a dialog: a track per channel
	// of the type 0 file, a track per track of the type 1 file.
	drop(app, {writeMidi(dir.path() / "type0.mid", 0), writeMidi(dir.path() / "type1.mid", 1)}, x, 300.0);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 5; }));
	CHECK(!dialogShown(app));
	project = app.call("project.get");
	CHECK(project.at("midiParts").size() == 4);
	for (size_t i = 1; i < 5; ++i)
		CHECK(project.at("tracks")[i].at("type") == "midi");
}

TEST_CASE("files dropped on a lane of a track whose lanes show go on that lane", "[ui]")
{
	const test::TempDir dir;
	const std::string take = writeAudio(dir.path() / "take.wav", 4800);
	test::UiApp app({900, 640});
	ui::EventDisplay& display = app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const Json media =
		app.call("media.add", {{"path", "first.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 4800}})
			.at("media");
	const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip");
	app.call("audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", 0}, {"lane", 1}});
	// A track 80 dp high, higher than the default: its lane from 0 to 80 dp, its lanes 0 and 1 below it, as high.
	display.setTrackHeight(track.get<uint64_t>(), 80.0);
	display.showLanes(track.get<uint64_t>(), true);
	REQUIRE(app.waitUntil([&display] { return display.laneTakes() == std::vector<int>{-1, 0, 1}; }));

	// On lane 1, from 160 to 240 dp: the lanes' own heights find it, not the default height's.
	const double x = (2.0 - display.view().start) / display.view().scale;
	drop(app, {take}, x, 200.0);
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 2; }));
	const Json dropped = app.call("project.get").at("audioEvents")[1];
	CHECK(dropped.at("track") == track);
	CHECK(dropped.at("lane") == 1);
}

TEST_CASE("File > Import > Audio File puts the files on the selected audio track, else on new tracks", "[ui]")
{
	const test::TempDir dir;
	const std::string kick = writeAudio(dir.path() / "kick.wav", 4800);
	const std::string snare = writeAudio(dir.path() / "snare.wav", 4800);
	test::UiApp app({900, 640});
	ui::EventDisplay& display = app.appUi().showProject();
	const auto add = [&app](const Json& parameters)
	{ return app.call("track.add", parameters).at("track").at("id").get<uint64_t>(); };
	const uint64_t keys = add({{"type", "midi"}, {"name", "Keys"}});
	const uint64_t drums = add({{"type", "audio"}, {"name", "Drums"}}); // musical
	const uint64_t bass = add({{"type", "audio"}, {"timeBase", "linear"}, {"name", "Bass"}});
	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list listitem").size() == 3; }));
	ui::TrackList* list = app.appUi().trackList();
	REQUIRE(list != nullptr);
	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(app.waitUntil([&display] { return display.cursor() == 1.0; }));
	const time::TempoMap tempo(120.0);
	const auto ticks = [&tempo](int64_t sample) { return tempo.toTicks(sample, 48000.0); };
	const auto events = [&app] { return app.call("project.get").at("audioEvents"); };
	const auto undo = [&app](size_t tracks)
	{
		app.call("history.undo");
		const Json project = app.call("project.get");
		CHECK(project.at("tracks").size() == tracks);
		CHECK(project.at("audioEvents").empty());
	};

	// An audio track selected: the first file goes on it at the project cursor, the others on new tracks there, as
	// one undo step.
	list->select({drums});
	importAudio(app, {kick, snare});
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	Json project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 4);
	CHECK(project.at("tracks")[3].at("name") == "snare");
	CHECK(project.at("audioEvents")[0].at("track") == drums);
	CHECK(project.at("audioEvents")[0].at("position") == ticks(48000));
	CHECK(project.at("audioEvents")[1].at("track") == project.at("tracks")[3].at("id"));
	CHECK(project.at("audioEvents")[1].at("position") == ticks(48000));
	undo(3);

	// With Import Files on One Track, all of them go on it, one after the other.
	importAudio(app, {kick, snare}, true);
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	project = app.call("project.get");
	CHECK(project.at("tracks").size() == 3);
	CHECK(project.at("audioEvents")[0].at("track") == drums);
	CHECK(project.at("audioEvents")[0].at("position") == ticks(48000));
	CHECK(project.at("audioEvents")[1].at("track") == drums);
	CHECK(project.at("audioEvents")[1].at("position") == ticks(52800));
	undo(3);

	// Several tracks selected: the first of them, top to bottom, takes the first file; the others go on new tracks,
	// not on the other selected tracks.
	list->select({bass, drums});
	importAudio(app, {kick, snare});
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 4);
	CHECK(project.at("audioEvents")[0].at("track") == drums);
	CHECK(project.at("audioEvents")[1].at("track") == project.at("tracks")[3].at("id"));
	undo(3);

	// The first selected track isn't an audio track: a new track, as with none selected.
	list->select({keys, bass});
	importAudio(app, {kick});
	REQUIRE(app.waitUntil([&events] { return events().size() == 1; }));
	project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 4);
	CHECK(project.at("audioEvents")[0].at("track") == project.at("tracks")[3].at("id"));
	undo(3);
	list->select({});
	importAudio(app, {kick, snare});
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 5);
	CHECK(project.at("audioEvents")[0].at("track") == project.at("tracks")[3].at("id"));
	CHECK(project.at("audioEvents")[1].at("track") == project.at("tracks")[4].at("id"));
	undo(3);

	// Split Channels puts every channel of a file with several on a new mono track of its own; a mono file still
	// goes on the selected track.
	app.call("project.save", {{"path", (dir.path() / "Song.dawproject").u8string()}, {"embedMedia", false}});
	const std::string stereo = writeAudio(dir.path() / "take.wav", 4800, 2);
	list->select({drums});
	importAudio(app, {stereo, kick}, false, true);
	REQUIRE(app.waitUntil([&events] { return events().size() == 3; }));
	project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 5);
	CHECK(project.at("audioEvents")[0].at("track") == project.at("tracks")[3].at("id"));
	CHECK(project.at("audioEvents")[1].at("track") == project.at("tracks")[4].at("id"));
	CHECK(project.at("audioEvents")[2].at("track") == drums);
	undo(3);

	// An audio track inside a folded folder stays selected and takes the file.
	const uint64_t folder = add({{"type", "folder"}, {"name", "Strings"}});
	const uint64_t violin = add({{"type", "audio"}, {"name", "Violin"}, {"parent", folder}});
	REQUIRE(app.waitUntil([&app] { return elements(app, "#track-list listitem").size() == 5; }));
	list->select({violin});
	list->setFolded(folder, true);
	REQUIRE(app.waitUntil([&display] { return display.laneTracks().size() == 4; }));
	importAudio(app, {kick});
	REQUIRE(app.waitUntil([&events] { return events().size() == 1; }));
	CHECK(events()[0].at("track") == violin);
}

TEST_CASE("Use Settings imports audio files without the Import Options dialog, with the stored options", "[ui]")
{
	const test::TempDir dir;
	const std::string kick = writeAudio(dir.path() / "kick.wav", 4800);
	const std::string snare = writeAudio(dir.path() / "snare.wav", 4800);
	test::UiApp app({900, 640});
	ui::EventDisplay& display = app.appUi().showProject();
	const auto set = [&app](const char* key, const Json& value)
	{ app.call("settings.set", {{"key", key}, {"value", value}}); };
	set("editing.onImportAudio", "useSettings");
	set("editing.importAudioOneTrack", true);
	const auto events = [&app] { return app.call("project.get").at("audioEvents"); };

	// Both files one after another on one track, without a dialog. The project has no folder yet, so the copy and
	// convert settings (on by default) don't apply.
	display.importFiles({kick, snare});
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	CHECK(!dialogShown(app));
	Json project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("audioEvents")[1].at("track") == project.at("tracks")[0].at("id"));
	CHECK(project.at("media")[0].at("path") == kick);

	// Saved: a stereo file split into files named as editing.importAudioNameFormat says, and copied.
	const std::filesystem::path folder = dir.path() / "project";
	std::filesystem::create_directories(folder);
	app.call("project.save", {{"path", (folder / "Song.dawproject").u8string()}, {"embedMedia", false}});
	set("editing.importAudioSplitChannels", true);
	set("editing.importAudioNameFormat", "dot");
	display.importFiles({writeAudio(dir.path() / "take.wav", 4800, 2), snare});
	REQUIRE(app.waitUntil([&events] { return events().size() == 5; }));
	CHECK(!dialogShown(app));
	project = app.call("project.get");
	REQUIRE(project.at("media").size() == 5);
	CHECK(project.at("media")[2].at("path") == (folder / "Audio" / "take.L.wav").u8string());
	CHECK(project.at("media")[3].at("path") == (folder / "Audio" / "take.R.wav").u8string());
	REQUIRE(app.waitUntil( // the mono file is copied by a job
		[&app, &folder]
		{ return app.call("project.get").at("media")[4].at("path") == (folder / "Audio" / "snare.wav").u8string(); }));

	// Copy and Convert off: a 44.1 kHz file from outside the project stays where it is, and no copy job starts.
	set("editing.importAudioSplitChannels", false);
	set("editing.importAudioCopy", false);
	set("editing.importAudioConvert", false);
	const std::string slow = writeAudio(dir.path() / "slow.wav", 4410, 1, 44100);
	const size_t jobs = app.call("job.list").at("jobs").size();
	display.importFiles({slow});
	REQUIRE(app.waitUntil([&events] { return events().size() == 6; }));
	Json media = app.call("project.get").at("media")[5];
	CHECK(media.at("path") == slow);
	CHECK(media.at("sampleRate") == 44100.0);
	CHECK(app.call("job.list").at("jobs").size() == jobs);

	// Convert on, Copy still off: converted into the project's Audio folder at its sample rate.
	set("editing.importAudioConvert", true);
	display.importFiles({slow});
	REQUIRE(app.waitUntil([&events] { return events().size() == 7; }));
	media = app.call("project.get").at("media")[6];
	CHECK(media.at("path") == (folder / "Audio" / "slow.wav").u8string());
	CHECK(media.at("sampleRate") == 48000.0);
	CHECK(app.call("job.list").at("jobs").size() == jobs);

	// The pool's Import Medium (clips only) and the selected track use the settings too.
	display.importFiles({kick}, std::nullopt, std::nullopt, true);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("clips").size() == 8; }));
	CHECK(!dialogShown(app));
	CHECK(events().size() == 7);
	const uint64_t selected = project.at("tracks")[0].at("id").get<uint64_t>();
	display.importToSelection({kick, snare}, {selected});
	REQUIRE(app.waitUntil([&events] { return events().size() == 9; }));
	CHECK(!dialogShown(app));
	const Json all = events();
	CHECK(all[7].at("track") == selected); // one after another on it, as editing.importAudioOneTrack says
	CHECK(all[8].at("track") == selected);

	// Back to the dialog.
	set("editing.onImportAudio", "openOptionsDialog");
	display.importFiles({kick});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
}

TEST_CASE("Do not ask again in the Import Options dialog switches to Use Settings with its choices", "[ui]")
{
	const test::TempDir dir;
	const std::string kick = writeAudio(dir.path() / "kick.wav", 4800);
	const std::string snare = writeAudio(dir.path() / "snare.wav", 4800);
	test::UiApp app({900, 640});
	ui::EventDisplay* display = &app.appUi().showProject();
	const auto setting = [&app](const char* key) { return app.call("settings.get", {{"key", key}}).at("value"); };
	const auto events = [&app] { return app.call("project.get").at("audioEvents"); };
	CHECK(setting("editing.onImportAudio") == "openOptionsDialog");

	// The dialog starts from the settings, and OK keeps the options it could choose in them; Do not ask again
	// starts unchecked.
	app.call("settings.set", {{"key", "editing.importAudioOneTrack"}, {"value", true}});
	display->importFiles({kick, snare});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(checked(app, "#import-one-track"));
	CHECK(!checked(app, "#import-do-not-ask"));
	CHECK(disabled(app, "#import-name-format")); // no project folder to split into
	click(app, "#import-one-track");
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	CHECK(setting("editing.importAudioOneTrack") == false);
	CHECK(setting("editing.importAudioCopy") == true); // disabled: as it was
	CHECK(setting("editing.onImportAudio") == "openOptionsDialog");
	CHECK(app.call("project.get").at("tracks").size() == 2);

	// Do not ask again: Use Settings with the dialog's choices, so the next import shows no dialog, also after a
	// restart.
	display->importFiles({kick, snare});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	click(app, "#import-one-track");
	click(app, "#import-do-not-ask");
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&events] { return events().size() == 4; }));
	CHECK(setting("editing.onImportAudio") == "useSettings");
	CHECK(setting("editing.importAudioOneTrack") == true);
	app.restart();
	display = &app.appUi().showProject();
	display->importFiles({kick, snare});
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	CHECK(!dialogShown(app));
	CHECK(app.call("project.get").at("tracks").size() == 1);

	// Resetting the setting brings the dialog back.
	app.call("settings.reset", {{"key", "editing.onImportAudio"}});
	display->importFiles({kick});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
}

TEST_CASE("The Import Options dialog shows and keeps Copy, Convert and Split Channels in the settings", "[ui]")
{
	const test::TempDir dir;
	const std::string slow = writeAudio(dir.path() / "slow.wav", 4410, 2, 44100);
	test::UiApp app({900, 640});
	ui::EventDisplay& display = app.appUi().showProject();
	const std::filesystem::path folder = dir.path() / "project";
	std::filesystem::create_directories(folder);
	app.call("project.save", {{"path", (folder / "Song.dawproject").u8string()}, {"embedMedia", false}});
	const auto set = [&app](const char* key, const Json& value)
	{ app.call("settings.set", {{"key", key}, {"value", value}}); };
	const auto setting = [&app](const char* key) { return app.call("settings.get", {{"key", key}}).at("value"); };
	const auto events = [&app] { return app.call("project.get").at("audioEvents"); };
	set("editing.importAudioCopy", false);
	set("editing.importAudioConvert", false);
	set("editing.importAudioSplitChannels", true);

	// The dialog opens with the stored options; the user turns each one around and checks Do not ask again.
	display.importFiles({slow});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(!checked(app, "#import-copy"));
	CHECK(!checked(app, "#import-convert"));
	CHECK(checked(app, "#import-split"));
	click(app, "#import-copy");
	click(app, "#import-convert");
	click(app, "#import-split");
	click(app, "#import-do-not-ask");
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&events] { return events().size() == 1; }));
	CHECK(setting("editing.importAudioCopy") == true);
	CHECK(setting("editing.importAudioConvert") == true);
	CHECK(setting("editing.importAudioSplitChannels") == false);
	CHECK(setting("editing.onImportAudio") == "useSettings");

	// The next import runs without the dialog with them: converted, not split.
	display.importFiles({slow});
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));
	CHECK(!dialogShown(app));
	const Json project = app.call("project.get");
	REQUIRE(project.at("media").size() == 2);
	CHECK(project.at("media")[1].at("channels") == 2);
	CHECK(project.at("media")[1].at("sampleRate") == 48000.0);
	CHECK(project.at("media")[1].at("path") == (folder / "Audio" / "slow-2.wav").u8string());
}

TEST_CASE("The Import Options dialog's Name Format names the files Split Channels writes", "[ui]")
{
	struct Format
	{
		const char* value;
		int down; // presses of Down from the first option
		const char* left;
	};
	const Format format = GENERATE(Format{"dash", 0, "take-L.wav"}, Format{"underscore", 1, "take_L.wav"},
		Format{"dot", 2, "take.L.wav"}, Format{"space", 3, "take L.wav"});
	INFO(format.value);
	const test::TempDir dir;
	const std::string take = writeAudio(dir.path() / "take.wav", 4800, 2);
	test::UiApp app({900, 640});
	ui::EventDisplay& display = app.appUi().showProject();
	const std::filesystem::path folder = dir.path() / "project";
	std::filesystem::create_directories(folder);
	app.call("project.save", {{"path", (folder / "Song.dawproject").u8string()}, {"embedMedia", false}});

	display.importFiles({take});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(!disabled(app, "#import-name-format"));
	if (!checked(app, "#import-split"))
		click(app, "#import-split");
	click(app, "#import-name-format");
	for (int i = 0; i < format.down; ++i)
		app.ui().key(std::nullopt, "Down");
	click(app, "#import-name-format");
	click(app, "#import-options-dialog button.primary");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 2; }));
	const Json project = app.call("project.get");
	CHECK(project.at("media")[0].at("path") == (folder / "Audio" / format.left).u8string());
	CHECK(app.call("settings.get", {{"key", "editing.importAudioNameFormat"}}).at("value") == format.value);

	// The dialog shows it the next time.
	display.importFiles({take});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	const Json select = elements(app, "#import-name-format");
	REQUIRE(select.size() == 1);
	CHECK(select[0].value("attributes", Json::object()).value("value", "") == format.value);
}

TEST_CASE("The preferences' Editing > Audio page sets On Import Audio Files and what Use Settings imports with", "[ui]")
{
	const test::TempDir dir;
	const std::string take = writeAudio(dir.path() / "take.wav", 4800, 2);
	test::UiApp app({1000, 760});
	ui::EventDisplay& display = app.appUi().showProject();
	const std::filesystem::path folder = dir.path() / "project";
	std::filesystem::create_directories(folder);
	app.call("project.save", {{"path", (folder / "Song.dawproject").u8string()}, {"embedMedia", false}});
	const auto setting = [&app](const char* key) { return app.call("settings.get", {{"key", key}}).at("value"); };
	const auto value = [&app](const std::string& selector)
	{
		const Json found = elements(app, selector);
		REQUIRE(found.size() == 1);
		return found[0].at("value");
	};
	// Opens a drop-down list, moves the selection that many options down (up when negative) and closes it.
	const auto choose = [&app](const std::string& selector, int down)
	{
		click(app, selector);
		for (int i = 0; i < std::abs(down); ++i)
			app.ui().key(std::nullopt, down > 0 ? "Down" : "Up");
		click(app, selector);
	};
	const auto openPreferences = [&app]
	{
		app.keys().run("window.preferences", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return elements(app, "#preferences").size() == 1; }));
		app.shell().step(std::chrono::milliseconds(0));
		click(app, "#pages-editing-audio");
	};
	const auto ok = [&app]
	{
		click(app, "#preferences-ok");
		REQUIRE(app.waitUntil([&app] { return elements(app, "#preferences").empty(); }));
	};
	app.call("settings.set", {{"key", "editing.onImportAudio"}, {"value", "useSettings"}}); // as Do not ask again

	// The page shows the settings; Split Channels on, Name Format "Name L" and Copy off.
	openPreferences();
	CHECK(value("#pref-editing-onImportAudio") == "useSettings");
	CHECK(checked(app, "#pref-editing-importAudioCopy"));
	CHECK(checked(app, "#pref-editing-importAudioConvert"));
	CHECK(!checked(app, "#pref-editing-importAudioSplitChannels"));
	CHECK(value("#pref-editing-importAudioNameFormat") == "dash");
	click(app, "#pref-editing-importAudioSplitChannels");
	choose("#pref-editing-importAudioNameFormat", 3);
	click(app, "#pref-editing-importAudioCopy");
	ok();
	CHECK(setting("editing.importAudioSplitChannels") == true);
	CHECK(setting("editing.importAudioNameFormat") == "space");
	CHECK(setting("editing.importAudioCopy") == false);

	// Use Settings imports with them, without the dialog.
	display.importFiles({take});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 2; }));
	CHECK(!dialogShown(app));
	CHECK(app.call("project.get").at("media")[0].at("path") == (folder / "Audio" / "take L.wav").u8string());

	// Split Channels writes its files into Audio whatever Copy says, so Copy is checked with a mono file, which
	// isn't split: with Copy off it keeps its original path and no copy job starts (with Copy on it would be copied
	// into Audio by a job).
	const std::string mono = writeAudio(dir.path() / "mono.wav", 4800);
	const size_t jobs = app.call("job.list").at("jobs").size();
	display.importFiles({mono});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("audioEvents").size() == 3; }));
	CHECK(!dialogShown(app));
	CHECK(app.call("project.get").at("media")[2].at("path") == mono);
	CHECK(app.call("job.list").at("jobs").size() == jobs);

	// Open Options Dialog resets Do not ask again: the dialog opens, starting from the same settings.
	openPreferences();
	choose("#pref-editing-onImportAudio", -1);
	ok();
	CHECK(setting("editing.onImportAudio") == "openOptionsDialog");
	display.importFiles({take});
	REQUIRE(app.waitUntil([&app] { return dialogShown(app); }));
	CHECK(checked(app, "#import-split"));
	CHECK(!checked(app, "#import-copy"));
	CHECK(elements(app, "#import-name-format")[0].value("attributes", Json::object()).value("value", "") == "space");
}
