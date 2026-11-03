// SPDX-License-Identifier: GPL-3.0-or-later
// What the MIDI editors show (#126, docs/features/midi-editors.md): editor.get, editor.open, editor.set and
// editor.close. tests/midieditor_tests.cpp drives the editors in the app.
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	struct Fixture
	{
		Json call(const std::string& command, Json parameters = Json::object())
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			REQUIRE(!result.ok());
			return result.error;
		}

		uint64_t track(const std::string& type, uint64_t parent = 0)
		{
			Json parameters = {{"type", type}};
			if (parent != 0)
				parameters["parent"] = parent;
			return call("track.add", parameters).at("track").at("id").get<uint64_t>();
		}

		uint64_t part(uint64_t track, int64_t position)
		{
			return call("midiPart.add", {{"track", track}, {"position", position}, {"length", 1920}})
				.at("part")
				.at("id")
				.get<uint64_t>();
		}

		Json view(const std::string& editor) { return call("editor.get", {{"editor", editor}}); }

		engine::Engine engine;
		engine::Client client{engine};
	};

	std::vector<uint64_t> ids(const Json& json)
	{
		return json.get<std::vector<uint64_t>>();
	}
}

TEST_CASE("a MIDI editor opens with several parts on several tracks, in the track list's order", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "editor.changed")
				changes.push_back(event.data);
		});
	const Json closed = {{"editor", "window"}, {"open", false}, {"parts", Json::array()}, {"activePart", 0},
		{"activeTrack", 0}, {"editable", Json::array()}, {"editingMode", "allParts"}, {"keepContents", false},
		{"globalTracks", Json::array()}};
	CHECK(f.view("window") == closed);

	const uint64_t keys = f.track("midi");
	const uint64_t synth = f.track("instrument");
	const uint64_t keysLater = f.part(keys, 3840);
	const uint64_t keysFirst = f.part(keys, 0);
	const uint64_t synthPart = f.part(synth, 0);

	// The parts as the Activate Part for Editing menu lists them (p. 1143): by track, then by position.
	const Json opened = f.call("editor.open", {{"editor", "window"}, {"parts", {synthPart, keysLater, keysFirst}}});
	CHECK(opened.at("open") == true);
	CHECK(ids(opened.at("parts")) == std::vector<uint64_t>{keysFirst, keysLater, synthPart});
	CHECK(opened.at("activePart") == keysFirst);
	CHECK(opened.at("activeTrack") == keys);
	CHECK(ids(opened.at("editable")) == std::vector<uint64_t>{keysFirst, keysLater, synthPart});
	CHECK(f.view("window") == opened);
	CHECK(f.view("lower").at("open") == false); // each editor has its own
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 1);
		CHECK(changes.back() == opened);
	}

	CHECK(f.call("editor.open", {{"editor", "lower"}, {"parts", {synthPart, keysFirst}}, {"activePart", synthPart}})
			  .at("activeTrack") == synth);
	CHECK_THAT(f.fails("editor.open", {{"editor", "lower"}, {"parts", {keysFirst}}, {"activePart", synthPart}}),
		ContainsSubstring("isn't one of the parts"));
	CHECK_THAT(f.fails("editor.open", {{"editor", "lower"}, {"parts", {keys}}}), ContainsSubstring("no MIDI part"));
	CHECK_THAT(f.fails("editor.open", {{"editor", "lower"}, {"parts", Json::array()}}), ContainsSubstring("parts"));
	CHECK_THAT(f.fails("editor.open", {{"editor", "other"}, {"parts", {keysFirst}}}), ContainsSubstring("editor"));

	// Editors aren't in the project: no undo step, and the project doesn't change.
	const Json revision = f.call("project.revision");
	const Json history = f.call("history.get");
	f.call("editor.set", {{"editor", "window"}, {"activePart", synthPart}, {"editingMode", "activePart"}});
	f.call("editor.close", {{"editor", "lower"}});
	CHECK(f.call("project.revision") == revision);
	CHECK(f.call("history.get") == history);

	// Closing it lets go of the parts and of Keep Editor Contents (p. 1153); the editing mode stays.
	f.call("editor.set", {{"editor", "window"}, {"keepContents", true}, {"editingMode", "activeTrack"}});
	const Json closedAgain = f.call("editor.close", {{"editor", "window"}});
	CHECK(closedAgain.at("open") == false);
	CHECK(closedAgain.at("parts").empty());
	CHECK(closedAgain.at("keepContents") == false);
	CHECK(closedAgain.at("editingMode") == "activeTrack");
	CHECK_THAT(
		f.fails("editor.set", {{"editor", "window"}, {"activePart", keysFirst}}), ContainsSubstring("isn't open"));
}

TEST_CASE("the active part and the part editing mode decide which parts an editor edits", "[unit]")
{
	Fixture f;
	const uint64_t keys = f.track("midi");
	const uint64_t bass = f.track("midi");
	const uint64_t keys1 = f.part(keys, 0);
	const uint64_t keys2 = f.part(keys, 1920);
	const uint64_t bass1 = f.part(bass, 0);
	f.call("editor.open", {{"editor", "lower"}, {"parts", {keys1, keys2, bass1}}});

	// Part Editing Mode (pp. 1142-1143): all parts, the active part, all parts on the active track.
	Json view = f.call("editor.set", {{"editor", "lower"}, {"activePart", keys2}, {"editingMode", "activePart"}});
	CHECK(ids(view.at("editable")) == std::vector<uint64_t>{keys2});
	view = f.call("editor.set", {{"editor", "lower"}, {"editingMode", "activeTrack"}});
	CHECK(ids(view.at("editable")) == std::vector<uint64_t>{keys1, keys2});
	view = f.call("editor.set", {{"editor", "lower"}, {"activeTrack", bass}});
	CHECK(view.at("activePart") == bass1);
	CHECK(ids(view.at("editable")) == std::vector<uint64_t>{bass1});
	CHECK_THAT(f.fails("editor.set", {{"editor", "lower"}, {"editingMode", "some"}}), ContainsSubstring("editingMode"));
	CHECK_THAT(f.fails("editor.set", {{"editor", "lower"}, {"activePart", 999}}), ContainsSubstring("doesn't show"));

	// A part removed leaves the editor; the active part moves to the first one left.
	f.call("event.remove", {{"events", {bass1}}});
	view = f.view("lower");
	CHECK(ids(view.at("parts")) == std::vector<uint64_t>{keys1, keys2});
	CHECK(view.at("activePart") == keys1);
	f.call("history.undo");
	CHECK(ids(f.view("lower").at("parts")) == std::vector<uint64_t>{keys1, keys2, bass1});
}

TEST_CASE("the Visibility tab shows and hides tracks' parts in an editor", "[unit]")
{
	Fixture f;
	const uint64_t folder = f.track("folder");
	const uint64_t keys = f.track("midi", folder);
	const uint64_t synth = f.track("instrument", folder);
	const uint64_t bass = f.track("midi");
	const uint64_t empty = f.track("midi");
	const uint64_t keys1 = f.part(keys, 0);
	const uint64_t keys2 = f.part(keys, 1920);
	const uint64_t synth1 = f.part(synth, 0);
	const uint64_t bass1 = f.part(bass, 0);
	f.call("editor.open", {{"editor", "window"}, {"parts", {keys1}}});

	// Showing a track shows all its parts and activates it (p. 1151).
	Json view = f.call("editor.set", {{"editor", "window"}, {"showTracks", {bass}}});
	CHECK(ids(view.at("parts")) == std::vector<uint64_t>{keys1, bass1});
	CHECK(view.at("activeTrack") == bass);
	view = f.call("editor.set", {{"editor", "window"}, {"showTracks", {keys}}});
	CHECK(ids(view.at("parts")) == std::vector<uint64_t>{keys1, keys2, bass1});
	CHECK(view.at("activePart") == keys1);
	view = f.call("editor.set", {{"editor", "window"}, {"hideTracks", {keys}}});
	CHECK(ids(view.at("parts")) == std::vector<uint64_t>{bass1});
	// A folder shows the tracks inside; exclusively hides the others.
	view = f.call("editor.set", {{"editor", "window"}, {"showTracks", {folder}}, {"exclusive", true}});
	CHECK(ids(view.at("parts")) == std::vector<uint64_t>{keys1, keys2, synth1});
	// At least one track stays shown; tracks without parts can't show.
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"hideTracks", {folder}}}),
		ContainsSubstring("at least one track"));
	CHECK_THAT(
		f.fails("editor.set", {{"editor", "window"}, {"showTracks", {empty}}}), ContainsSubstring("has no MIDI parts"));
	CHECK(ids(f.view("window").at("parts")) == std::vector<uint64_t>{keys1, keys2, synth1}); // nothing changed
}

TEST_CASE("the editor window shows global tracks, and a new project closes the editors", "[unit]")
{
	Fixture f;
	const uint64_t keys = f.track("midi");
	const uint64_t ruler = f.track("ruler");
	const uint64_t markers = f.call("marker.add", Json::object()).at("marker").at("track").get<uint64_t>();
	const uint64_t part = f.part(keys, 0);
	f.call("editor.open", {{"editor", "window"}, {"parts", {part}}});

	// In the track list's order (p. 1145); the marker track came last, so it's below the ruler track.
	Json view = f.call("editor.set", {{"editor", "window"}, {"globalTracks", {markers, ruler}}});
	CHECK(ids(view.at("globalTracks")) == std::vector<uint64_t>{ruler, markers});
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"globalTracks", {keys}}}),
		ContainsSubstring("isn't a global track"));
	// The lower zone's editor has no global tracks (p. 1146).
	CHECK_THAT(f.fails("editor.set", {{"editor", "lower"}, {"globalTracks", {ruler}}}),
		ContainsSubstring("only the editor window"));
	f.call("track.remove", {{"track", ruler}});
	CHECK(ids(f.view("window").at("globalTracks")) == std::vector<uint64_t>{markers});

	f.call("project.new");
	view = f.view("window");
	CHECK(view.at("open") == false);
	CHECK(view.at("parts").empty());
	CHECK(view.at("globalTracks").empty());
}
