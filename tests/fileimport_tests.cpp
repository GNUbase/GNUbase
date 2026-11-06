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

	std::string writeAudio(const std::filesystem::path& path, size_t frames, int channels = 1)
	{
		test::writeWav(path, {std::vector<float>(frames * static_cast<size_t>(channels), 0.25f), channels, 48000});
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
